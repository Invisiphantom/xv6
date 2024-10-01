
// 互斥自旋锁
struct spinlock {
    uint locked;  // 锁是否被占用

    // debug
    char* name;       // 锁名称
    struct cpu* cpu;  // 占用锁的CPU
};
