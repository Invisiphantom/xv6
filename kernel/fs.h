
// 磁盘文件系统格式
// 内核和用户程序都使用此头文件

#define ROOTINO 1   // root i-number
#define BSIZE 1024  // 块大小

// 磁盘布局
// [ boot block | super block | log | inode blocks | free bit map | data blocks]

// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
    uint magic;       // 魔数=FSMAGIC
    uint size;        // 文件系统总块数
    uint nblocks;     // 数据块数量
    uint ninodes;     // inode数量
    uint nlog;        // 日志块数量
    uint logstart;    // 第一个日志块的块号
    uint inodestart;  // 第一个inode块的块号
    uint bmapstart;   // 第一个空闲映射块的块号
};

#define FSMAGIC 0x10203040

#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

// 硬盘上的inode结构
struct dinode {
    short type;               // 文件类型
    short major;              // 主设备号
    short minor;              // 次设备号
    short nlink;              // 硬链接数
    uint size;                // 文件大小
    uint addrs[NDIRECT + 1];  // 数据块地址
};

// 每个inode块 最多能包含的inode数量
#define IPB (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// 每个位图块 最多能够指示的块数
#define BPB (BSIZE * 8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

// 目录项
struct dirent {
    ushort inum;        // inode编号
    char name[DIRSIZ];  // 内部包含的文件名称
};
