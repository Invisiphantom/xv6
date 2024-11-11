
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

// 内存文件系统格式

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

// 内存-索引项
typedef struct minode {
    uint dev;  // 设备号(主+次)
    uint inum; // 索引编号
    int ref;   // 引用计数

    sleeplock lock; // 同步睡眠锁
    int valid;      // inode是否已从硬盘读取

    // inode (硬盘中的类型) (fs.h)
    short type;              // 文件类型 (0:空闲 1:目录 2:文件 3:设备)
    short major;             // 主设备号
    short minor;             // 次设备号
    short nlink;             // 硬链接数
    uint size;               // 文件总大小 (字节)
    uint addrs[NDIRECT + 1]; // 文件所占有的块号 (直接块+间接引导块)
} minode;

// 将主设备号映射到设备的读写函数
struct devsw {
    int (*read)(int, uint64, int);
    int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

// 终端的主设备号 1
#define CONSOLE 1
