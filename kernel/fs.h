// On-disk file system format.
// Both the kernel and user programs use this header file.

#define ROOTINO 1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
    uint magic;       // Must be FSMAGIC
    uint size;        // Size of file system image (blocks)
    uint nblocks;     // Number of data blocks
    uint ninodes;     // Number of inodes.
    uint nlog;        // Number of log blocks
    uint logstart;    // Block number of first log block
    uint inodestart;  // Block number of first inode block
    uint bmapstart;   // Block number of first free map block
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

// Inodes per block.
#define IPB (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// Bitmap bits per block
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
