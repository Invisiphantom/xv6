
// 文件系统实现:
//  + FS.img: 文件系统镜像 (mkfs.c)
//  + Dev+blockno: 原始硬盘块 (virtio_disk.c)
//  + Bcache: 缓存链环 (bio.c)
//  + Log: 多步更新的崩溃恢复 (log.c)
//  + Inodes: inode分配器, 读取, 写入, 元数据 (fs.c)
//  + Directories: 具有特殊内容的inode(其他inode的列表) (fs.c)
//  + PathNames: 方便命名的路径, 如 /usr/rtm/xv6/fs.c (fs.c)

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

// 避免和标准库的stat结构体冲突
#define stat std_stat

#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
#ifndef static_assert
#define static_assert(a, b)                                                                        \
    do {                                                                                           \
        switch (0)                                                                                 \
        case 0:                                                                                    \
        case (a):;                                                                                 \
    } while (0)
#endif

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [      0     |      1      | 2       31 | 32        44 |      45      | 46     1999 ]
#define NINODES 200                   // 索引结点的最大数量
int nbitmap = FSSIZE / BPB + 1;       // 需要的位图块数量 (1)
int ninodeblocks = NINODES / IPB + 1; // 需要的索引块数量 (13)
int nlog = LOGSIZE;                   // 需要的日志块数量 (30)
int nmeta;                            // 元数据块数量 (46)
int nblocks;                          // 空闲数据块数量 (1954)

int fsfd;             // fs.img 文件描述符
struct superblock sb; // 超级块结构体
char zeroes[BSIZE];   // 用于填充的零块
uint freeinode = 1;   // 索引结点编号
uint freeblock;       // 空闲数据块

void rsect(uint sec, void* buf);
void wsect(uint sec, void* buf);

uint ialloc(ushort type);
void rinode(uint inum, struct dinode* ip);
void winode(uint inum, struct dinode* ip);
void iappend(uint inum, void* p, int n);

void balloc(uint used);
void die(const char*);

// 大小端序转换(16位)
ushort xshort(ushort x)
{
    ushort y;
    uchar* a = (uchar*)(&y);
    a[0] = x;
    a[1] = x >> 8;
    return y;
}

// 大小端序转换(32位)
uint xint(uint x)
{
    uint y;
    uchar* a = (uchar*)(&y);
    a[0] = x;
    a[1] = x >> 8;
    a[2] = x >> 16;
    a[3] = x >> 24;
    return y;
}

