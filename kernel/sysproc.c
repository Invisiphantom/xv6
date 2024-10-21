#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

// int exit(int status)
uint64 sys_exit(void)
{
    int status;
    argint(0, &status);

    exit(status);
    
    return 0; // not reached
}

// int getpid()
uint64 sys_getpid(void) { return myproc()->pid; }

// int fork()
uint64 sys_fork(void) { return fork(); }

// int wait(int *status)
uint64 sys_wait(void)
{
    uint64 status;
    argaddr(0, &status);

    return wait(status);
}

// char *sbrk(int n)
uint64 sys_sbrk(void)
{
    int n;
    argint(0, &n);

    uint64 addr = myproc()->sz;
    if (growproc(n) < 0)
        return -1;
    return addr;
}

// int sleep(int n)
uint64 sys_sleep(void)
{
    int n;
    argint(0, &n);
    if (n < 0)
        n = 0;

    acquire(&tickslock); // 获取锁

    // 记录当前时钟中断计数
    uint ticks0 = ticks;

    // 等待n个时钟中断
    while (ticks - ticks0 < n) {
        // 如果进程被终止, 则返回-1
        if (killed(myproc())) {
            release(&tickslock);
            return -1;
        }
        // 释放锁 并在ticks上休眠
        sleep(&ticks, &tickslock);
    }

    release(&tickslock); // 释放锁

    return 0;
}

// int kill(int pid)
uint64 sys_kill(void)
{
    int pid;
    argint(0, &pid);

    return kill(pid);
}

// int uptime()
// 返回已发生的时钟中断次数
uint64 sys_uptime(void)
{
    uint xticks;

    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);
    
    return xticks;
}
