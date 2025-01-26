
// 文件系统实现:
//  + UART: 串口输入输出 (printf.c console.c uart.c)
//  -------------------------------------------------
//  + FS.img: 文件系统映像 (mkfs.c)
//  + VirtIO: 虚拟硬盘驱动 (virtio.h virtio_disk.c)
//  + BCache: LRU缓存链环 (buf.h bio.c)
//  + Log: 两步提交的日志系统 (log.c)
//  + Inode Dir Path: 硬盘文件系统实现 (stat.h fs.h fs.c)
//  + Pipe: 管道实现 (pipe.c)
//  + File Descriptor: 文件描述符 (file.h file.c)
//  + File SysCall: 文件系统调用 (fcntl.h sysfile.c)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [      0     |      1      | 2       31 | 32        44 |      45      | 46     1999 ]

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
    struct spinlock lock;
    char data[PIPESIZE];

    int readopen;  // 读端是否打开
    int writeopen; // 写端是否打开

    uint nread;  // 读位置
    uint nwrite; // 写位置
};

int pipealloc(file** rf, file** wf)
{
    *rf = *wf = NULL;
    struct pipe* pi = NULL;

    if ((*rf = filealloc()) == NULL || (*wf = filealloc()) == NULL)
        panic("pipealloc");
    if ((pi = (struct pipe*)kalloc()) == NULL)
        panic("pipealloc");

    initlock(&pi->lock, "pipe");
    pi->readopen = true;
    pi->writeopen = true;
    pi->nread = 0;
    pi->nwrite = 0;

    (*rf)->type = FD_PIPE;
    (*rf)->readable = 1;
    (*rf)->writable = 0;
    (*rf)->pipe = pi;

    (*wf)->type = FD_PIPE;
    (*wf)->readable = 0;
    (*wf)->writable = 1;
    (*wf)->pipe = pi;
    return 0;
}

// 关闭管道的读端或写端
void pipeclose(struct pipe* pi, int writable)
{
    acquire(&pi->lock); //* 获取管道锁

    if (writable) {
        pi->writeopen = false;
        wakeup(&pi->nread);
    } else {
        pi->readopen = false;
        wakeup(&pi->nwrite);
    }

    if (pi->readopen == false && pi->writeopen == false) {
        release(&pi->lock); //* 释放管道锁
        kfree((char*)pi);   // 释放管道内存
    } else
        release(&pi->lock); //* 释放管道锁
}

// 向管道写入数据
int pipewrite(struct pipe* pi, uint64 addr, int n)
{
    int i = 0;
    struct proc* pr = myproc();

    acquire(&pi->lock); //* 获取管道锁

    while (i < n) {
        // 如果读端已关闭 或者进程有终止标志
        if (pi->readopen == false || killed(pr)) {
            release(&pi->lock); //* 释放管道锁
            return -1;
        }

        // 如果管道已满
        if (pi->nwrite == pi->nread + PIPESIZE) {
            wakeup(&pi->nread);            // 唤醒读端
            sleep(&pi->nwrite, &pi->lock); //* 休眠写端
        }

        else {
            char ch; // 字符: 内核空间<==用户空间
            if (copyin(pr->pagetable, &ch, addr + i, 1) == -1)
                break;
            pi->data[pi->nwrite % PIPESIZE] = ch;
            pi->nwrite++;
            i++;
        }
    }

    wakeup(&pi->nread); // 唤醒读端
    release(&pi->lock); //* 释放管道锁
    return i;
}

// 从管道读取数据
int piperead(struct pipe* pi, uint64 addr, int n)
{
    struct proc* pr = myproc();
    acquire(&pi->lock); //* 获取管道锁

    // 休眠等待写端填充数据
    while (pi->nread == pi->nwrite && pi->writeopen) {
        if (killed(pr)) {
            release(&pi->lock); //* 释放管道锁
            return -1;
        }
        sleep(&pi->nread, &pi->lock); //* 休眠等待写入
    }

    int i;
    for (i = 0; i < n; i++) {
        if (pi->nread == pi->nwrite)
            break;
        char ch = pi->data[pi->nread % PIPESIZE];
        pi->nread++; // 字符: 用户空间<==内核空间
        if (copyout(pr->pagetable, addr + i, &ch, 1) == -1)
            break;
    }

    wakeup(&pi->nwrite); // 唤醒写端
    release(&pi->lock);  //* 释放管道锁
    return i;
}