int main(int argc, char* argv[])
{
    int cnt, fd;
    char buf[BSIZE];

    // 确保int是4字节
    static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");
    if (argc < 2) {
        fprintf(stderr, "Usage: mkfs fs.img files...\n");
        exit(1);
    }

    // 确保硬盘块大小是硬盘inode和dirent的整数倍
    assert((BSIZE % sizeof(struct dinode)) == 0);
    assert((BSIZE % sizeof(struct dirent)) == 0);

    // fs.img 如果不存在则创建, 如果存在则清空 (读写权限)
    fsfd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fsfd < 0)
        die(argv[1]);

    // 元数据块数量 (46) (boot, sb, nlog, inode, bitmap)
    nmeta = 1 + 1 + nlog + ninodeblocks + nbitmap;
    nblocks = FSSIZE - nmeta; // 空闲数据块数量 (1954)

    sb.magic = FSMAGIC;                           // 魔数
    sb.size = xint(FSSIZE);                       // 文件系统总块数
    sb.nblocks = xint(nblocks);                   // 数据块数量 (1954)
    sb.ninodes = xint(NINODES);                   // 索引数量 (200)
    sb.nlog = xint(nlog);                         // 日志块数量 (30)
    sb.logstart = xint(2);                        // 第一个日志块的块号 (2)
    sb.inodestart = xint(2 + nlog);               // 第一个索引块的块号 (32)
    sb.bmapstart = xint(2 + nlog + ninodeblocks); // 第一个位图块的块号 (234)

    printf("total=%d\n"
           "|--nmeta=%d\n"
           "|  |--boot=1\n"
           "|  |--sb=1\n"
           "|  |--nlog=%u\n"
           "|  |--ninodeblocks=%u\n"
           "|  └--nbitmap=%u\n"
           "└--nblocks=%d\n\n",
        FSSIZE, nmeta, nlog, ninodeblocks, nbitmap, nblocks);

    // 第一个可分配的数据块
    freeblock = nmeta;

    // 清空fsfd的所有块区
    for (int i = 0; i < FSSIZE; i++)
        wsect(i, zeroes);

    // 写入超级块
    memset(buf, 0, sizeof(buf));
    memmove(buf, &sb, sizeof(sb));
    wsect(1, buf);

    // 分配新的根目录inode
    uint rootino = ialloc(T_DIR);
    assert(rootino == ROOTINO);

    // 向rootino追加 dirent{rootino, "."}
    struct dirent de;
    memset(&de, 0, sizeof(de));
    de.inum = xshort(rootino);
    strcpy(de.name, ".");
    iappend(rootino, &de, sizeof(de));

    // 向rootino追加 dirent{rootino, ".."}
    memset(&de, 0, sizeof(de));
    de.inum = xshort(rootino);
    strcpy(de.name, "..");
    iappend(rootino, &de, sizeof(de));

    // 向根目录装载用户程序
    for (int i = 2; i < argc; i++) {
        char* shortname;

        // 移除前缀 "user/"
        if (strncmp(argv[i], "user/", 5) == 0)
            shortname = argv[i] + 5;
        else
            shortname = argv[i];

        // 确保文件名不包含 '/'
        assert(strchr(shortname, '/') == 0);

        // 打开文件描述符 fd->argv[i]
        if ((fd = open(argv[i], 0)) < 0)
            die(argv[i]);

        // 移除前缀 '_'
        if (shortname[0] == '_')
            shortname += 1;

        // 确保文件名长度不超过 DIRSIZ
        assert(strlen(shortname) <= DIRSIZ);

        // 分配新的文件inode
        uint inum = ialloc(T_FILE);

        // 向根目录inode追加 dirent{inum, shortname}
        memset(&de, 0, sizeof(de));
        de.inum = xshort(inum);
        strncpy(de.name, shortname, DIRSIZ);
        iappend(rootino, &de, sizeof(de));

        // 向新的文件inode追加内容
        while ((cnt = read(fd, buf, sizeof(buf))) > 0)
            iappend(inum, buf, cnt);

        // 关闭文件描述符
        close(fd);
    }

    // 将根目录大小对齐到BSIZE
    struct dinode din;
    rinode(rootino, &din);
    uint off = xint(din.size);
    off = ((off / BSIZE) + 1) * BSIZE;
    din.size = xint(off);
    winode(rootino, &din);

    // 写入首个位图块
    balloc(freeblock);

    exit(0);
}

