
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

// 清空硬盘块 (fs.c->balloc)
static void bzero(uint dev, uint bno)
{
    buf* bp = bread(dev, bno);  //* 锁定块
    memset(bp->data, 0, BSIZE); // 清空数据
    log_write(bp);              // 写回日志
    brelse(bp);                 //* 释放块
}

// 分配硬盘块 返回块号
static uint balloc(uint dev)
{
    // 遍历所有的位图分区
    for (uint bp_part = 0; bp_part < sb.size; bp_part += BPB) {
        buf* bp = bread(dev, BBLOCK(bp_part, sb)); //* 锁定位图块

        // 遍历该分区的所有块
        for (uint bi = 0; bi < BPB && bp_part + bi < sb.size; bi++) {
            uchar mask = 1 << (bi % 8);

            // 如果当前位是空闲的
            if ((bp->data[bi / 8] & mask) == 0) {
                bp->data[bi / 8] |= mask; // 标记此位
                log_write(bp);            // 写回日志
                brelse(bp);               //* 释放位图块

                bzero(dev, bp_part + bi); // 清空分配块
                return bp_part + bi;      // 返回分配块号
            }
        }

        brelse(bp); //* 释放位图块
        panic("balloc: out of blocks");
    }
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
    minode inode[NINODE]; // 索引数组
} itable;

void iinit()
{
    initlock(&itable.lock, "itable");
    for (int i = 0; i < NINODE; i++)
        initsleeplock(&itable.inode[i].lock, "inode");
}

static struct minode* iget(uint dev, uint inum);

// 从硬盘分配一个空闲inode并标记类型type
// 返回此inum在内存中对应的minode表项
minode* ialloc(uint dev, short type) // stat.h
{
    for (int inum = 1; inum < sb.ninodes; inum++) {
        buf* bp = bread(dev, IBLOCK(inum, sb)); //* 锁定索引块
        dinode* dip = (dinode*)bp->data + inum % IPB;

        // 如果此dinode是空闲的
        if (dip->type == 0) {
            memset(dip, 0, sizeof(*dip)); // 将dinode清零
            dip->type = type;             // 标记dinode类型
            log_write(bp);                // 写回更新后的d索引块 (日志)
            brelse(bp);                   //* 释放索引块
            return iget(dev, inum);       // 返回inum对应的minode表项 (内存)
        }
        brelse(bp); //* 释放索引块
    }

    panic("ialloc: no inodes");
    return 0;
}

// 将修改后的内存minode 更新到 硬盘dinode (需持有ip->lock)
void iupdate(struct minode* ip)
{
    struct buf* bp;
    struct dinode* dip;

    // 读取inum所在的硬盘块, 并计算块中偏移地址
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum % IPB;

    // 更新硬盘中的inode信息
    dip->type = ip->type;
    dip->major = ip->major;
    dip->minor = ip->minor;
    dip->nlink = ip->nlink;
    dip->size = ip->size;
    memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));

    log_write(bp); // 写回更新后的硬盘块 (日志)
    brelse(bp);    // 释放硬盘块缓存
}

// 在minode表中查找inum对应的minode
// - 如果在minode表中找到, 则返回该minode
// - 如果没有找到, 则从minode表分配一个新的空minode
// (不会锁定 inode, 也不从硬盘读取它)
static struct minode* iget(uint dev, uint inum)
{
    struct minode *ip, *empty = 0;

    acquire(&itable.lock); // 获取itable锁

    // 遍历minode表
    for (ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++) {
        // 如果找到了对应的minode
        if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
            ip->ref++;             // 增加引用计数
            release(&itable.lock); // 释放itable锁
            return ip;             // 返回minode
        }
        // 如果有空闲的minode, 则记录到empty
        if (empty == 0 && ip->ref == 0) // Remember empty slot.
            empty = ip;
    }

    // 既没有找到对应minode, 也没有空闲minode
    if (empty == 0)
        panic("iget: no inodes");

    // 如果没有找到对应minode
    // 就初始化一个空闲minode
    ip = empty;
    ip->dev = dev;   // 设备号
    ip->inum = inum; // inode编号
    ip->ref = 1;     // 引用计数置1
    ip->valid = 0;   // 目前此项无效
    release(&itable.lock);

    return ip;
}

// 增加ip的引用计数, 返回ip  <ip = idup(ip1)>
struct minode* idup(struct minode* ip)
{
    acquire(&itable.lock);
    ip->ref++;
    release(&itable.lock);
    return ip;
}

