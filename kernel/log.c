
// 文件系统实现:
//  + FS.img: 文件系统映像 (mkfs.c)
//  + Dev+blockno: 虚拟硬盘块设备 (virtio_disk.c)
//  + Bcache: 缓存链环 (bio.c)
//  + Log: 多步更新的崩溃恢复 (log.c)
//  + Inodes: inode分配器, 读取, 写入, 元数据 (fs.c)
//  + Directories: 具有特殊内容的inode(其他inode的列表) (fs.c)
//  + PathNames: 方便命名的路径, 如 /usr/rtm/xv6/fs.c (fs.c)

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// https://www.cnblogs.com/KatyuMarisaBlog/p/14385792.html
// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
    int n;
    int block[LOGSIZE];
};

struct log {
    struct spinlock lock;
    int start;
    int size;
    int outstanding;  // how many FS sys calls are executing.
    int committing;   // in commit(), please wait.
    int dev;
    struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();

// 初始化日志 (fs.c->fsinit)
void initlog(int dev, struct superblock* sb) {
    // logheader结构体大小不能超过一个块
    if (sizeof(struct logheader) >= BSIZE)
        panic("initlog: too big logheader");

    initlock(&log.lock, "log");  // 初始化日志锁
    log.start = sb->logstart;    // 第一个日志块的块号
    log.size = sb->nlog;         // 日志块数量
    log.dev = dev;
    recover_from_log();
}

// Copy committed blocks from log to their home location
static void install_trans(int recovering) {
    int tail;

    for (tail = 0; tail < log.lh.n; tail++) {
        struct buf* lbuf = bread(log.dev, log.start + tail + 1);  // read log block
        struct buf* dbuf = bread(log.dev, log.lh.block[tail]);    // read dst
        memmove(dbuf->data, lbuf->data, BSIZE);                   // copy block to dst
        bwrite(dbuf);                                             // write dst to disk
        if (recovering == 0)
            bunpin(dbuf);
        brelse(lbuf);
        brelse(dbuf);
    }
}

// Read the log header from disk into the in-memory log header
static void read_head(void) {
    struct buf* buf = bread(log.dev, log.start);
    struct logheader* lh = (struct logheader*)(buf->data);
    int i;
    log.lh.n = lh->n;
    for (i = 0; i < log.lh.n; i++) {
        log.lh.block[i] = lh->block[i];
    }
    brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
static void write_head(void) {
    struct buf* buf = bread(log.dev, log.start);
    struct logheader* hb = (struct logheader*)(buf->data);
    int i;
    hb->n = log.lh.n;
    for (i = 0; i < log.lh.n; i++) {
        hb->block[i] = log.lh.block[i];
    }
    bwrite(buf);  // 写入硬盘
    brelse(buf);
}

static void recover_from_log(void) {
    read_head();
    install_trans(1);  // if committed, copy from log to disk
    log.lh.n = 0;
    write_head();  // clear the log
}

// 在每个文件系统调用的开始部分被调用
void begin_op(void) {
    acquire(&log.lock);  // 获取日志锁
    while (1) {
        if (log.committing) {
            sleep(&log, &log.lock);  // 释放锁并等待commit, 醒来后重新获取锁
        } else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS > LOGSIZE) {
            sleep(&log, &log.lock);  // 当前操作可能导致日志空间溢出, 等待commit
        } else {
            log.outstanding += 1;  // 日志计数器+1
            release(&log.lock);    // 释放日志锁
            break;
        }
    }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void end_op(void) {
    int do_commit = 0;

    acquire(&log.lock);    // 获取日志锁
    log.outstanding -= 1;  // 当前操作完成, 日志计数器-1
    if (log.committing)
        panic("log.committing");
    if (log.outstanding == 0) {
        do_commit = 1;
        log.committing = 1;
    } else {
        // begin_op() may be waiting for log space,
        // and decrementing log.outstanding has decreased
        // the amount of reserved space.
        wakeup(&log);
    }
    release(&log.lock);

    if (do_commit) {
        // call commit w/o holding locks, since not allowed
        // to sleep with locks.
        commit();
        acquire(&log.lock);
        log.committing = 0;
        wakeup(&log);
        release(&log.lock);
    }
}

// Copy modified blocks from cache to log.
static void write_log(void) {
    int tail;

    for (tail = 0; tail < log.lh.n; tail++) {
        struct buf* to = bread(log.dev, log.start + tail + 1);  // log block
        struct buf* from = bread(log.dev, log.lh.block[tail]);  // cache block
        memmove(to->data, from->data, BSIZE);
        bwrite(to);  // 将日志块写入硬盘
        brelse(from);
        brelse(to);
    }
}

static void commit() {
    if (log.lh.n > 0) {
        write_log();       // Write modified blocks from cache to log
        write_head();      // Write header to disk -- the real commit
        install_trans(0);  // Now install writes to home locations
        log.lh.n = 0;
        write_head();  // Erase the transaction from the log
    }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
void log_write(struct buf* b) {
    int i;

    acquire(&log.lock);
    if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
        panic("too big a transaction");
    if (log.outstanding < 1)
        panic("log_write outside of trans");

    for (i = 0; i < log.lh.n; i++) {
        if (log.lh.block[i] == b->blockno)  // log absorption
            break;
    }
    log.lh.block[i] = b->blockno;
    if (i == log.lh.n) {  // Add new block to log?
        bpin(b);
        log.lh.n++;
    }
    release(&log.lock);
}
