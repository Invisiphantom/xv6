#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "proc.h"

// 主设备号->设备读写函数
struct devsw devsw[NDEV];

struct {
    struct spinlock lock;
    struct file file[NFILE];
} ftable;

void fileinit(void) { initlock(&ftable.lock, "ftable"); }

// Allocate a file structure.
struct file* filealloc(void)
{
    struct file* f;

    acquire(&ftable.lock);
    for (f = ftable.file; f < ftable.file + NFILE; f++) {
        if (f->ref == 0) {
            f->ref = 1;
            release(&ftable.lock);
            return f;
        }
    }
    release(&ftable.lock);
    return 0;
}

// 增加文件f的引用计数
struct file* filedup(struct file* f)
{
    acquire(&ftable.lock); // 获取锁

    // 确保文件存在引用
    if (f->ref <= 0)
        panic("filedup");
    // 增加引用计数
    f->ref++;

    release(&ftable.lock); // 释放锁

    return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void fileclose(struct file* f)
{
    struct file ff;

    acquire(&ftable.lock);
    if (f->ref < 1)
        panic("fileclose");
    if (--f->ref > 0) {
        release(&ftable.lock);
        return;
    }
    ff = *f;
    f->ref = 0;
    f->type = FD_NONE;
    release(&ftable.lock);

    if (ff.type == FD_PIPE) {
        pipeclose(ff.pipe, ff.writable);
    } else if (ff.type == FD_INODE || ff.type == FD_DEVICE) {
        begin_op();
        iput(ff.ip);
        end_op();
    }
}

// 获取文件f的元数据
// 将文件f的元数据写入addr指向的用户虚拟地址
int filestat(struct file* f, uint64 addr)
{
    struct proc* p = myproc();
    struct stat st;

    if (f->type == FD_INODE || f->type == FD_DEVICE) {
        ilock(f->ip);      // 获取inode锁
        stati(f->ip, &st); // 拷贝inode信息到stat结构体
        iunlock(f->ip);    // 释放inode锁
        // 将stat结构体写入用户空间
        if (copyout(p->pagetable, addr, (char*)&st, sizeof(st)) < 0)
            return -1;
        return 0;
    }
    return -1;
}

// 从文件f读取n个字节到用户空间addr
// sysfile.c->sys_read() 调用此函数
int fileread(struct file* f, uint64 addr, int n)
{
    int r = 0;

    // 如果文件不可读
    if (f->readable == 0)
        return -1;

    // 如果是管道文件
    if (f->type == FD_PIPE) {
        r = piperead(f->pipe, addr, n);
    }

    // 如果是设备文件
    else if (f->type == FD_DEVICE) {
        // 确保设备号合法, 并且设备有读函数
        if (f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
            return -1;
        // 跳转到console.c->consoleread()
        r = devsw[f->major].read(1, addr, n);
    }

    // 如果是普通文件
    else if (f->type == FD_INODE) {
        ilock(f->ip); // 获取锁

        if ((r = readi(f->ip, true, addr, f->off, n)) > 0)
            f->off += r; // 更新文件描述符偏移量

        iunlock(f->ip); // 释放锁
    }

    else
        panic("fileread");

    return r;
}

// 将用户空间addr的n个字节写入文件f
int filewrite(struct file* f, uint64 addr, int n)
{
    int r, ret = 0;

    // 确保文件可写
    if (f->writable == 0)
        return -1;

    // 如果是管道文件
    if (f->type == FD_PIPE) {
        ret = pipewrite(f->pipe, addr, n);
    }

    // 如果是设备文件
    else if (f->type == FD_DEVICE) {
        if (f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
            return -1;
        // user_src=1, src=addr, n=n
        // 跳转到console.c->consolewrite()
        ret = devsw[f->major].write(1, addr, n);
    }

    // 如果是普通文件
    else if (f->type == FD_INODE) {
        // write a few blocks at a time to avoid exceeding
        // the maximum log transaction size, including
        // i-node, indirect block, allocation blocks,
        // and 2 blocks of slop for non-aligned writes.
        // this really belongs lower down, since writei()
        // might be writing a device like the console.
        // 每次只写入几个块, 以避免超过最大日志事务大小
        // 包括inode, 间接块, 分配块, 以及2个块的空间用于非对齐写入

        int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;

        int i = 0;
        while (i < n) {
            int n1 = n - i;
            if (n1 > max)
                n1 = max;

            begin_op(); //*

            ilock(f->ip); //**

            if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
                f->off += r;

            iunlock(f->ip); //**

            end_op(); //*

            if (r != n1) {
                // error from writei
                break;
            }

            i += r;
        }

        ret = (i == n ? n : -1);
    }

    else
        panic("filewrite: unknown file type");

    return ret;
}