// 获取inode锁 (如果需要, 则从硬盘读取inode)
void ilock(struct minode* ip)
{
    struct buf* bp;
    struct dinode* dip;

    // 确保minode非空闲
    if (ip == 0 || ip->ref <= 0)
        panic("ilock");

    acquiresleep(&ip->lock); // 获取inode锁

    // 如果minode无效
    if (ip->valid == 0) {
        // 读取inum所在的硬盘块, 并计算块中偏移地址
        bp = bread(ip->dev, IBLOCK(ip->inum, sb));
        dip = (struct dinode*)bp->data + ip->inum % IPB;

        // 拷贝硬盘dinode到内存minode
        ip->type = dip->type;
        ip->major = dip->major;
        ip->minor = dip->minor;
        ip->nlink = dip->nlink;
        ip->size = dip->size;
        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));

        brelse(bp);
        ip->valid = 1;

        if (ip->type == 0)
            panic("ilock: no type");
    }
}

// 释放inode锁
void iunlock(struct minode* ip)
{
    // 确保minode非空闲 且持有锁
    if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
        panic("iunlock");

    releasesleep(&ip->lock);
}

// 减少对内存中minode的引用计数。
// 如果这是最后一个引用, 那么可以释放minode表项
// 如果这是最后一个引用, 且没有硬链接, 那么可以释放硬盘上的dinode 及其文件内容
// 所有对 iput() 的调用必须在一个事务中进行，以防需要释放 inode
void iput(struct minode* ip)
{
    acquire(&itable.lock); // 获取itable锁

    // 如果是最后一个引用, 并且没有硬链接
    if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
        // ip->ref == 1 意味着没有其他进程可以锁定ip
        // 因此这个acquiresleep()不会被阻塞(或死锁)
        acquiresleep(&ip->lock); // 获取inode锁

        release(&itable.lock); // 释放itable锁

        itrunc(ip);    // 释放minode的所有硬盘数据块
        ip->type = 0;  // 将minode标记为空闲
        iupdate(ip);   // 将更新后的minode写回硬盘
        ip->valid = 0; // 标记内存minode失效

        releasesleep(&ip->lock); // 释放inode锁

        acquire(&itable.lock); // 重新获取itable锁
    }

    ip->ref--;             // 减少引用计数
    release(&itable.lock); // 释放itable锁
}

// 释放minode锁 并释放minode表项及数据块
void iunlockput(struct minode* ip)
{
    iunlock(ip);
    iput(ip);
}

// -------------------------------- Inode Content -------------------------------- //

