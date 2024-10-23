

struct buf {
    int valid;             // 数据是否已从硬盘读取
    int disk;              // 虚拟硬盘是否正在处理buf (virtio_disk.c)
    uint dev;              // 设备号
    uint blockno;          // 硬盘块号
    struct sleeplock lock; // 睡眠锁, 用于同步
    uint refcnt;           // 引用计数
    struct buf* prev;      // 链环中的前块
    struct buf* next;      // 链环中的后块
    uchar data[BSIZE];     // 缓冲链块数据
};
