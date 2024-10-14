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
    int n;
    argint(0, &n);
    exit(n);
    return 0; // not reached
}

// int getpid()
uint64 sys_getpid(void) { return myproc()->pid; }

// int fork()
uint64 sys_fork(void) { return fork(); }

// int wait(int *status)
uint64 sys_wait(void)
{
    uint64 p;
    // 获取第0个参数
    argaddr(0, &p);
    return wait(p);
}

// char *sbrk(int n)
uint64 sys_sbrk(void)
{
    uint64 addr;
    int n;

    argint(0, &n);
    addr = myproc()->sz;
    if (growproc(n) < 0)
        return -1;
    return addr;
}

// int sleep(int n)
uint64 sys_sleep(void)
{
    int n;
    uint ticks0;

    argint(0, &n);
    if (n < 0)
        n = 0;

    acquire(&tickslock);
    ticks0 = ticks;
    while (ticks - ticks0 < n) {
        if (killed(myproc())) {
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);

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
