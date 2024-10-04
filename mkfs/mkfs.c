
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

// 避免和主机的stat结构体冲突
#define stat xv6_stat
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
#ifndef static_assert
#define static_assert(a, b) \
    do {                    \
        switch (0)          \
        case 0:             \
        case (a):;          \
    } while (0)
#endif

// 磁盘布局 
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]
// [          0 |           1 | 2       31 | 32        44 |           45 | 46     1999 ]
#define NINODES 200                    // 最大inode数量
int nbitmap = FSSIZE / BPB + 1;        // 需要的位图块数量
int ninodeblocks = NINODES / IPB + 1;  // 需要的inode块数量
int nlog = LOGSIZE;                    // 需要的日志块数量
int nmeta;                             // 元数据块数量 (boot, sb, nlog, inode, bitmap)
int nblocks;                           // 数据块数量

int fsfd;              // fs.img 文件描述符
struct superblock sb;  // 超级块
char zeroes[BSIZE];    // 零填充块
uint freeinode = 1;    // 空闲inode编号
uint freeblock;        // 空闲数据块

void rsect(uint sec, void* buf);  // 读取磁盘的第sec扇区到buf
void wsect(uint sec, void* buf);  // 将buf写入到磁盘的第sec扇区

uint ialloc(ushort type);                   // 分配类型为type的新inode
void rinode(uint inum, struct dinode* ip);  // 读取第inum个inode信息到ip
void winode(uint inum, struct dinode* ip);  // 将inode信息ip写入到对应inode块
void iappend(uint inum, void* p, int n);    // 向inode追加数据p[n]

void balloc(int used);  // 更新首个位图块  used:现已使用的块数
void die(const char*);  // 打印错误信息并退出

// 大小端序转换(16位)
ushort xshort(ushort x) {
    ushort y;
    uchar* a = (uchar*)(&y);
    a[0] = x;
    a[1] = x >> 8;
    return y;
}

// 大小端序转换(32位)
uint xint(uint x) {
    uint y;
    uchar* a = (uchar*)(&y);
    a[0] = x;
    a[1] = x >> 8;
    a[2] = x >> 16;
    a[3] = x >> 24;
    return y;
}

