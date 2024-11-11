// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

void initsleeplock(struct sleeplock* lk, char* name)
{
    initlock(&lk->lk, "sleep lock");
    lk->name = name;
    lk->locked = 0;
    lk->pid = 0;
}

// 获取锁 如果锁被占用则休眠
void acquiresleep(struct sleeplock* lk)
{
    acquire(&lk->lk); //*

    while (lk->locked)
        sleep(lk, &lk->lk); //*

    lk->locked = 1;
    lk->pid = myproc()->pid;

    release(&lk->lk); //*
}

// 释放锁 唤醒等待锁的进程
void releasesleep(struct sleeplock* lk)
{
    acquire(&lk->lk); //*

    lk->locked = 0;
    lk->pid = 0;

    wakeup(lk); // 唤醒等待锁的进程

    release(&lk->lk); //*
}

// 检查当前进程是否持有锁
int holdingsleep(struct sleeplock* lk)
{
    acquire(&lk->lk); //*
    int r = lk->locked && (lk->pid == myproc()->pid);
    release(&lk->lk); //*
    return r;
}
