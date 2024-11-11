
// 文件系统实现:
//  + UART: 串口输入输出 (printf.c console.c uart.c)
//  -------------------------------------------------
//  + FS.img: 文件系统映像 (mkfs.c)
//  + VirtIO: 虚拟硬盘驱动 (virtio.h virtio_disk.c)
//  + BCache: LRU缓存链环 (buf.h bio.c)
//  + Log: 两步提交的日志系统 (log.c)
//  + Inode Dir Path: 硬盘文件系统实现 (stat.h fs.h fs.c)
//  + File SysCall: 文件系统调用 (file.h file.c pipe.c sysfile.c)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [      0     |      1      | 2       31 | 32        44 |      45      | 46     1999 ]

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

superblock sb;

// 超级块: 硬盘=>内存
static void readsb(int dev, superblock* sb)
{
    buf* bp = bread(dev, 1);            //* 锁定超级块
    memmove(sb, bp->data, sizeof(*sb)); // 拷贝数据
    brelse(bp);                         //* 释放超级块
}

// 初始化文件系统 (proc.c->forkret)
void fsinit(int dev)
{
    // 加载内存-超级块
    readsb(dev, &sb);

    // 校验文件系统魔数
    if (sb.magic != FSMAGIC)
        panic("invalid file system");

    // 初始化日志系统
    initlog(dev, &sb);
}

// -------------------------------- Block -------------------------------- //

// 分配硬盘块 返回块号
static uint balloc(uint dev)
{
    // 遍历所有的位图分区
    for (uint part = 0; part < sb.size; part += BPB) {
        buf* bp = bread(dev, BBLOCK(part, sb)); //* 锁定位图块

        // 遍历该分区的所有位
        for (uint bi = 0; bi < BPB && part + bi < sb.size; bi++) {
            uchar mask = 1 << (bi % 8);

            // 如果当前位是空闲的
            if ((bp->data[bi / 8] & mask) == 0) {
                bp->data[bi / 8] |= mask; // 标记此位
                log_write(bp);            // 写回日志
                brelse(bp);               //* 释放位图块

                // 分配新块并清空数据
                uint bno = part + bi;
                bp = bread(dev, bno);       //** 锁定分配块
                memset(bp->data, 0, BSIZE); // 清空数据
                log_write(bp);              // 写回日志
                brelse(bp);                 //** 释放分配块
                return bno;
            }
        }
        brelse(bp); //* 释放位图块
    }

    panic("balloc: out of blocks");
    return 0;
}

// 释放硬盘块
static void bfree(int dev, uint bno)
{
    uint bi = bno % BPB;
    uchar mask = 1 << (bi % 8);
    buf* bp = bread(dev, BBLOCK(bno, sb)); //* 读取位图块

    // 确保此块已分配
    if ((bp->data[bi / 8] & mask) == 0)
        panic("freeing free block");

    bp->data[bi / 8] &= ~mask; // 清除标记
    log_write(bp);             // 写回日志
    brelse(bp);                //* 释放位图块
}

// -------------------------------- Inode -------------------------------- //

// 内存-索引表
struct {
    spinlock lock;        // 索引表锁
    minode inode[NINODE]; // 内存-索引项数组
} itable;

void iinit()
{
    initlock(&itable.lock, "itable");
    for (int i = 0; i < NINODE; i++)
        initsleeplock(&itable.inode[i].lock, "inode");
}

// ----------------------------------------------------------------

static minode* iget(uint dev, uint inum);

// 找到空闲的硬盘-索引项
// 返回对应的内存-索引项 (暂时不加载数据)
minode* ialloc(uint dev, short type)
{
    // 遍历所有的硬盘-索引项
    for (uint inum = ROOTINO; inum < sb.ninodes; inum++) {
        buf* bp = bread(dev, IBLOCK(inum, sb)); //* 锁定索引块
        dinode* dip = (dinode*)bp->data + inum % IPB;

        // 如果找到空闲的硬盘-索引项
        if (dip->type == T_FREE) {
            memset(dip, 0, sizeof(*dip)); // 清空数据
            dip->type = type;             // 文件类型
            log_write(bp);                // 写回日志
            brelse(bp);                   //* 释放索引块

            // 索引项条目: 硬盘=>内存 (暂时不加载数据)
            minode* mip = iget(dev, inum);
            return mip;
        }

        brelse(bp); //* 释放索引块
    }

    panic("ialloc: no inodes");
    return 0;
}