// 返回 inode 第 bn 个块的硬盘地址
// 如果没有这样的块，则分配一个新块
static uint bmap(struct minode* ip, uint bn)
{
    uint addr, *a;
    struct buf* bp;

    // 寻找直接块
    if (bn < NDIRECT) {
        if ((addr = ip->addrs[bn]) == 0) {
            addr = balloc(ip->dev); // 分配新直接块
            if (addr == 0)
                return 0;
            ip->addrs[bn] = addr;
        }
        return addr;
    }

    // 寻找间接块
    bn -= NDIRECT;
    if (bn < NINDIRECT) {
        if ((addr = ip->addrs[NDIRECT]) == 0) {
            addr = balloc(ip->dev); // 分配新间接引导块
            if (addr == 0)
                return 0;
            ip->addrs[NDIRECT] = addr;
        }
        bp = bread(ip->dev, addr); // 读取间接引导块
        a = (uint*)bp->data;
        if ((addr = a[bn]) == 0) {
            addr = balloc(ip->dev); // 分配新间接块
            if (addr) {
                a[bn] = addr;
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
void itrunc(struct minode* ip)
{
    struct buf* bp;

    // 释放所有直接块
    for (int i = 0; i < NDIRECT; i++) {
        if (ip->addrs[i]) {
            bfree(ip->dev, ip->addrs[i]);
            ip->addrs[i] = 0;
        }
    }

    // 释放所有间接块
    if (ip->addrs[NDIRECT]) {
        bp = bread(ip->dev, ip->addrs[NDIRECT]);
        uint* a = (uint*)bp->data;
        // 遍历所有间接块 并释放
        for (int j = 0; j < NINDIRECT; j++)
            if (a[j])
                bfree(ip->dev, a[j]);
        brelse(bp);
        bfree(ip->dev, ip->addrs[NDIRECT]);
        ip->addrs[NDIRECT] = 0;
    }

    ip->size = 0; // 更新inode大小
    iupdate(ip);  // 将更新后的minode写回硬盘
}

// 从inode复制stat信息至st
// 调用者必须持有ip->lock
void stati(struct minode* ip, struct stat* st)
{
    st->dev = ip->dev;
    st->ino = ip->inum;
    st->type = ip->type;
    st->nlink = ip->nlink;
    st->size = ip->size;
}

// 读取文件的n个字节到dst
// 返回成功读取的字节数
// (调用者必须持有ip->lock)
// user_dst=1 : dst是用户地址
// user_dst=0 : dst是内核地址
int readi(struct minode* ip, int user_dst, uint64 dst, uint off, uint n)
{
    uint total, m;
    struct buf* bp;

    // 确保偏移量在文件范围内
    if (off >= ip->size || n <= 0)
        return 0;

    // 如果总长度超过文件总大小, 则截断
    if (off + n > ip->size)
        n = ip->size - off;

    for (total = 0; total < n; total += m, off += m, dst += m) {
        // 返回 off 所处硬盘块的地址 (保证偏移量在范围内, 不会扩展文件)
        uint addr = bmap(ip, off / BSIZE);
        if (addr == 0)
            break;

        // 读取 off 所处硬盘块
        bp = bread(ip->dev, addr);

        // 计算可以读取的字节数
        m = min(n - total, BSIZE - off % BSIZE);

        // 将硬盘块数据拷贝到dst[m]
        if (either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
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
int writei(struct minode* ip, int user_src, uint64 src, uint off, uint n)
{
    uint total, m;
    struct buf* bp;

    // 确保偏移量在文件范围内 并且不会溢出
    if (off > ip->size || off + n < off)
        return -1;

    // 确保写入总长度不超过 允许的文件最大长度
    if (off + n > MAXFILE * BSIZE)
        return -1;

    for (total = 0; total < n; total += m, off += m, src += m) {
        // 返回 off 所处硬盘块的地址 (超出部分 自动扩展文件)
        uint addr = bmap(ip, off / BSIZE);
        if (addr == 0)
            break;

        // 读取 off 所处硬盘块
        bp = bread(ip->dev, addr);

        // 计算可以写入的字节数
        m = min(n - total, BSIZE - off % BSIZE);

        // 将dst[m]数据写入到硬盘块
        if (either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
            brelse(bp);
            break;
        }

        log_write(bp); // 写回硬盘块 (日志)
        brelse(bp);    // 释放硬盘块缓存
    }

    // 如果写入超过文件大小, 则更新
    if (off > ip->size)
        ip->size = off;

    // ip->size可能更新, bmap可能更新了ip->addrs[]
    // 将修改后的内存minode 更新到 硬盘dinode
    iupdate(ip);

    return total;
}

// -------------------------------- Directory -------------------------------- //

// 比较两个目录项的名称是否相同
int namecmp(const char* s, const char* t) { return strncmp(s, t, DIRSIZ); }

// 在指定目录dp中查找给定名称name的目录项
// 如果找到该目录项，则将其字节偏移量存储在 *poff 中，并返回 minode
struct minode* dirlookup(struct minode* dp, char* name, uint* poff)
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
int dirlink(struct minode* dp, char* name, uint inum)
{
    int off;
    struct dirent de;
    struct minode* ip;

    // 确保文件name在目录dp中
    if ((ip = dirlookup(dp, name, 0)) != 0) {
        iput(ip);
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
    char* s;
    int len;

    // 跳过前缀的'/'
    while (*path == '/')
        path++;

    // 如果path为空, 则返回0
    if (*path == 0)
        return 0;

    // 记录路径的起始位置
    s = path;

    // 提取第一个元素
    while (*path != '/' && *path != 0)
        path++;

    // 记录第一个元素的长度
    len = path - s;

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
static struct minode* namex(char* path, int nameiparent, char* name)
{
    struct minode *ip, *next;

    if (*path == '/')
        ip = iget(ROOTDEV, ROOTINO); // 直接返回根目录inode
    else
        ip = idup(myproc()->cwd); // 增加对当前工作目录的引用

    // 逐级查找目录
    while ((path = skipelem(path, name)) != 0) {
        // 锁定当前目录inode
        ilock(ip);
        // 确保cwd是一个目录类型
        if (ip->type != T_DIR) {
            iunlockput(ip);
            return 0;
        }
        // 提前一次循环, 返回父目录inode
        if (nameiparent && *path == '\0') {
            iunlock(ip);
            return ip;
        }
        // 在当前目录ip中查找name对应项 并记为next
        if ((next = dirlookup(ip, name, 0)) == 0) {
            iunlockput(ip);
            return 0;
        }
        // 释放当前目录inode, 继续处理next
        iunlockput(ip);
        ip = next;
    }

    // 确保需要返回父目录时, 不会到达这里
    if (nameiparent) {
        iput(ip);
        return 0;
    }

    // 返回path对应的inode
    return ip;
}

// 获取path对应的inode (封装namex)
struct minode* namei(char* path)
{
    char name[DIRSIZ];
    return namex(path, 0, name);
}

// 获取path对应的inode的父目录inode (封装namex)
struct minode* nameiparent(char* path, char* name) { return namex(path, 1, name); }
