
// 文件系统实现:
//  + FS.img: 文件系统映像 (mkfs.c)
//  + Dev+blockno: 虚拟硬盘块设备 (virtio_disk.c)
//  + Bcache: 缓存链环 (bio.c)
//  + Log: 多步更新的崩溃恢复 (log.c)
//  + Inodes: inode分配器, 读取, 写入, 元数据 (fs.c)
//  + Directories: 具有特殊内容的inode(其他inode的列表) (fs.c)
//  + PathNames: 方便命名的路径, 如 /usr/rtm/xv6/fs.c (fs.c)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [          0 |           1 | 2       31 | 32        44 |           45 | 46     1999 ]

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// 允许并发文件系统调用的简单日志系统
// https://www.cnblogs.com/KatyuMarisaBlog/p/14385792.html

// 一个日志事务包含多个文件系统系统调用的更新
// 日志系统仅在没有活跃的文件系统系统调用时进行提交
// 因此, 不需要考虑 是否误将未提交的系统调用的更新写入硬盘

// 文件系统调用通过 begin_op() / end_op() 来标记其开始和结束
// 通常 begin_op() 只是增加正在进行的文件系统系统调用的计数并返回
// 但是, 如果它认为日志快要用完, 它会等待其余未完成的 end_op() 

// 硬盘块布局:
// Boot Block
// Super Block
// log.start  ->Head Block
// log.start+1->Log Block 1
// log.start+2->Log Block 2
// ...
// log.start+LOGSIZE-1 -> Log Block N-1
// Inode Blocks ...


struct logheader {
    int n;               // 日志数据块的数量
    int block[LOGSIZE];  // 每个日志数据快 对应的 目标块编号
};

struct log {
    struct spinlock lock;  // 自旋锁
    int start;             // 日志头块的块号
    int size;              // 日志总块数
    int outstanding;       // 当前文件调用量
    int committing;        // 是否正在执行提交
    int dev;               // 设备编号
    struct logheader lh;   // 日志头
};
struct log log;

static void recover_from_log(void);
static void commit();

// 初始化日志系统 (fs.c->fsinit)
void initlog(int dev, struct superblock* sb) {
    // logheader结构体大小不能超过一个块
    if (sizeof(struct logheader) >= BSIZE)
        panic("initlog: too big logheader");

    initlock(&log.lock, "log");  // 初始化日志锁
    log.start = sb->logstart;    // 日志头块的块号
    log.size = sb->nlog;         // 日志块总数
    log.dev = dev;               // 所属设备号

    recover_from_log();  // 恢复已有日志
}

// 将所有已提交的日志块 写入到它们的目标硬盘位置
static void install_trans(int recovering) {
    for (int tail = 0; tail < log.lh.n; tail++) {
        struct buf* lbuf = bread(log.dev, log.start + tail + 1);  // 读取日志数据块
        struct buf* dbuf = bread(log.dev, log.lh.block[tail]);    // 读取目标块

        memmove(dbuf->data, lbuf->data, BSIZE);  // 拷贝数据
        bwrite(dbuf);                            // 写回目标块

        // 恢复模式需要继续保持已有的缓存链块
        if (recovering == 0)  // 如果不是恢复模式
            bunpin(dbuf);     // 减少缓存链块的引用

        brelse(lbuf);
        brelse(dbuf);
    }
}

// 从硬盘读取日志头, 并更新内存中的对应数据
static void read_head(void) {
    // 从设备dev读取日志头块start, 并返回锁定的buf
    struct buf* buf = bread(log.dev, log.start);
    // 获取日志头数据
    struct logheader* lh = (struct logheader*)(buf->data);
    // 更新内存中的日志数据块数量 (硬盘->内存)
    log.lh.n = lh->n;
    // 更新内存中的日志数据块编号 (硬盘->内存)
    for (int i = 0; i < log.lh.n; i++)
        log.lh.block[i] = lh->block[i];
    // 释放缓存链块
    brelse(buf);
}

