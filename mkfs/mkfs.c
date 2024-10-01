#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#ifndef static_assert
#define static_assert(a, b) \
    do {                    \
        switch (0)          \
        case 0:             \
        case (a):;          \
    } while (0)
#endif

// 磁盘布局 [ boot block | sb block | log | inode blocks | free bit map | data blocks ]
#define NINODES 200                    // 最大inode数量
int nbitmap = FSSIZE / BPB + 1;        // 需要的位图块数量
int ninodeblocks = NINODES / IPB + 1;  // 需要的inode块数量
int nlog = LOGSIZE;                    // 需要的日志块数量
int nmeta;                             // 元数据块数量 (boot, sb, nlog, inode, bitmap)
int nblocks;                           // 数据块数量

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;

void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode* ip);
void rsect(uint sec, void* buf);
uint ialloc(ushort type);
void iappend(uint inum, void* p, int n);
void die(const char*);

// 将大端序转换为小端序
ushort xshort(ushort x) {
    ushort y;
    uchar* a = (uchar*)(&y);
    a[0] = x;
    a[1] = x >> 8;
    return y;
}
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

    // fs.img 如果不存在则创建, 如果已存在则清空 (读写权限)
    fsfd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fsfd < 0)
        die(argv[1]);

    // 文件系统块 <-> 磁盘扇区
    // 元数据块数量 (boot, sb, nlog, inode, bitmap)
    nmeta = 1 + 1 + nlog + ninodeblocks + nbitmap;
    nblocks = FSSIZE - nmeta;  // 数据块数量

    sb.magic = FSMAGIC;                            // 魔数
    sb.size = xint(FSSIZE);                        // 文件系统总块数
    sb.nblocks = xint(nblocks);                    // 数据块数量
    sb.ninodes = xint(NINODES);                    // inode数量
    sb.nlog = xint(nlog);                          // 日志块数量
    sb.logstart = xint(2);                         // 第一个日志块的块号
    sb.inodestart = xint(2 + nlog);                // 第一个inode块的块号
    sb.bmapstart = xint(2 + nlog + ninodeblocks);  // 第一个空闲映射块的块号

    printf(
        "nmeta=%d (boot, super, log blocks=%u, inode blocks=%u, bitmap blocks=%u)"
        " blocks=%d total=%d\n",
        nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

    freeblock = nmeta;  // 第一个可分配的空闲块

    // 清空所有磁盘块
    for (i = 0; i < FSSIZE; i++)
        wsect(i, zeroes);

    // 初始化超级块
    memset(buf, 0, sizeof(buf));
    memmove(buf, &sb, sizeof(sb));
    wsect(1, buf);

    rootino = ialloc(T_DIR);
    assert(rootino == ROOTINO);

    bzero(&de, sizeof(de));
    de.inum = xshort(rootino);
    strcpy(de.name, ".");
    iappend(rootino, &de, sizeof(de));

    bzero(&de, sizeof(de));
    de.inum = xshort(rootino);
    strcpy(de.name, "..");
    iappend(rootino, &de, sizeof(de));

    for (i = 2; i < argc; i++) {
        // get rid of "user/"
        char* shortname;
        if (strncmp(argv[i], "user/", 5) == 0)
            shortname = argv[i] + 5;
        else
            shortname = argv[i];

        assert(index(shortname, '/') == 0);

        if ((fd = open(argv[i], 0)) < 0)
            die(argv[i]);

        // Skip leading _ in name when writing to file system.
        // The binaries are named _rm, _cat, etc. to keep the
        // build operating system from trying to execute them
        // in place of system binaries like rm and cat.
        if (shortname[0] == '_')
            shortname += 1;

        assert(strlen(shortname) <= DIRSIZ);

        inum = ialloc(T_FILE);

        bzero(&de, sizeof(de));
        de.inum = xshort(inum);
        strncpy(de.name, shortname, DIRSIZ);
        iappend(rootino, &de, sizeof(de));

        while ((cc = read(fd, buf, sizeof(buf))) > 0)
            iappend(inum, buf, cc);

        close(fd);
    }

    // fix size of root inode dir
    rinode(rootino, &din);
    off = xint(din.size);
    off = ((off / BSIZE) + 1) * BSIZE;
    din.size = xint(off);
    winode(rootino, &din);

    balloc(freeblock);

    exit(0);
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

void winode(uint inum, struct dinode* ip) {
    char buf[BSIZE];
    uint bn;
    struct dinode* dip;

    bn = IBLOCK(inum, sb);
    rsect(bn, buf);
    dip = ((struct dinode*)buf) + (inum % IPB);
    *dip = *ip;
    wsect(bn, buf);
}

void rinode(uint inum, struct dinode* ip) {
    char buf[BSIZE];
    uint bn;
    struct dinode* dip;

    bn = IBLOCK(inum, sb);
    rsect(bn, buf);
    dip = ((struct dinode*)buf) + (inum % IPB);
    *ip = *dip;
}

void rsect(uint sec, void* buf) {
    if (lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
        die("lseek");
    if (read(fsfd, buf, BSIZE) != BSIZE)
        die("read");
}

// 分配类型为type的inode
uint ialloc(ushort type) {
    // 分配新的inode编号
    uint inum = freeinode++;
    struct dinode din;

    bzero(&din, sizeof(din));
    din.type = xshort(type);
    din.nlink = xshort(1);
    din.size = xint(0);
    winode(inum, &din);
    return inum;
}

void balloc(int used) {
    uchar buf[BSIZE];
    int i;

    printf("balloc: first %d blocks have been allocated\n", used);
    assert(used < BPB);
    bzero(buf, BSIZE);
    for (i = 0; i < used; i++) {
        buf[i / 8] = buf[i / 8] | (0x1 << (i % 8));
    }
    printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
    wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void iappend(uint inum, void* xp, int n) {
    char* p = (char*)xp;
    uint fbn, off, n1;
    struct dinode din;
    char buf[BSIZE];
    uint indirect[NINDIRECT];
    uint x;

    rinode(inum, &din);
    off = xint(din.size);
    // printf("append inum %d at off %d sz %d\n", inum, off, n);
    while (n > 0) {
        fbn = off / BSIZE;
        assert(fbn < MAXFILE);
        if (fbn < NDIRECT) {
            if (xint(din.addrs[fbn]) == 0) {
                din.addrs[fbn] = xint(freeblock++);
            }
            x = xint(din.addrs[fbn]);
        } else {
            if (xint(din.addrs[NDIRECT]) == 0) {
                din.addrs[NDIRECT] = xint(freeblock++);
            }
            rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
            if (indirect[fbn - NDIRECT] == 0) {
                indirect[fbn - NDIRECT] = xint(freeblock++);
                wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
            }
            x = xint(indirect[fbn - NDIRECT]);
        }
        n1 = min(n, (fbn + 1) * BSIZE - off);
        rsect(x, buf);
        bcopy(p, buf + off - (fbn * BSIZE), n1);
        wsect(x, buf);
        n -= n1;
        off += n1;
        p += n1;
    }
    din.size = xint(off);
    winode(inum, &din);
}

void die(const char* s) {
    perror(s);
    exit(1);
}
