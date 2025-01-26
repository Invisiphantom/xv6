
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
#include "proc.h"
#include "buf.h"
#include "stat.h"
#include "fs.h"
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
}

// 释放硬盘块
static void bfree(uint dev, uint bno)
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

// 找到空闲的硬盘-索引项 (需要事务)
// 返回对应的内存-索引项 (暂时不加载数据到内存)
minode* ialloc(uint dev, short type)
{
    // 遍历所有的硬盘-索引项
    for (uint inum = ROOTINO; inum < sb.ninodes; inum++) {
        buf* bp = bread(dev, IBLOCK(inum, sb)); //* 锁定索引块
        dinode* dip = (dinode*)bp->data + inum % IPB;

        // 如果找到空闲的硬盘-索引项
        // TODO: 需要初始化清空inode硬盘块
        if (dip->type == I_FREE) {
            memset(dip, 0, sizeof(*dip)); // 清空数据
            dip->type = type;             // 索引类型
            log_write(bp);                // 日志写回
            brelse(bp);                   //* 释放索引块

            // 索引项条目: 硬盘=>内存 (暂时不加载数据)
            minode* mip = iget(dev, inum);
            return mip;
        }

        brelse(bp); //* 释放索引块
    }

    panic("ialloc: no inodes");
}

// 索引项条目: 硬盘=>内存 (暂时不加载数据 增加引用)
static minode* iget(uint dev, uint inum)
{
    minode *mip, *empty = NULL;
    acquire(&itable.lock); //* 获取索引表锁

    // 遍历内存-索引表 寻找对应索引项
    for (mip = &itable.inode[0]; mip < &itable.inode[NINODE]; mip++) {
        // 如果索引项已在内存, 则增加引用计数
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

// 锁定内存-索引项 (加载数据到内存)
void ilock(minode* mip)
{
    if (mip == NULL || mip->ref <= 0)
        panic("ilock");

    acquiresleep(&mip->lock); //** 获取inode锁 (休眠)

    // 如果索引无效, 则从硬盘加载
    if (mip->valid == false) {
        buf* bp = bread(mip->dev, IBLOCK(mip->inum, sb)); //* 锁定索引块
        dinode* dip = (struct dinode*)bp->data + mip->inum % IPB;

        // 索引项数据: 内存<==硬盘
        mip->type = dip->type;   // 索引类型 (stat.h)
        mip->major = dip->major; // 主设备号
        mip->minor = dip->minor; // 次设备号
        mip->nlink = dip->nlink; // 硬链接数
        mip->size = dip->size;   // 文件大小 (字节)
        memmove(mip->addrs, dip->addrs, sizeof(mip->addrs));

        brelse(bp); //* 释放索引块

        mip->valid = true;
        if (mip->type == I_FREE)
            panic("ilock: no type");
    }
}

// 将更新索引项写回硬盘 (需持有inode锁)
void iupdate(minode* mip)
{
    buf* bp = bread(mip->dev, IBLOCK(mip->inum, sb)); //* 锁定索引块
    dinode* dip = (dinode*)bp->data + mip->inum % IPB;

    // 索引项数据: 硬盘<==内存
    dip->type = mip->type;   // 索引类型 (stat.h)
    dip->major = mip->major; // 主设备号
    dip->minor = mip->minor; // 次设备号
    dip->nlink = mip->nlink; // 硬链接数
    dip->size = mip->size;   // 文件大小 (字节)
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

    // 释放最后一个引用, 并且没有硬链接
    if (mip->ref == 1 && mip->valid && mip->nlink == 0) {
        // 此时只有当前进程在引用, 因此获取锁不会阻塞
        acquiresleep(&mip->lock); //** 获取inode锁 (休眠)
        release(&itable.lock);    //* 释放索引表锁

        itrunc(mip);        // 清除文件块
        mip->type = I_FREE; // 标记为空闲
        iupdate(mip);       // 将更新索引项写回硬盘
        mip->valid = false; // 标记索引无效

        releasesleep(&mip->lock); //** 释放inode锁 (唤醒)
        acquire(&itable.lock);    //* 获取索引表锁
    }

    mip->ref--;            // 减少引用计数
    release(&itable.lock); //* 释放索引表锁
}

// 释放锁 并减少索引项的引用
void iunlockput(minode* mip)
{
    iunlock(mip); //** 释放inode锁 (唤醒)
    iput(mip);
}

// ----------------------------------------------------------------

// 获取文件块 (需持有inode锁)
static uint bmap(minode* mip, uint addri)
{
    uint addr;

    // 寻找直接块
    if (addri < NDIRECT) {
        addr = mip->addrs[addri]; // 获取直接块
        if (addr == 0) {
            addr = balloc(mip->dev);  // 分配直接块
            mip->addrs[addri] = addr; // 记录直接块
        }
        return addr;
    }

    // 寻找间接块
    addri -= NDIRECT;
    if (addri < NINDIRECT) {
        addr = mip->addrs[NDIRECT]; // 获取间接引导块
        if (addr == 0) {
            addr = balloc(mip->dev);    // 分配间接引导块
            mip->addrs[NDIRECT] = addr; // 记录间接引导块
        }

        buf* bp = bread(mip->dev, addr); //* 锁定间接引导块
        uint* in_addrs = (uint*)bp->data;

        addr = in_addrs[addri]; // 获取间接块
        if (addr == 0) {
            addr = balloc(mip->dev); // 分配间接块
            in_addrs[addri] = addr;  // 记录间接块
            log_write(bp);           // 写回间接引导块
        }

        brelse(bp); //* 释放间接引导块
        return addr;
    }

    panic("bmap: out of range");
}

// 清除文件块 (需持有inode锁)
void itrunc(minode* mip)
{
    // 清除直接块
    for (uint i = 0; i < NDIRECT; i++) {
        if (mip->addrs[i]) {
            bfree(mip->dev, mip->addrs[i]);
            mip->addrs[i] = 0;
        }
    }

    // 清除间接块
    if (mip->addrs[NDIRECT]) {
        buf* bp = bread(mip->dev, mip->addrs[NDIRECT]); //* 锁定间接引导块
        uint* in_addrs = (uint*)bp->data;

        for (uint j = 0; j < NINDIRECT; j++)
            if (in_addrs[j])
                bfree(mip->dev, in_addrs[j]);

        brelse(bp); //* 释放间接引导块

        // 清除间接引导块
        bfree(mip->dev, mip->addrs[NDIRECT]);
        mip->addrs[NDIRECT] = 0;
    }

    mip->size = 0; // 清零文件大小
    iupdate(mip);  // 将更新索引项写回硬盘
}

// ----------------------------------------------------------------

// 索引信息: stat<==minode
void stati(minode* mip, struct stat* st)
{
    st->dev = mip->dev;     // 设备号
    st->inum = mip->inum;   // 索引编号
    st->type = mip->type;   // 索引类型
    st->nlink = mip->nlink; // 硬链接数
    st->size = mip->size;   // 文件大小 (字节)
}

// ----------------------------------------------------------------

// 读取文件内容 (需持有inode锁)
int readi(minode* mip, int user_dst, uint64 dst, uint off, uint n)
{
    // 确保偏移量和长度合法
    if (off >= mip->size)
        return 0;

    // 截断读取长度到文件范围内
    if (off + n > mip->size)
        n = mip->size - off;

    uint total;
    for (total = 0; total < n;) {
        uint addr = bmap(mip, off / BSIZE); // 获取文件块
        buf* bp = bread(mip->dev, addr);    //* 锁定文件块

        // 块内最大读取长度
        uint max_len = min(n - total, BSIZE - off % BSIZE);

        // 文件内容: dst<==文件块
        if (either_copyout(user_dst, dst, bp->data + (off % BSIZE), max_len) == -1) {
            brelse(bp); //* 释放文件块
            return -1;
        }

        brelse(bp); //* 释放文件块
        total += max_len;
        off += max_len;
        dst += max_len;
    }

    return total; // 返回实际读取长度
}

// 写入文件内容 (需持有inode锁)
int writei(minode* mip, int user_src, uint64 src, uint off, uint n)
{
    // 确保偏移量在文件范围内 并且不会溢出
    if (off > mip->size || off + n < off)
        return -1;

    // 确保写入总长度不超过 允许的文件最大长度
    if (off + n > MAXFILE * BSIZE)
        return -1;

    uint total;
    for (total = 0; total < n;) {
        uint addr = bmap(mip, off / BSIZE); // 获取文件块
        buf* bp = bread(mip->dev, addr);    //* 锁定文件块

        // 块内最大写入长度
        uint max_len = min(n - total, BSIZE - off % BSIZE);

        // 文件内容: 文件块<==src
        if (either_copyin(bp->data + (off % BSIZE), user_src, src, max_len) == -1) {
            brelse(bp);
            break;
        }

        log_write(bp); // 写回日志
        brelse(bp);    //* 释放文件块
        total += max_len;
        off += max_len;
        src += max_len;
    }

    // 更新文件大小
    if (off > mip->size)
        mip->size = off;

    iupdate(mip); // 将更新索引项写回硬盘
    return total; // 返回实际写入长度
}

// -------------------------------- Dir -------------------------------- //

int namecmp(const char* s1, const char* s2) { return strncmp(s1, s2, DIRSIZ); }

// 在目录中查找文件项
minode* dirlookup(minode* dp, char* name, uint* poff)
{
    // 确保索引类型是目录
    if (dp->type != I_DIR)
        panic("dirlookup not DIR");

    // 遍历目录下的所有文件项
    for (uint off = 0; off < dp->size; off += sizeof(dirent)) {
        dirent de; // 读取文件项
        if (readi(dp, false, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlookup read");

        // 跳过空文件项
        if (de.inum == 0)
            continue;

        // 如果找到文件
        if (namecmp(name, de.name) == 0) {
            if (poff != NULL)
                *poff = off;                      // 记录偏移量
            minode* mip = iget(dp->dev, de.inum); // 获取索引项 (加引用)
            return mip;
        }
    }

    // 没有找到文件
    return NULL;
}

// 向目录中添加文件项
int dirlink(minode* dp, char* name, uint inum)
{
    minode* mip; // 确保此时目录中不存在此文件项
    if ((mip = dirlookup(dp, name, 0)) != NULL) {
        iput(mip); // 减少引用计数
        return -1;
    }

    uint off;
    dirent de;

    // 寻找空文件项 如果没找到 则off=dp->size
    for (off = 0; off < dp->size; off += sizeof(dirent)) {
        if (readi(dp, false, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlink read");
        if (de.inum == 0)
            break;
    }

    de.inum = inum;                 // 索引编号
    strncpy(de.name, name, DIRSIZ); // 文件名称

    // 写入文件项
    if (writei(dp, false, (uint64)&de, off, sizeof(de)) != sizeof(de))
        return -1;

    return 0;
}

// -------------------------------- Path -------------------------------- //

// 获取路径的首元素 返回剩余路径
// skipelem("a/bb/c")="bb/c" --> name="a"
static char* skipelem(char* path, char* name)
{
    // 跳过前缀的斜线
    while (*path == '/')
        path++;

    // 如果路径为空
    if (*path == 0)
        return NULL;

    // 记录路径起始位置
    char* s = path;

    // 提取首元素
    while (*path != '/' && *path != 0)
        path++;

    // 记录首元素的长度
    int len = path - s;

    // 确保文件名不超过最大长度
    if (len >= DIRSIZ)
        memmove(name, s, DIRSIZ);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }

    // 跳过后续的斜线
    while (*path == '/')
        path++;
    return path;
}

// 获取路径对应的索引项
// parent=0 : 返回路径文件索引项
// parent=1 : 返回其父目录索引项
static minode* namex(char* path, int parent, char* name)
{
    minode* mip;
    if (*path == '/')
        mip = iget(ROOTDEV, ROOTINO); // 绝对路径 (加引用)
    else
        mip = idup(myproc()->cwd); // 相对路径 (加引用)

    // 逐级查找路径经过的目录
    while ((path = skipelem(path, name)) != NULL) {
        ilock(mip); //* 锁定索引项 (休眠)

        // 确保索引类型是目录
        if (mip->type != I_DIR) {
            iunlockput(mip); //* 释放索引项 (唤醒 减引用)
            return 0;
        }

        // 如果需要 则直接返回父目录
        if (parent && *path == '\0') {
            iunlock(mip); //* 释放索引项 (唤醒)
            return mip;
        }

        // 查找下一级路径元素
        minode* next = dirlookup(mip, name, 0);
        if (next == 0) {
            iunlockput(mip); //* 释放索引项 (唤醒 减引用)
            return 0;
        }

        iunlockput(mip); //* 释放索引项 (唤醒 减引用)
        mip = next;
    }

    if (parent)
        panic("namex");

    // 返回路径对应项
    return mip;
}

// 获取路径对应项 (封装namex)
minode* namei(char* path)
{
    char name[DIRSIZ];
    return namex(path, false, name);
}

// 获取路径对应项的父目录 (封装namex)
minode* nameiparent(char* path, char* name) { return namex(path, true, name); }
