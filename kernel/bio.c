
// 文件系统实现:
//  + FS.img: 文件系统映像 (mkfs.c)
//  + Dev+blockno: 虚拟硬盘块设备 (virtio_disk.c)
//  + Bcache: 缓存链环 (bio.c)
//  + Log: 多步更新的崩溃恢复 (log.c)
//  + Inode: inode分配器, 读取, 写入, 元数据 (fs.c)
//  + Directory: 具有特殊内容的inode(其他inode的列表) (fs.c)
//  + Path: 方便命名的路径, 如 /usr/rtm/xv6/fs.c (fs.c)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [          0 |           1 | 2       31 | 32        44 |           45 | 46     1999 ]

// buffer cache是buf结构体链表, 用于缓存硬盘块内容
// 在内存中缓存硬盘块减少了硬盘读取次数, 也为多个进程使用的硬盘块提供了同步点

// 同时只有一个进程可以使用一个缓存
// 所以不要超过必要保持缓存的时间

// bread: 读取硬盘块内容到缓存
// bwrite: 将缓存内容写回硬盘
// brelse: 释放缓存 (不要在调用brelse后使用缓存)

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// 缓存链环, 通过prev/next连接
// 按照最近使用的顺序排序
// head.next是最新使用的, head.prev是最旧使用的
struct {
    struct spinlock lock;
    struct buf buf[NBUF];
    struct buf head;
} bcache;

void binit(void)
{
    struct buf* b;

    initlock(&bcache.lock, "bcache"); // 初始化缓存锁

    // head自环
    bcache.head.prev = &bcache.head;
    bcache.head.next = &bcache.head;

    // 在环中插入所有缓存
    // head->buf[NBUF-1]->...->buf[0]->head
    for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        initsleeplock(&b->lock, "buffer"); // 为每个缓存块初始化锁
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }
}

// 查找缓存链环 是否已缓存设备dev的块blockno
// 如果未找到, 则分配一个空缓存
// 无论哪种情况, 返回锁定的缓存
static struct buf* bget(uint dev, uint blockno)
{
    struct buf* b;

    acquire(&bcache.lock); // 获取缓存锁

    // 从链环头部开始 查找该硬盘块是否已有缓存
    for (b = bcache.head.next; b != &bcache.head; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            release(&bcache.lock);  // 释放缓存锁
            acquiresleep(&b->lock); // 获取块锁
            return b;
        }
    }

    // 如果没有缓存, 则从缓存链环尾部找到一个闲置的LRU缓存
    for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
        if (b->refcnt == 0) {
            b->dev = dev;           // 设备号
            b->blockno = blockno;   // 块号
            b->valid = 0;           // 有效位:0
            b->refcnt = 1;          // 引用计数:1
            release(&bcache.lock);  // 释放缓存锁
            acquiresleep(&b->lock); // 获取块锁
            return b;
        }
    }
    panic("bget: no buffers");
}

// 从设备dev读取块blockno, 并返回锁定的buf
struct buf* bread(uint dev, uint blockno)
{
    struct buf* b;

    // 获取blockno对应的缓存
    b = bget(dev, blockno);

    // 如果是新的无效缓存, 则从硬盘读取
    if (!b->valid) {
        virtio_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

// 将缓冲链块b的内容写入硬盘 (必须已被锁定)
void bwrite(struct buf* b)
{
    if (!holdingsleep(&b->lock))
        panic("bwrite");
    virtio_disk_rw(b, 1);
}

// 将已锁定的缓存 释放锁
// 并将移动到LRU列表的头部
void brelse(struct buf* b)
{
    // 如果未锁定, 则panic
    if (!holdingsleep(&b->lock))
        panic("brelse");

    releasesleep(&b->lock); // 释放块锁

    acquire(&bcache.lock); // 获取缓存锁

    b->refcnt--; // 引用减1

    if (b->refcnt == 0) {
        // 如果引用为0, 则将缓存移动到LRU列表的头部
        b->next->prev = b->prev;
        b->prev->next = b->next;
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }

    release(&bcache.lock);
}

// 增加缓存链块的引用
void bpin(struct buf* b)
{
    acquire(&bcache.lock);
    b->refcnt++;
    release(&bcache.lock);
}

// 减少缓存链块的引用
void bunpin(struct buf* b)
{
    acquire(&bcache.lock);
    b->refcnt--;
    release(&bcache.lock);
}
