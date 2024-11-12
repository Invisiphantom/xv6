
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
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "proc.h"

// 主设备号->设备读写函数
struct devsw devsw[NDEV];

// 文件描述符表
struct {
    spinlock lock;
    file file[NFILE];
} ftable;

void fileinit(void) { initlock(&ftable.lock, "ftable"); }

// ----------------------------------------------------------------

// 分配空闲描述符
file* filealloc(void)
{
    acquire(&ftable.lock); //* 获取描述符表锁
    for (file* f = ftable.file; f < ftable.file + NFILE; f++) {
        if (f->ref == 0) {
            f->ref = 1;
            release(&ftable.lock); //* 释放描述符表锁
            return f;
        }
    }

    panic("filealloc");
}

// ----------------------------------------------------------------

// 增加描述符的引用计数
// int dup(int fd)
file* filedup(file* f)
{
    acquire(&ftable.lock); //* 获取描述符表锁
    if (f->ref <= 0)
        panic("filedup");
    f->ref++;
    release(&ftable.lock); //* 释放描述符表锁
    return f;
}

// 减少描述符的引用计数
// int close(int fd)
void fileclose(file* f)
{
    acquire(&ftable.lock); //* 获取描述符表锁
    if (f->ref <= 0)
        panic("fileclose");

    // 减少引用计数
    f->ref--;

    // 如果还有引用, 则返回
    if (f->ref >= 1) {
        release(&ftable.lock); //* 释放描述符表锁
        return;
    }

    // 关闭描述符
    file ff = *f;
    f->ref = 0;
    f->type = FD_NONE;
    release(&ftable.lock); //* 释放描述符表锁

    // 如果是管道, 则关闭
    if (ff.type == FD_PIPE) {
        pipeclose(ff.pipe, ff.writable);
    }
    // 如果是索引项, 则减少引用
    else if (ff.type == FD_INODE || ff.type == FD_DEVICE) {
        begin_op();   //* 事务开始
        iput(ff.mip); // 减少索引的引用计数
        end_op();     //* 事务结束
    }
}

// ----------------------------------------------------------------

// 获取描述符对应索引的状态信息
// int fstat(int fd, struct stat *st)
int filestat(file* f, uint64 addr)
{
    struct proc* p = myproc();
    struct stat st;

    if (f->type == FD_INODE || f->type == FD_DEVICE) {
        ilock(f->mip);      //* 获取inode锁 (休眠)
        stati(f->mip, &st); // 索引信息 inode=>stat
        iunlock(f->mip);    //* 释放inode锁 (唤醒)

        // 内核-st=>用户-addr
        if (copyout(p->pagetable, addr, (char*)&st, sizeof(st)) < 0)
            return -1;
        return 0;
    }

    return -1;
}

// ----------------------------------------------------------------

// 文件内容: 内核空间=>用户空间
// int read(int fd, char *buf, int n)
int fileread(file* f, uint64 addr, int n)
{
    // 确保文件可读
    if (f->readable == false)
        return -1;

    int r = 0;
    switch (f->type) {
        case FD_PIPE:
            r = piperead(f->pipe, addr, n);
            break;

        case FD_DEVICE:
            if (f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
                return -1;
            r = devsw[f->major].read(true, addr, n); // consoleread
            break;

        case FD_INODE:
            ilock(f->mip); //* 获取inode锁 (休眠)

            // 读取文件内容
            r = readi(f->mip, true, addr, f->off, n);

            // 更新描述符偏移量
            if (r > 0)
                f->off += r;

            iunlock(f->mip); //* 释放inode锁 (唤醒)
            break;

        default:
            panic("fileread: unknown file type");
            break;
    }
    return r;
}

// 文件内容: 用户空间=>内核空间
// int write(int fd, char *buf, int n)
int filewrite(file* f, uint64 addr, int n)
{
    // 确保文件可写
    if (f->writable == false)
        return -1;

    int ret = 0;
    switch (f->type) {
        case FD_PIPE:
            ret = pipewrite(f->pipe, addr, n);
            break;

        case FD_DEVICE:
            if (f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
                return -1;
            ret = devsw[f->major].write(true, addr, n); // consolewrite
            break;

        case FD_INODE:
            // log_write 为什么要除以二?
            int op_maxlen = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;

            int i = 0;
            while (i < n) {
                int nn = n - i;
                if (nn > op_maxlen)
                    nn = op_maxlen;

                begin_op(); //* 事务开始
                ilock(f->mip); //** 获取inode锁 (休眠)

                int r = writei(f->mip, true, addr + i, f->off, nn);
                if (r > 0)
                    f->off += r;

                iunlock(f->mip); //** 释放inode锁 (唤醒)
                end_op(); //* 事务结束

                if (r != nn)
                    panic("filewrite");

                i += r;
            }
            ret = (i == n ? n : -1);
            break;

        default:
            panic("filewrite: unknown file type");
            break;
    }

    return ret;
}
