
// 文件系统实现:
//  + UART: 串口输入输出 (printf.c, console.c, uart.c)
//  + FS.img: 文件系统映像 (mkfs.c)
//  + Dev+blockno: 虚拟硬盘块设备 (virtio_disk.c)
//  + Bcache: 缓存链环 (bio.c, buf.h)
//  + Log: 多步更新的崩溃恢复 (log.c)
//  + Inode: inode分配器, 读取, 写入, 元数据 (fs.c)
//  + Directory: 具有特殊内容的inode(其他inode的列表) (fs.c)
//  + Path: 方便命名的路径, 如 /usr/rtm/xv6/fs.c (fs.c)
//  + File SysCall: 文件系统调用 (sysfile.c, pipe.c, file.c, file.h)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [      0     |      1      | 2       31 | 32        44 |      45      | 46     1999 ]

// 硬盘文件系统格式
// 内核和用户程序都使用此头文件

#define ROOTINO 1  // 根目录inode编号
#define BSIZE 1024 // 块大小

#define FSMAGIC 0x10203040
struct superblock {
    uint magic;      // 魔数=FSMAGIC
    uint size;       // 文件系统总块数 (2000块)
    uint nblocks;    // 数据块数量 (1954块)
    uint ninodes;    // inode数量 (200项)
    uint nlog;       // 日志块数量 (30块)
    uint logstart;   // 第一个日志块的块号  (第2块)
    uint inodestart; // 第一个索引块的块号 (第32块)
    uint bmapstart;  // 第一个位图块的块号  (第45块)
};

#define NDIRECT 12                       // 直接块数量
#define NINDIRECT (BSIZE / sizeof(uint)) // 间接块数量
#define MAXFILE (NDIRECT + NINDIRECT)    // 最大文件大小 (块数)

// inode结构 (硬盘中) (大端序)
struct dinode {
    short type;              // 文件类型 (0:空闲 1:目录 2:文件 3:设备)
    short major;             // 主设备号
    short minor;             // 次设备号
    short nlink;             // 硬链接数
    uint size;               // 文件总大小 (字节)
    uint addrs[NDIRECT + 1]; // 文件所占有的块号 (直接块+间接索引块)
};

// 每个索引块 最多能包含的inode数量
#define IPB (BSIZE / sizeof(struct dinode))

// 根据超级块 计算第i个inode所在的块
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// 每个位图块 最多能够指示的块数
#define BPB (BSIZE * 8)

// 根据超级块 计算第b个块对应位所在的位图块
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart)

// 最大的文件名长度
#define DIRSIZ 14

// 需要给目录内追加的文件项
struct dirent {
    ushort inum;       // inode编号
    char name[DIRSIZ]; // 文件名称
};