int main(int argc, char* argv[]) {
    int i, cc, fd;
    uint rootino, inum, off;
    struct dirent de;
    char buf[BSIZE];
    struct dinode din;

    // 保证int是4字节
    static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

    // 用法 mkfs fs.img files...
    if (argc < 2) {
        fprintf(stderr, "Usage: mkfs fs.img files...\n");
        exit(1);
    }

    // 确保磁盘块大小是inode和irent大小的整数倍
    assert((BSIZE % sizeof(struct dinode)) == 0);
    assert((BSIZE % sizeof(struct dirent)) == 0);

    // fs.img 如果不存在则创建, 如果已存在则清空 (有读写权限)
    fsfd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fsfd < 0)
        die(argv[1]);

    // 文件系统块 <-> 磁盘扇区
    // 元数据块数量 (boot, sb, nlog, inode, bitmap)
    nmeta = 1 + 1 + nlog + ninodeblocks + nbitmap;
    nblocks = FSSIZE - nmeta;  // 空闲数据块数量

    sb.magic = FSMAGIC;                            // 魔数
    sb.size = xint(FSSIZE);                        // 文件系统总块数
    sb.nblocks = xint(nblocks);                    // 数据块数量
    sb.ninodes = xint(NINODES);                    // inode数量
    sb.nlog = xint(nlog);                          // 日志块数量
    sb.logstart = xint(2);                         // 第一个日志块的块号
    sb.inodestart = xint(2 + nlog);                // 第一个inode块的块号
    sb.bmapstart = xint(2 + nlog + ninodeblocks);  // 第一个位图块的块号

    printf(
        "total=%d\n"
        "\tnmeta=%d\n"
        "\t -boot=1\n"
        "\t -super=1\n"
        "\t -log blocks=%u\n"
        "\t -inode blocks=%u\n"
        "\t -bitmap blocks=%u\n"
        "\tdata blocks=%d\n\n",
        FSSIZE, nmeta, nlog, ninodeblocks, nbitmap, nblocks);

    // 第一个可分配的数据块
    freeblock = nmeta;

    // 清空所有磁盘块
    for (i = 0; i < FSSIZE; i++)
        wsect(i, zeroes);

    // 初始化并写入超级块
    memset(buf, 0, sizeof(buf));
    memmove(buf, &sb, sizeof(sb));
    wsect(1, buf);

    // 写入根目录inode
    rootino = ialloc(T_DIR);
    assert(rootino == ROOTINO);

    // 向根目录inode追加 dirent{rootino, "."}
    memset(&de, 0, sizeof(de));
    de.inum = xshort(rootino);
    strcpy(de.name, ".");
    iappend(rootino, &de, sizeof(de));

    // 向根目录inode追加 dirent{rootino, ".."}
    memset(&de, 0, sizeof(de));
    de.inum = xshort(rootino);
    strcpy(de.name, "..");
    iappend(rootino, &de, sizeof(de));

    for (i = 2; i < argc; i++) {
        char* shortname;

        // 确保文件名不包含 "user/"
        if (strncmp(argv[i], "user/", 5) == 0)
            shortname = argv[i] + 5;
        else
            shortname = argv[i];

        // 确保文件名不包含 '/'
        assert(strchr(shortname, '/') == 0);

        // 打开文件描述符 fd->argv[i]
        if ((fd = open(argv[i], 0)) < 0)
            die(argv[i]);

        // 移除程序文件名前面的 '_'
        if (shortname[0] == '_')
            shortname += 1;

        // 确保文件名长度不超过 DIRSIZ
        assert(strlen(shortname) <= DIRSIZ);

        // 分配新的文件inode
        inum = ialloc(T_FILE);

        // 向根目录inode追加 dirent{inum, shortname}
        memset(&de, 0, sizeof(de));
        de.inum = xshort(inum);
        strncpy(de.name, shortname, DIRSIZ);
        iappend(rootino, &de, sizeof(de));

        // 向新的文件inode追加文件内容
        while ((cc = read(fd, buf, sizeof(buf))) > 0)
            iappend(inum, buf, cc);

        // 关闭文件描述符
        close(fd);
    }

    // 将根目录大小对齐到BSIZE
    rinode(rootino, &din);
    off = xint(din.size);
    off = ((off / BSIZE) + 1) * BSIZE;
    din.size = xint(off);
    winode(rootino, &din);

    // 更新首个位图块
    balloc(freeblock);

    exit(0);
}

