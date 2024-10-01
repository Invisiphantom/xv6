

struct file {
    enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;  // 文件类型
    int ref;                                              // 引用计数
    char readable;                                        // 可读
    char writable;                                        // 可写
    struct pipe* pipe;                                    // FD_PIPE
    struct inode* ip;                                     // FD_INODE and FD_DEVICE
    uint off;                                             // FD_INODE
    short major;                                          // FD_DEVICE
};

#define major(dev) ((dev) >> 16 & 0xFFFF)      // 获取主设备号 (高16位)
#define minor(dev) ((dev) & 0xFFFF)            // 获取次设备号 (低16位)
#define mkdev(m, n) ((uint)((m) << 16 | (n)))  // 创建设备号 (高+低)

// 内存中的inode结构体
struct inode {
    uint dev;   // 设备号(主+次)
    uint inum;  // inode编号
    int ref;    // 引用计数

    struct sleeplock lock;  // 保护下面的所有内容
    int valid;              // inode是否已经从硬盘读取

    // 硬盘inode的类型拷贝(fs.h)
    short type;               // 文件类型
    short major;              // 主设备号
    short minor;              // 次设备号
    short nlink;              // 硬链接数
    uint size;                // 文件大小
    uint addrs[NDIRECT + 1];  // 数据块地址
};

// 将主设备号映射到设备的读写函数
struct devsw {
    int (*read)(int, uint64, int);
    int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

// 终端的主设备号 1
#define CONSOLE 1
