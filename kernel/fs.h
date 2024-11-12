
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

#define BSIZE 1024 // 块大小
#define FSMAGIC 0x10203040
typedef struct superblock {
    uint magic;   // 魔数=FSMAGIC
    uint size;    // 文件系统总块数 (2000块)
    uint nblocks; // 数据块数量 (1954块)
    uint ninodes; // 索引数量 (200项)
    uint nlog;    // 日志块数量 (30块)

    uint logstart;   // 第一个日志块的块号 (2)
    uint inodestart; // 第一个索引块的块号 (32)
    uint bmapstart;  // 第一个位图块的块号 (45)
} superblock;

// ----------------------------------------------------------------

#define ROOTINO 1                        // 根目录的索引编号 (1)
#define NDIRECT 12                       // 直接块数量 (12)
#define NINDIRECT (BSIZE / sizeof(uint)) // 间接块数量 (256)
#define MAXFILE (NDIRECT + NINDIRECT)    // 最大总块数 (268)

// 硬盘-索引项
typedef struct dinode {
    short type;              // 索引类型 (stat.h)
    short major;             // 主设备号
    short minor;             // 次设备号
    short nlink;             // 硬链接数
    uint size;               // 文件大小 (字节)
    uint addrs[NDIRECT + 1]; // 文件块号 (直接块+间接引导块)
} dinode;

// ----------------------------------------------------------------

#define BPB (BSIZE * 8)                          // 每块的最大位图数量
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart) // 计算所在的位图块

#define IPB (BSIZE / sizeof(struct dinode))       // 每块的最大索引数量
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart) // 计算所在的索引块

// ----------------------------------------------------------------

#define DIRSIZ 14 // 文件名最大长度

// 目录下的文件项
typedef struct dirent {
    ushort inum;       // 索引编号
    char name[DIRSIZ]; // 文件名称
} dirent;