// 索引项条目: 硬盘=>内存 (暂时不加载数据)
static minode* iget(uint dev, uint inum)
{
    minode *mip, *empty = NULL;
    acquire(&itable.lock); //* 获取索引表锁

    // 遍历内存-索引表 寻找对应索引项
    for (mip = &itable.inode[0]; mip < &itable.inode[NINODE]; mip++) {
        if (mip->ref > 0 && mip->dev == dev && mip->inum == inum) {
            mip->ref++;            // 增加引用计数
            release(&itable.lock); //* 释放索引表锁
            return mip;
        }

        // 记录空闲索引项
        if (empty == NULL && mip->ref == 0)
            empty = mip;
    }

    if (empty == NULL)
        panic("iget: no inodes");

    mip = empty;        // 空闲索引项
    mip->dev = dev;     // 设备号
    mip->inum = inum;   // 索引编号
    mip->ref = 1;       // 引用计数
    mip->valid = false; // 有效位

    release(&itable.lock); //* 释放索引表锁
    return mip;
}

// ----------------------------------------------------------------

// 锁定内存-索引项
void ilock(minode* mip)
{
    if (mip == NULL || mip->ref <= 0)
        panic("ilock");

    acquiresleep(&mip->lock); //** 获取inode锁 (休眠)

    // 如果索引无效, 则从硬盘加载
    if (mip->valid == false) {
        buf* bp = bread(mip->dev, IBLOCK(mip->inum, sb)); //* 锁定索引块
        dinode* dip = (struct dinode*)bp->data + mip->inum % IPB;

        // 索引项数据: 硬盘=>内存
        mip->type = dip->type;
        mip->major = dip->major;
        mip->minor = dip->minor;
        mip->nlink = dip->nlink;
        mip->size = dip->size;
        memmove(mip->addrs, dip->addrs, sizeof(mip->addrs));

        brelse(bp); //* 释放索引块

        mip->valid = true;
        if (mip->type == T_FREE)
            panic("ilock: no type");
    }
}

// 更新索引项写回硬盘 (需持有inode锁)
void iupdate(minode* mip)
{
    buf* bp = bread(mip->dev, IBLOCK(mip->inum, sb)); //* 锁定索引块
    dinode* dip = (dinode*)bp->data + mip->inum % IPB;

    // 索引项数据: 内存=>硬盘
    dip->type = mip->type;
    dip->major = mip->major;
    dip->minor = mip->minor;
    dip->nlink = mip->nlink;
    dip->size = mip->size;
    memmove(dip->addrs, mip->addrs, sizeof(mip->addrs));

    log_write(bp); // 写回日志
    brelse(bp);    //* 释放索引块
}

// 释放内存-索引项
void iunlock(minode* mip)
{
    // 确保当前进程持有inode锁
    if (mip == NULL || holdingsleep(&mip->lock) == false || mip->ref <= 0)
        panic("iunlock");
    releasesleep(&mip->lock); //** 释放inode锁 (唤醒)
}

// ----------------------------------------------------------------

// 增加内存-索引项的引用计数
minode* idup(minode* mip)
{
    acquire(&itable.lock); //* 获取索引表锁
    mip->ref++;
    release(&itable.lock); //* 释放索引表锁
    return mip;
}

// 减少内存-索引项的引用计数
void iput(minode* mip)
{
    acquire(&itable.lock); //* 获取索引表锁

    // 如果是最后一个引用, 并且没有硬链接
    if (mip->ref == 1 && mip->valid && mip->nlink == 0) {
        // mip->ref == 1 意味着没有其他进程可以锁定ip
        // 因此这个acquiresleep()不会被阻塞(或死锁)
        acquiresleep(&mip->lock); //** 获取inode锁 (休眠)
        release(&itable.lock);    //* 释放索引表锁

        itrunc(mip);        // 释放minode的所有硬盘数据块
        mip->type = 0;      // 将minode标记为空闲
        iupdate(mip);       // 将更新后的minode写回硬盘
        mip->valid = false; // 标记内存minode失效

        releasesleep(&mip->lock); //** 释放inode锁 (唤醒)
        acquire(&itable.lock);    //* 获取索引表锁
    }

    mip->ref--;            // 减少引用计数
    release(&itable.lock); //* 释放索引表锁
}

// 释放minode锁 并释放minode表项及数据块
void iunlockput(minode* mip)
{
    iunlock(mip);
    iput(mip);
}

// -------------------------------- Inode Content -------------------------------- //

// 返回 inode 第 addri 个块的硬盘地址
// 如果没有这样的块，则分配一个新块
static uint bmap(minode* mip, uint addri)
{
    uint addr;

    // 寻找直接块
    if (addri < NDIRECT) {
        if ((addr = mip->addrs[addri]) == 0) {
            addr = balloc(mip->dev); // 分配新直接块
            if (addr == 0)
                return 0;
            mip->addrs[addri] = addr;
        }
        return addr;
    }

    // 寻找间接块
    addri -= NDIRECT;
    if (addri < NINDIRECT) {
        if ((addr = mip->addrs[NDIRECT]) == 0) {
            addr = balloc(mip->dev); // 分配新间接引导块
            if (addr == 0)
                return 0;
            mip->addrs[NDIRECT] = addr;
        }
        buf* bp = bread(mip->dev, addr); // 读取间接引导块
        uint* a = (uint*)bp->data;
        if ((addr = a[addri]) == 0) {
            addr = balloc(mip->dev); // 分配新间接块
            if (addr) {
                a[addri] = addr;
                log_write(bp); // 更新写回间接引导块
            }
        }
        brelse(bp);
        return addr;
    }

    panic("bmap: out of range");
}

