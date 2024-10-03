

struct buf {
    int valid;              // 数据是否已从硬盘读取
    int disk;               // 虚拟硬盘是否正在处理buf (virtio_disk.c)
    uint dev;               // 设备号
    uint blockno;           // 硬盘块号
    struct sleeplock lock;  // 睡眠锁, 用于同步
    uint refcnt;            // 引用计数
    struct buf* prev;       // LRU 缓存列表中的前一个缓冲链块(更新)
    struct buf* next;       // LRU 缓存列表中的后一个缓冲链块(更旧)
    uchar data[BSIZE];      // 缓冲链块数据
};
