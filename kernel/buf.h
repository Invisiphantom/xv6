

typedef struct buf {
    int valid;      // 数据是否有效
    uint dev;       // 设备号
    uint blockno;   // 硬盘块号
    uint refcnt;    // 引用计数
    uint disk;      // virtio是否正在处理
    sleeplock lock; // 同步睡眠锁

    struct buf* prev;  // 链环中的前块
    struct buf* next;  // 链环中的后块
    uchar data[BSIZE]; // 缓冲链块数据
} buf;