// 释放 minode 的所有硬盘数据块
// 调用者必须持有ip->lock
void itrunc(minode* mip)
{
    // 释放所有直接块
    for (int i = 0; i < NDIRECT; i++) {
        if (mip->addrs[i]) {
            bfree(mip->dev, mip->addrs[i]);
            mip->addrs[i] = 0;
        }
    }

    // 释放所有间接块
    if (mip->addrs[NDIRECT]) {
        buf* bp = bread(mip->dev, mip->addrs[NDIRECT]);
        uint* a = (uint*)bp->data;
        // 遍历所有间接块 并释放
        for (int j = 0; j < NINDIRECT; j++)
            if (a[j])
                bfree(mip->dev, a[j]);
        brelse(bp);
        bfree(mip->dev, mip->addrs[NDIRECT]);
        mip->addrs[NDIRECT] = 0;
    }

    mip->size = 0; // 更新inode大小
    iupdate(mip);  // 将更新后的minode写回硬盘
}

// 从inode复制stat信息至st
// 调用者必须持有ip->lock
void stati(minode* mip, struct stat* st)
{
    st->dev = mip->dev;
    st->ino = mip->inum;
    st->type = mip->type;
    st->nlink = mip->nlink;
    st->size = mip->size;
}

// 读取文件的n个字节到dst
// 返回成功读取的字节数
// (调用者必须持有ip->lock)
// user_dst=1 : dst是用户地址
// user_dst=0 : dst是内核地址
int readi(minode* mip, int user_dst, uint64 dst, uint off, uint n)
{
    uint total, max_len;

    // 确保偏移量在文件范围内
    if (off >= mip->size || n <= 0)
        return 0;

    // 如果总长度超过文件总大小, 则截断
    if (off + n > mip->size)
        n = mip->size - off;

    for (total = 0; total < n; total += max_len, off += max_len, dst += max_len) {
        // 返回 off 所处硬盘块的地址 (保证偏移量在范围内, 不会扩展文件)
        uint addr = bmap(mip, off / BSIZE);
        if (addr == 0)
            break;

        // 读取 off 所处硬盘块
        buf* bp = bread(mip->dev, addr);

        // 计算可以读取的字节数
        max_len = min(n - total, BSIZE - off % BSIZE);

        // 将硬盘块数据拷贝到dst[max_len]
        if (either_copyout(user_dst, dst, bp->data + (off % BSIZE), max_len) == -1) {
            brelse(bp);
            total = -1;
            break;
        }

        brelse(bp); // 释放硬盘块缓存
    }

    return total;
}

// user_dst=1 : dst是用户虚拟地址
// user_dst=0 : dst是内核地址
// 将src[n]数据写入inode[off, off+n], 返回成功写入的字节数
// 如果写入超过文件大小, 则自动扩展并更新文件大小
int writei(minode* mip, int user_src, uint64 src, uint off, uint n)
{
    uint total, max_len;

    // 确保偏移量在文件范围内 并且不会溢出
    if (off > mip->size || off + n < off)
        return -1;

    // 确保写入总长度不超过 允许的文件最大长度
    if (off + n > MAXFILE * BSIZE)
        return -1;

    for (total = 0; total < n; total += max_len, off += max_len, src += max_len) {
        // 返回 off 所处硬盘块的地址 (超出部分 自动扩展文件)
        uint addr = bmap(mip, off / BSIZE);
        if (addr == 0)
            break;

        // 读取 off 所处硬盘块
        buf* bp = bread(mip->dev, addr);

        // 计算可以写入的字节数
        max_len = min(n - total, BSIZE - off % BSIZE);

        // 将dst[max_len]数据写入到硬盘块
        if (either_copyin(bp->data + (off % BSIZE), user_src, src, max_len) == -1) {
            brelse(bp);
            break;
        }

        log_write(bp); // 写回硬盘块 (日志)
        brelse(bp);    // 释放硬盘块缓存
    }

    // 如果写入超过文件大小, 则更新
    if (off > mip->size)
        mip->size = off;

    // mip->size可能更新, bmap可能更新了ip->addrs[]
    // 将修改后的内存minode 更新到 硬盘dinode
    iupdate(mip);

    return total;
}

// -------------------------------- Directory -------------------------------- //