// 读取磁盘的第sec扇区到buf
void rsect(uint sec, void* buf) {
    // 移动fsfd位置到第sec扇区
    if (lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
        die("lseek");
    // 读取第sec扇区到buf
    if (read(fsfd, buf, BSIZE) != BSIZE)
        die("read");
}

// 将buf写入到磁盘的第sec扇区
void wsect(uint sec, void* buf) {
    // 移动fsfd位置到第sec扇区
    if (lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
        die("lseek");
    // 将buf写入到第sec扇区
    if (write(fsfd, buf, BSIZE) != BSIZE)
        die("write");
}

// 分配类型为type的新inode
uint ialloc(ushort type) {
    // 分配新的inode编号
    uint inum = freeinode++;
    struct dinode din;

    // 清空din
    memset(&din, 0, sizeof(din));

    // 转换大小端序
    din.type = xshort(type);  // 文件类型
    din.nlink = xshort(1);    // 硬链接数
    din.size = xint(0);       // 文件大小

    // 将inode信息写入到对应inode块
    winode(inum, &din);
    return inum;
}

// 读取第inum个inode信息到ip
void rinode(uint inum, struct dinode* ip) {
    char buf[BSIZE];
    uint bn;
    struct dinode* dip;

    // 根据超级块 计算第i个inode所在的块
    bn = IBLOCK(inum, sb);

    // 读取第bn块到buf
    rsect(bn, buf);

    // 移动到第inum个inode在bn块中的位置
    dip = ((struct dinode*)buf) + (inum % IPB);

    // 将dip的信息读取到ip
    *ip = *dip;
}

// 将inode信息ip写入到对应inode块
void winode(uint inum, struct dinode* ip) {
    char buf[BSIZE];
    uint bn;
    struct dinode* dip;

    // 根据超级块 计算第inum个inode所在的块
    bn = IBLOCK(inum, sb);

    // 读取第bn块到buf
    rsect(bn, buf);

    // 移动到第inum个inode在bn块中的位置
    dip = ((struct dinode*)buf) + (inum % IPB);

    // 将ip写入到dip
    *dip = *ip;

    // 将buf写回到第bn块
    wsect(bn, buf);
}

// 向指定的inode追加数据xp[n]
// inum: inode编号  xp: 写入的数据  n: 写入的数据大小
void iappend(uint inum, void* xp, int n) {
    char* p = (char*)xp;
    uint fbn, off, n1;
    struct dinode din;         // 磁盘inode
    char buf[BSIZE];           // 读写缓冲区
    uint indirect[NINDIRECT];  // 间接索引块
    uint x;                    // 可供写入的文件块

    // 读取inode信息到din
    rinode(inum, &din);

    // inode现有文件的总大小
    off = xint(din.size);

    while (n > 0) {
        // 计算当前处于inode现有文件的第几个块
        fbn = off / BSIZE;
        assert(fbn < MAXFILE);

        // 处理直接块
        if (fbn < NDIRECT) {
            // 如果该直接块还未分配, 则分配一个空闲块
            if (din.addrs[fbn] == 0) {
                din.addrs[fbn] = xint(freeblock++);
            }
            x = xint(din.addrs[fbn]);
        }

        // 处理间接块
        else {
            // 如果该间接索引块未分配, 则分配一个空闲块
            if (xint(din.addrs[NDIRECT]) == 0) {
                din.addrs[NDIRECT] = xint(freeblock++);
            }
            // 读取间接索引块 到indirect
            rsect(xint(din.addrs[NDIRECT]), (char*)indirect);

            // 如果该间接块还未分配, 则分配一个空闲块
            if (indirect[fbn - NDIRECT] == 0) {
                indirect[fbn - NDIRECT] = xint(freeblock++);
                // 更新间接索引块的索引信息
                wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
            }
            x = xint(indirect[fbn - NDIRECT]);
        }

        // 计算可以写入到当前块x的数据大小
        n1 = min(n, (fbn + 1) * BSIZE - off);

        rsect(x, buf);                            // 读取块x到buf
        bcopy(p, buf + (off - fbn * BSIZE), n1);  // 将p部分写入到buf
        wsect(x, buf);                            // 将buf写回到块x

        // 继续写入剩余数据
        n -= n1;
        p += n1;
        off += n1;
    }
    // 更新inode文件总大小
    din.size = xint(off);
    winode(inum, &din);
}

// 更新首个位图块  used:现已使用的块数
void balloc(int used) {
    uchar buf[BSIZE];
    int i;

    printf("balloc: 目前已分配前 %d 块\n", used);
    assert(used < BPB);  // 保证已用块数 小于 单个位图块 最多能够指示的块数

    // 填充位图块内容 (used)
    memset(buf, 0, BSIZE);
    for (i = 0; i < used; i++)
        buf[i / 8] = buf[i / 8] | (0x1 << (i % 8));

    printf("balloc: 位图块处于第 %d 块\n", sb.bmapstart);
    wsect(sb.bmapstart, buf);  // 将buf写入到位图块
}

// 打印错误信息并退出
void die(const char* s) {
    perror(s);
    exit(1);
}
