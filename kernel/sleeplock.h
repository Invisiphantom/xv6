
// 睡眠锁
typedef struct sleeplock {
    char* name;  // 锁名称
    uint locked; // 是否持有
    int pid;     // 持有锁的进程
    spinlock lk; // 用自旋锁保护变量
} sleeplock;