// 比较两个目录项的名称是否相同
int namecmp(const char* s, const char* t) { return strncmp(s, t, DIRSIZ); }

// 在指定目录dp中查找给定名称name的目录项
// 如果找到该目录项，则将其字节偏移量存储在 *poff 中，并返回 minode
minode* dirlookup(minode* dp, char* name, uint* poff)
{
    uint inum;
    struct dirent de;

    // 确保dp是一个目录
    if (dp->type != T_DIR)
        panic("dirlookup not DIR");

    // 遍历目录下的所有项
    for (uint off = 0; off < dp->size; off += sizeof(de)) {
        // 读取dp[off, off+n]的数据到de[n]
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlookup read");

        // 跳过空目录项
        if (de.inum == 0)
            continue;

        // 如果找到匹配的目录项
        if (namecmp(name, de.name) == 0) {
            if (poff)
                *poff = off;            // 记录在目录内部的偏移量
            inum = de.inum;             // 记录inode编号
            return iget(dp->dev, inum); // 返回inum对应的minode
        }
    }

    return 0;
}

// 在指定的目录 dp 中写入新的目录项{inum,name}
int dirlink(minode* dp, char* name, uint inum)
{
    int off;
    struct dirent de;
    minode* mip;

    // 确保文件name在目录dp中
    if ((mip = dirlookup(dp, name, 0)) != NULL) {
        iput(mip);
        return -1;
    }

    // 寻找一个空的目录项
    for (off = 0; off < dp->size; off += sizeof(de)) {
        // 读取dp[off, off+n]的数据到de[n]
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlink read");
        // 找到了空的目录项
        if (de.inum == 0)
            break;
    }

    de.inum = inum;                 // 更新de.inum
    strncpy(de.name, name, DIRSIZ); // 更新de.name

    // 将更新后的目录项de写回到dp[off, off+n] (如果超出文件大小, 则自动扩展)
    if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        return -1;

    return 0;
}

// -------------------------------- Path -------------------------------- //

// 拆分路径path, 将第一个元素复制到name中, 并返回剩余的路径
// - skipelem("a/bb/c", name) = "bb/c" --> name = "a"
// - skipelem("///a//bb", name) = "bb" --> name = "a"
// - skipelem("a", name) = ""          --> name = "a"
// - skipelem("", name) = skipelem("////", name) = 0
static char* skipelem(char* path, char* name)
{
    // 跳过前缀的'/'
    while (*path == '/')
        path++;

    // 如果path为空, 则返回0
    if (*path == 0)
        return 0;

    // 记录路径的起始位置
    char* s = path;

    // 提取第一个元素
    while (*path != '/' && *path != 0)
        path++;

    // 记录第一个元素的长度
    int len = path - s;

    // 如果其名称长度大于DIRSIZ, 则截断
    if (len >= DIRSIZ)
        memmove(name, s, DIRSIZ);

    // 否则直接复制到name中
    else {
        memmove(name, s, len);
        name[len] = 0;
    }

    // 跳过后续的'/'
    while (*path == '/')
        path++;
    return path;
}

// 查找path对应的路径inode
// nameiparent=0 : 返回路径对应inode
// nameiparent=1 : 返回其父目录inode
// 并将最后的路径元素复制到name中, name必须有DIRSIZ字节的空间
// 必须在transaction中调用, 因为它调用了iput()
static minode* namex(char* path, int nameiparent, char* name)
{
    minode *mip, *next;

    if (*path == '/')
        mip = iget(ROOTDEV, ROOTINO); // 直接返回根目录inode
    else
        mip = idup(myproc()->cwd); // 增加对当前工作目录的引用

    // 逐级查找目录
    while ((path = skipelem(path, name)) != 0) {
        // 锁定当前目录inode
        ilock(mip);
        // 确保cwd是一个目录类型
        if (mip->type != T_DIR) {
            iunlockput(mip);
            return 0;
        }
        // 提前一次循环, 返回父目录inode
        if (nameiparent && *path == '\0') {
            iunlock(mip);
            return mip;
        }
        // 在当前目录ip中查找name对应项 并记为next
        if ((next = dirlookup(mip, name, 0)) == 0) {
            iunlockput(mip);
            return 0;
        }
        // 释放当前目录inode, 继续处理next
        iunlockput(mip);
        mip = next;
    }

    // 确保需要返回父目录时, 不会到达这里
    if (nameiparent) {
        iput(mip);
        return 0;
    }

    // 返回path对应的inode
    return mip;
}

// 获取path对应的inode (封装namex)
minode* namei(char* path)
{
    char name[DIRSIZ];
    return namex(path, 0, name);
}

// 获取path对应的inode的父目录inode (封装namex)
minode* nameiparent(char* path, char* name) { return namex(path, 1, name); }
