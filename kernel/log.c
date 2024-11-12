
// 文件系统实现:
//  + UART: 串口输入输出 (printf.c console.c uart.c)
//  -------------------------------------------------
//  + FS.img: 文件系统映像 (mkfs.c)
//  + VirtIO: 虚拟硬盘驱动 (virtio.h virtio_disk.c)
//  + BCache: LRU缓存链环 (buf.h bio.c)
//  + Log: 两步提交的日志系统 (log.c)
//  + Inode Dir Path: 硬盘文件系统实现 (stat.h fs.h fs.c)
//  + Pipe: 管道实现 (pipe.c)
//  + File Descriptor: 文件描述符 (file.h file.c)
//  + File SysCall: 文件系统调用 (fcntl.h sysfile.c)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [      0     |      1      | 2       31 | 32        44 |      45      | 46     1999 ]

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "buf.h"
#include "fs.h"

// 允许并发文件系统调用的简单日志系统
// https://www.cnblogs.com/KatyuMarisaBlog/p/14385792.html

// begin_op: 开始文件事务
// end_op: 结束文件事务

struct logheader {
    int n;                  // 当前记录的日志数量 (<LOGSIZE)
    int block[LOGSIZE - 1]; // 记录映射的目标块号 [0,28]->[3,31]
};

struct log {
    struct spinlock lock; // 自旋锁
    int start;            // 日志头块的块号 (2)
    int outstanding;      // 当前文件操作数量
    int committing;       // 是否正在执行提交
    int dev;              // 设备编号
    struct logheader lh;  // 日志头
} log;

static void recover_from_log(void);
static void commit();

// 初始化日志系统 (fs.c->fsinit)
void initlog(int dev, struct superblock* sb)
{
    // 确保记录结构体不会太大
    if (sizeof(struct logheader) >= BSIZE)
        panic("initlog: too big logheader");

    initlock(&log.lock, "log"); // 初始化日志锁
    log.start = sb->logstart;   // 日志头块的块号 (2)
    log.dev = dev;              // 所属设备号

    // 恢复上次停机时的日志块
    recover_from_log();
}

// 硬盘: 日志块=>目标块
static void install_trans(int recovering)
{
    // 遍历当前已记录的日志块
    for (int tail = 0; tail < log.lh.n; tail++) {
        //* 锁定日志块和目标块
        struct buf* lbuf = bread(log.dev, (log.start + 1) + tail);
        struct buf* dbuf = bread(log.dev, log.lh.block[tail]);

        // 日志快->目标块 并写回
        memmove(dbuf->data, lbuf->data, BSIZE);
        bwrite(dbuf);

        //** 如果不是重启恢复, 则减少引用
        if (recovering == false)
            bunpin(dbuf);

        //* 释放日志块和目标块
        brelse(lbuf);
        brelse(dbuf);
    }
}

// 日志头: 硬盘=>内存
static void read_head(void)
{
    //* 锁定日志头块
    struct buf* lh_buf = bread(log.dev, log.start);
    struct logheader* lh = (struct logheader*)(lh_buf->data);

    // 更新内存-日志头
    log.lh.n = lh->n;
    for (int i = 0; i < log.lh.n; i++)
        log.lh.block[i] = lh->block[i];

    //* 释放日志头块
    brelse(lh_buf);
}

// 日志头: 内存=>硬盘 (事务提交)
static void write_head(void)
{
    //* 锁定日志头块
    struct buf* lh_buf = bread(log.dev, log.start);
    struct logheader* lh = (struct logheader*)(lh_buf->data);

    // 更新硬盘-日志头
    lh->n = log.lh.n;
    for (int i = 0; i < log.lh.n; i++)
        lh->block[i] = log.lh.block[i];

    // 写回硬盘
    bwrite(lh_buf);

    //* 释放日志头块
    brelse(lh_buf);
}

static void recover_from_log(void)
{
    read_head();         // 日志头: 硬盘=>内存
    install_trans(true); // 硬盘: 日志块=>目标块 (恢复模式)
    log.lh.n = 0;        // 清空内存-日志数
    write_head();        // 日志头: 内存=>硬盘 (事务提交)
}

// ----------------------------------------------------------------

// 内存-目标块=>硬盘-日志块
static void write_log(void)
{
    // 遍历当前正在使用的日志块
    for (int tail = 0; tail < log.lh.n; tail++) {
        //* 锁定日志块和目标块
        struct buf* lbuf = bread(log.dev, (log.start + 1) + tail);
        struct buf* dbuf = bread(log.dev, log.lh.block[tail]);

        // 目标块->日志块 并写回
        memmove(lbuf->data, dbuf->data, BSIZE);
        bwrite(lbuf);

        //* 释放日志块和目标块
        brelse(dbuf);
        brelse(lbuf);
    }
}

static void commit()
{
    // 先移动数据, 再更新日志头
    if (log.lh.n > 0) {
        write_log();  // 内存-目标块=>硬盘-日志块
        write_head(); // 日志头: 内存=>硬盘 (事务提交)

        install_trans(false); // 硬盘: 日志块=>目标块
        log.lh.n = 0;         // 清空内存-日志数
        write_head();         // 日志头: 内存=>硬盘 (事务提交)
    }
}

// 开始文件事务
void begin_op(void)
{
    acquire(&log.lock); //* 获取日志锁
    while (1) {
        //* 等待提交完成
        if (log.committing == true)
            sleep(&log, &log.lock);

        //* 等待日志空间
        else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS >= LOGSIZE)
            sleep(&log, &log.lock);

        else {
            log.outstanding++;  // 增加当前文件操作数
            release(&log.lock); //* 释放日志锁
            break;
        }
    }
}

// 结束文件事务
void end_op(void)
{
    int do_commit = false;

    acquire(&log.lock); //* 获取日志锁

    // 减少当前文件操作数
    log.outstanding--;

    // 确保现在没有提交
    if (log.committing == true)
        panic("end_op: log is committing");

    // 唤醒等待日志空间的begin_op
    if (log.outstanding != 0)
        wakeup(&log);
    else {
        // 最后操作, 执行日志提交
        do_commit = true;
        log.committing = true;
    }

    release(&log.lock); //* 释放日志锁

    if (do_commit == true) {
        commit(); // (内存-目标块)=>(硬盘-日志块)=>(硬盘-目标块)

        acquire(&log.lock);     //* 获取日志锁
        log.committing = false; // 关闭提交标志
        wakeup(&log);           // 唤醒等待提交完成的begin_op
        release(&log.lock);     //* 释放日志锁
    }
}

// ----------------------------------------------------------------

// 用log_write来代替直接写入硬盘的bwrite
void log_write(struct buf* b)
{
    acquire(&log.lock); //* 获取日志锁

    // 确保不会超出日志 且处于事务中
    if (log.lh.n + 1 >= LOGSIZE)
        panic("log_write: transaction too big");
    if (log.outstanding < 1)
        panic("log_write: outside of trans");

    // 遍历检查是否在日志
    int havelog = false;
    for (int i = 0; i < log.lh.n; i++)
        if (log.lh.block[i] == b->blockno) {
            havelog = true;
            break;
        }

    // 如果不在日志, 则添加
    if (havelog == false) {
        bpin(b); // 增加引用
        log.lh.block[log.lh.n] = b->blockno;
        log.lh.n++;
    }

    release(&log.lock); //* 释放日志锁
}
