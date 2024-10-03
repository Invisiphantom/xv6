
// 文件系统实现:
//  + FS.img: 文件系统映像 (mkfs.c)
//  + Dev+blockno: 虚拟硬盘块设备 (virtio_disk.c)
//  + Bcache: 缓存链环 (bio.c)
//  + Log: 多步更新的崩溃恢复 (log.c)
//  + Inodes: inode分配器, 读取, 写入, 元数据 (fs.c)
//  + Directories: 具有特殊内容的inode(其他inode的列表) (fs.c)
//  + PathNames: 方便命名的路径, 如 /usr/rtm/xv6/fs.c (fs.c)

// 硬盘文件系统格式
// 内核和用户程序都使用此头文件

#define ROOTINO 1   // 根目录inode编号
#define BSIZE 1024  // 块大小

// 硬盘布局
// [ boot block | super block | log | inode blocks | free bit map | data blocks ]
#define FSMAGIC 0x10203040
struct superblock {
    uint magic;       // 魔数=FSMAGIC
    uint size;        // 文件系统总块数
    uint nblocks;     // 数据块数量
    uint ninodes;     // inode数量
    uint nlog;        // 日志块数量
    uint logstart;    // 第一个日志块的块号
    uint inodestart;  // 第一个inode块的块号
    uint bmapstart;   // 第一个位图块的块号
};

#define NDIRECT 12                        // 直接块数量
#define NINDIRECT (BSIZE / sizeof(uint))  // 间接块数量
#define MAXFILE (NDIRECT + NINDIRECT)     // 最大文件大小 (块数)

// 硬盘上的inode结构 (大端序)
struct dinode {
    short type;               // 文件类型
    short major;              // 主设备号
    short minor;              // 次设备号
    short nlink;              // 硬链接数
    uint size;                // 文件总大小 (字节)
    uint addrs[NDIRECT + 1];  // 文件所占有的块号 (直接块+间接索引块)
};

// 每个inode块 最多能包含的inode数量
#define IPB (BSIZE / sizeof(struct dinode))

// 根据超级块 计算第i个inode所在的块
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// 每个位图块 最多能够指示的块数
#define BPB (BSIZE * 8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart)

// 最大的文件名长度
#define DIRSIZ 14

// 需要给目录内追加的文件项
struct dirent {
    ushort inum;        // inode编号
    char name[DIRSIZ];  // 文件名称
};
