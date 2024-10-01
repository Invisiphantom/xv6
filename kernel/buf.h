

struct buf {
    int valid;              // 数据是否已从磁盘读取
    int disk;               // 磁盘是否拥有buf
    uint dev;               // 设备号
    uint blockno;           // 磁盘块号
    struct sleeplock lock;  // 睡眠锁，用于同步
    uint refcnt;            // 引用计数
    struct buf* prev;       // LRU 缓存列表中的前一个缓冲区(更新)
    struct buf* next;       // LRU 缓存列表中的后一个缓冲区(更旧)
    uchar data[BSIZE];      // 缓冲区数据
};
