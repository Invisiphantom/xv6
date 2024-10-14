
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

// 初始化自旋锁
void initlock(struct spinlock* lk, char* name)
{
    lk->name = name; // 锁名称
    lk->locked = 0;
    lk->cpu = 0;
}

// 不断循环直到获取自旋锁
void acquire(struct spinlock* lk)
{
    // 关闭中断, 避免死锁
    push_off();

    // 确保当前CPU未持有该锁
    if (holding(lk))
        panic("acquire");

    // a5 = 1
    // s1 = &lk->locked
    // amoswap.w.aq a5, a5, (s1)   <原子交换操作>
    // aq: 该指令后序所有访存 必须等该指令完成后才开始执行
    while (__sync_lock_test_and_set(&lk->locked, 1) != 0)
        ;

    // 告诉gcc和CPU不要将 前后的内存操作越过此处
    // 以确保关键区的内存使用 严格在锁被获取之后发生
    __sync_synchronize(); // 内存屏障

    // 记录当前CPU信息到锁中 (用于调试)
    lk->cpu = mycpu();
}

// 释放自旋锁
void release(struct spinlock* lk)
{
    // 确保当前CPU持有该锁
    if (!holding(lk))
        panic("release");

    // 清除锁中的CPU信息
    lk->cpu = 0;

    // 告诉gcc和CPU不要将 前后的内存操作越过此处
    // 以确保关键区的内存使用 严格在释放锁之前发生
    __sync_synchronize(); // 内存屏障

    // s1 = &lk->locked
    // amoswap.w zero, zero, (s1)  <原子交换操作>
    __sync_lock_release(&lk->locked);

    // 重新开启中断
    pop_off();
}

// 检查当前CPU是否持有锁
inline int holding(struct spinlock* lk)
{
    int r = (lk->locked && lk->cpu == mycpu());
    return r;
}

// 增加中断禁用计数
void push_off(void)
{
    // 记录当前中断状态
    int old = intr_get();

    // 关闭中断
    intr_off();

    // 如果是第一次增加计数, 则记录原有中断状态
    if (mycpu()->off_num == 0)
        mycpu()->intr_enable = old;

    // 增加计数
    mycpu()->off_num += 1;
}

// 减少中断禁用计数
void pop_off(void)
{
    struct cpu* c = mycpu();

    // 确保此时中断关闭 且计数大于0
    if (intr_get())
        panic("pop_off - interruptible");
    if (c->off_num < 1)
        panic("pop_off");

    // 减少计数
    c->off_num -= 1;

    // 如果是最后一次减少计数, 则恢复原有中断状态
    if (c->off_num == 0 && c->intr_enable)
        intr_on();
}
