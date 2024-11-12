
// 文件系统实现:
//  + UART: 串口输入输出 (printf.c console.c uart.c)
//  -------------------------------------------------
//  + FS.img: 文件系统映像 (mkfs.c)
//  + VirtIO: 虚拟硬盘驱动 (virtio.h virtio_disk.c)
//  + BCache: LRU缓存链环 (buf.h bio.c)
//  + Log: 两步提交的日志系统 (log.c)
//  + Inode Dir Path: 硬盘文件系统实现 (stat.h fs.h fs.c)
//  + Pipe: 管道实现 (pipe.c)
//  + File Descriptor: 文件描述符 (file.h file.c)
//  + File SysCall: 文件系统调用 (fcntl.h sysfile.c)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [      0     |      1      | 2       31 | 32        44 |      45      | 46     1999 ]

typedef struct file {
    // 文件描述符类型
    enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;

    int ref;       // 引用计数
    char readable; // 是否可读
    char writable; // 是否可写

    uint off;           // 偏移量
    short major;        // 主设备号
    struct minode* mip; // 索引信息
    struct pipe* pipe;  // 管道信息
} file;

#define major(dev) ((dev) >> 16 & 0xFFFF)     // 获取主设备号 (高16位)
#define minor(dev) ((dev) & 0xFFFF)           // 获取次设备号 (低16位)
#define mkdev(m, n) ((uint)((m) << 16 | (n))) // 创建设备号 (主+次)

// 内存-索引项
typedef struct minode {
    sleeplock lock; // 同步睡眠锁

    uint dev;  // 设备号(主+次)
    uint inum; // 索引编号
    int ref;   // 引用计数
    int valid; // 有效位

    // 硬盘-索引项
    short type;              // 索引类型 (stat.h)
    short major;             // 主设备号
    short minor;             // 次设备号
    short nlink;             // 硬链接数
    uint size;               // 文件大小 (字节)
    uint addrs[NDIRECT + 1]; // 文件块号 (直接块+间接引导块)
} minode;

// 终端设备
struct devsw {
    int (*read)(int user_dst, uint64 dst, int n);  // consoleread
    int (*write)(int user_src, uint64 src, int n); // consolewrite
};

extern struct devsw devsw[];

// 终端的主设备号 (1)
#define CONSOLE 1
