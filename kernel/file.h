
// 文件系统实现:
//  + FS.img: 文件系统映像 (mkfs.c)
//  + Dev+blockno: 虚拟硬盘块设备 (virtio_disk.c)
//  + Bcache: 缓存链环 (bio.c)
//  + Log: 多步更新的崩溃恢复 (log.c)
//  + Inode: inode分配器, 读取, 写入, 元数据 (fs.c)
//  + Directory: 具有特殊内容的inode(其他inode的列表) (fs.c)
//  + Path: 方便命名的路径, 如 /usr/rtm/xv6/fs.c (fs.c)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [          0 |           1 | 2       31 | 32        44 |           45 | 46     1999 ]

struct file {
    enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type; // 文件类型
    int ref;                                             // 引用计数
    char readable;                                       // 是否可读
    char writable;                                       // 是否可写
    struct pipe* pipe;                                   // 管道信息
    struct minode* ip;                                   // 内存inode信息
    uint off;                                            // 文件描述符偏移量
    short major;                                         // 主设备号
};

#define major(dev) ((dev) >> 16 & 0xFFFF)     // 获取主设备号 (高16位)
#define minor(dev) ((dev) & 0xFFFF)           // 获取次设备号 (低16位)
#define mkdev(m, n) ((uint)((m) << 16 | (n))) // 创建设备号 (主+次)

// inode结构体 (内存中)
struct minode {
    uint dev;  // 设备号(主+次)
    uint inum; // inode编号
    int ref;   // 引用计数

    struct sleeplock lock; // 保护下面的所有内容
    int valid;             // inode是否已从硬盘读取

    // inode (硬盘中的类型) (fs.h)
    short type;              // 文件类型 (0:空闲 1:目录 2:文件 3:设备)
    short major;             // 主设备号
    short minor;             // 次设备号
    short nlink;             // 硬链接数
    uint size;               // 文件总大小 (字节)
    uint addrs[NDIRECT + 1]; // 文件所占有的块号 (直接块+间接索引块)
};

// 将主设备号映射到设备的读写函数
struct devsw {
    int (*read)(int, uint64, int);
    int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

// 终端的主设备号 1
#define CONSOLE 1
