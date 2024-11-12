
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

// bread: 锁定硬盘块到缓存
// bwrite: 将缓存块写回硬盘
// brelse: 释放缓存块

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "buf.h"
#include "fs.h"

// 缓存块链环 (LRU)
// head.next是最近使用的
// head.prev是最久未用的
struct {
    struct spinlock lock; // 缓存锁
    struct buf buf[NBUF]; // 缓存块数组
    struct buf head;      // 缓存链环头结点
} bcache;

// 初始化缓存链环
void binit(void)
{
    initlock(&bcache.lock, "bcache");

    // 初始化头结点自环
    bcache.head.prev = &bcache.head;
    bcache.head.next = &bcache.head;

    // 插入所有缓存块到链环
    // head-->buf[NBUF-1...0]-->head
    for (struct buf* b = bcache.buf; b < bcache.buf + NBUF; b++) {
        b->prev = &bcache.head;
        b->next = bcache.head.next;
        initsleeplock(&b->lock, "buffer");
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }
}

// 锁定硬盘块到缓存
struct buf* bread(uint dev, uint blockno)
{
    buf* b;

    acquire(&bcache.lock); //* 获取缓存锁

    // 遍历缓存链环 寻找对应缓存块
    for (b = bcache.head.next; b != &bcache.head; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;           // 增加引用计数
            release(&bcache.lock); //* 释放缓存锁

            acquiresleep(&b->lock); //** 获取块锁 (休眠)
            break;
        }
    }

    // 如果没找到 则从尾部找闲置LRU缓存
    if (b == &bcache.head)
        for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
            if (b->refcnt == 0) {
                b->dev = dev;          // 设备号
                b->blockno = blockno;  // 对应块号
                b->refcnt = 1;         // 引用计数 (1)
                release(&bcache.lock); //* 释放缓存锁

                acquiresleep(&b->lock);   //** 获取块锁 (休眠)
                virtio_disk_rw(b, false); // 从硬盘加载数据
                break;
            }
        }

    if (b == &bcache.head)
        panic("bget: no buffers");
    return b;
}

// 将缓存块写回硬盘
void bwrite(struct buf* b)
{
    // 确保当前进程持有块锁
    if (holdingsleep(&b->lock) == false)
        panic("bwrite");
    virtio_disk_rw(b, true);
}

// 释放缓存块
void brelse(struct buf* b)
{
    // 确保当前进程持有块锁
    if (holdingsleep(&b->lock) == false)
        panic("brelse");

    releasesleep(&b->lock); //** 释放块锁 (唤醒)

    acquire(&bcache.lock); //* 获取缓存锁

    // 减少引用计数
    b->refcnt--;

    // 如果引用清零, 则将缓存块移到队首
    if (b->refcnt == 0) {
        b->next->prev = b->prev;
        b->prev->next = b->next;
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }

    release(&bcache.lock); //* 释放缓存锁
}

// 增加缓存链块的引用
void bpin(struct buf* b)
{
    acquire(&bcache.lock); //* 获取缓存锁
    b->refcnt++;
    release(&bcache.lock); //* 释放缓存锁
}

// 减少缓存链块的引用
void bunpin(struct buf* b)
{
    acquire(&bcache.lock); //* 获取缓存锁
    b->refcnt--;
    release(&bcache.lock); //* 释放缓存锁
}