// 读取fsfd的第sec块区到buf
void rsect(uint sec, void* buf)
{
    // 从开头移动fsfd位置到第sec块区
    if (lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
        die("lseek");
    // 读取长度为BSIZE的块到buf
    if (read(fsfd, buf, BSIZE) != BSIZE)
        die("read");
}

// 将buf写入到fsfd的第sec块区
void wsect(uint sec, void* buf)
{
    // 从开头移动fsfd位置到第sec块区
    if (lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
        die("lseek");
    // 从buf写入长度为BSIZE的块
    if (write(fsfd, buf, BSIZE) != BSIZE)
        die("write");
}

// 分配类型为type的新inode, 返回编号
uint ialloc(ushort type)
{
    // 分配新的inode编号
    uint inum = freeinode++;

    // 清空并写入dinode信息
    struct dinode din;
    memset(&din, 0, sizeof(din));
    din.type = xshort(type); // 文件类型
    din.nlink = xshort(1);   // 硬链接数
    din.size = xint(0);      // 文件大小

    // 将inode信息写入到对应索引块
    winode(inum, &din);
    return inum;
}

// 读取硬盘-索引项
void rinode(uint inum, dinode* ip)
{
    char buf[BSIZE];

    // 计算并读取硬盘-索引块
    uint bn = IBLOCK(inum, sb);
    rsect(bn, buf);

    // 解析出对应索引项
    dinode* dip = ((struct dinode*)buf) + (inum % IPB);
    *ip = *dip;
}

// 写回硬盘-索引项
void winode(uint inum, dinode* ip)
{
    char buf[BSIZE];

    // 计算并读取硬盘-索引块
    uint bn = IBLOCK(inum, sb);
    rsect(bn, buf);

    // 将索引写入缓冲区
    dinode* dip = ((struct dinode*)buf) + (inum % IPB);
    *dip = *ip;

    // 写回缓冲区
    wsect(bn, buf);
}

// 向指定的inode追加数据xp[n]
// inum:inode编号  xp:写入的数据  n:写入的数据大小
void iappend(uint inum, void* xp, int n)
{
    char* p = (char*)xp;
    uint fbn, off, n1;
    struct dinode din;        // 硬盘inode
    char buf[BSIZE];          // 读写缓冲区
    uint indirect[NINDIRECT]; // 间接索引块缓冲区
    uint x;                   // 可供写入的文件块

    // 读取inode信息到din
    rinode(inum, &din);

    // inode现有文件的总大小
    off = xint(din.size);

    while (n > 0) {
        // 最后一个文件块编号
        fbn = off / BSIZE;
        assert(fbn < MAXFILE);

        // 处理直接块
        if (fbn < NDIRECT) {
            // 如果该直接块还未分配, 则分配一个空闲块
            if (din.addrs[fbn] == 0) {
                din.addrs[fbn] = xint(freeblock);
                freeblock++;
            }
            x = xint(din.addrs[fbn]); // 文件末块地址
        }

        // 处理间接块
        else {
            // 如果该间接索引块未分配, 则分配一个空闲块
            if (xint(din.addrs[NDIRECT]) == 0) {
                din.addrs[NDIRECT] = xint(freeblock);
                freeblock++;
            }

            // 读取间接索引块 到indirect
            rsect(xint(din.addrs[NDIRECT]), (char*)indirect);

            // 如果该间接块还未分配, 则分配一个空闲块
            if (indirect[fbn - NDIRECT] == 0) {
                indirect[fbn - NDIRECT] = xint(freeblock);
                freeblock++;

                // 写回更新间接索引块
                wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
            }
            x = xint(indirect[fbn - NDIRECT]); // 文件末块地址
        }

        // 计算可以写入到当前块x的数据大小
        n1 = min(n, (fbn + 1) * BSIZE - off);

        // 将p[n1]写入末尾块x
        rsect(x, buf);
        bcopy(p, buf + (off - fbn * BSIZE), n1);
        wsect(x, buf);

        // 继续处理剩余部分
        n -= n1;
        p += n1;
        off += n1;
    }

    // 更新索引块的inode信息
    din.size = xint(off);
    winode(inum, &din);
}

// 写入首个位图块
// used:现已使用的块数
void balloc(uint used)
{
    uchar buf[BSIZE];

    printf("balloc: 目前已使用前 %d 块\n", used);
    assert(used < BPB);

    // 填充位图块内容 (used)
    memset(buf, 0, BSIZE);
    for (uint i = 0; i < used; i++)
        buf[i / 8] = buf[i / 8] | (0x1 << (i % 8));

    printf("balloc: 位图块处于第 %d 块\n", sb.bmapstart);
    wsect(sb.bmapstart, buf); // 将buf写入到位图块
}

// 打印错误信息并退出
void die(const char* s)
{
    perror(s);
    exit(1);
}
