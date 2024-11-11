
// 自旋锁
typedef struct spinlock {
    char* name;      // 锁名称
    uint locked;     // 是否持有
    struct cpu* cpu; // 持有锁的CPU
} spinlock;