// 将内存中的日志头写入硬盘
// 这是当前事务提交的真正点
static void write_head(void) {
    // 从设备dev读取块start, 并返回锁定的buf
    struct buf* buf = bread(log.dev, log.start);
    // 获取日志头数据
    struct logheader* lh = (struct logheader*)(buf->data);
    // 更新硬盘中的日志数据块数量 (内存->硬盘)
    lh->n = log.lh.n;
    // 更新硬盘中的日志数据块编号 (内存->硬盘)
    for (int i = 0; i < log.lh.n; i++)
        lh->block[i] = log.lh.block[i];
    // 写入硬盘
    bwrite(buf);
    // 释放缓存链块
    brelse(buf);
}

static void recover_from_log(void) {
    read_head();       // 从硬盘读取日志头, 并更新内存中的对应数据
    install_trans(1);  // 将所有已提交的日志块 写入到它们的目标硬盘位置 (恢复模式)
    log.lh.n = 0;      // 内存中的日志块数量清零
    write_head();      // 将内存中的日志头写入硬盘
}

// 在每个文件系统调用的开始部分被调用
void begin_op(void) {
    acquire(&log.lock);  // 获取日志锁
    while (1) {
        // 如果当前有提交操作, 则等待
        if (log.committing)
            sleep(&log, &log.lock);
        // 如果当前操作可能导致日志溢出, 则等待
        else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS > LOGSIZE)
            sleep(&log, &log.lock);
        // 增加当前操作数, 并释放日志锁
        else {
            log.outstanding += 1;  // 日志计数器+1
            release(&log.lock);    // 释放日志锁
            break;
        }
    }
}

// 在每个文件系统调用的结束部分被调用
// 在最后一个操作结束时执行提交
void end_op(void) {
    acquire(&log.lock);  // 获取日志锁

    // 当前操作数-1
    log.outstanding -= 1;

    // 确保现在没有正在提交的操作
    if (log.committing)
        panic("log.committing");

    // 如果是最后一个操作, 则执行提交
    int do_commit = 0;
    if (log.outstanding == 0) {
        do_commit = 1;
        log.committing = 1;
    }
    // 如果不是最后一个操作, 则唤醒等待进程
    else
        wakeup(&log);  // begin_op() 可能有操作等待日志空间

    release(&log.lock);  // 释放日志锁

    if (do_commit) {
        commit();  // 提交所有的日志

        acquire(&log.lock);  // 获取日志锁
        log.committing = 0;  // 关闭提交标志
        wakeup(&log);        // begin_op() 可能有操作等待提交完成
        release(&log.lock);  // 释放日志锁
    }
}

// 则更新每个日志块, 使其与对应的目标块(缓存)相同
static void write_log(void) {
    for (int tail = 0; tail < log.lh.n; tail++) {
        struct buf* to = bread(log.dev, log.start + tail + 1);  // 日志数据块
        struct buf* from = bread(log.dev, log.lh.block[tail]);  // 对应的目标块(缓存)

        memmove(to->data, from->data, BSIZE);  // 拷贝数据
        bwrite(to);                            // 将日志块写入硬盘

        brelse(from);
        brelse(to);
    }
}

// 提交所有的日志
static void commit() {
    if (log.lh.n > 0) {
        write_log();   // 则更新每个日志块, 使其与对应的目标块(缓存)相同
        write_head();  // 将内存中的日志头写入硬盘

        install_trans(0);  // 将所有已提交的日志块 写入到它们的目标硬盘位置

        log.lh.n = 0;  // 清空日志数
        write_head();  // 将内存中的日志头写入硬盘
    }
}

// 调用者修改了b->data, 并结束了对缓冲区使用
// 记录块号并通过增加refcnt将其固定在缓存中
// commit()/write_log()将执行硬盘写入

// 用log_write来代替直接写入硬盘的bwrite
void log_write(struct buf* b) {
    int i;

    acquire(&log.lock);  // 获取日志锁

    // 确保未超过日志大小
    if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
        panic("too big a transaction");
    // 确保当前在事务中
    if (log.outstanding < 1)
        panic("log_write outside of trans");

    // 如果该块已经在日志中, 则不需要再次添加
    for (i = 0; i < log.lh.n; i++)
        if (log.lh.block[i] == b->blockno)
            break;

    // 记录块号到第i个日志块
    log.lh.block[i] = b->blockno;

    // 如果该块不在日志中, 则添加到日志中
    if (i == log.lh.n) {
        bpin(b);     // 增加缓存链块的引用
        log.lh.n++;  // 日志块数量+1
    }

    release(&log.lock);  // 释放日志锁
}
