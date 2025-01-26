
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
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "fcntl.h"

// 获取第n个系统调用参数 (文件描述符)
// 返回文件描述符和对应的文件结构体
// n:参数索引  pdf:返回文件描述符值  pf:返回文件结构体
static int argfd(int n, int* pfd, struct file** pf)
{
    int fd;
    argint(n, &fd);

    struct file* f;
    if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
        return -1;

    if (pfd)
        *pfd = fd;

    if (pf)
        *pf = f;

    return 0;
}

// 对于给定的文件 分配一个文件描述符
static int fdalloc(struct file* f)
{
    struct proc* p = myproc();

    // 遍历进程的文件描述符表, 分配空闲文件描述符
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->ofile[fd] == 0) {
            p->ofile[fd] = f;
            return fd;
        }
    }
    return -1;
}

// int dup(int fd)
uint64 sys_dup(void)
{
    int fd;

    // 获取对应文件结构体
    struct file* f;
    if (argfd(0, 0, &f) < 0)
        return -1;

    // 分配 指向相同文件结构体的 新文件描述符
    if ((fd = fdalloc(f)) < 0)
        return -1;

    // 增加引用计数
    filedup(f);

    return fd;
}

// 从文件描述符fd中读取n字节到buf
// 返回读取的字节数, EOF返回0
// int read(int fd, char *buf, int n)
uint64 sys_read(void)
{
    struct file* f;
    if (argfd(0, 0, &f) < 0)
        return -1;

    uint64 buf;
    argaddr(1, &buf);

    int n;
    argint(2, &n);

    return fileread(f, buf, n);
}

// int write(int fd, char *buf, int n)
uint64 sys_write(void)
{
    struct file* f;
    uint64 buf;
    int n;

    if (argfd(0, 0, &f) < 0)
        return -1;
    argaddr(1, &buf);
    argint(2, &n);

    return filewrite(f, buf, n);
}

// int close(int fd)
uint64 sys_close(void)
{
    int fd;
    struct file* f;

    if (argfd(0, &fd, &f) < 0)
        return -1;
    myproc()->ofile[fd] = 0;
    fileclose(f);
    return 0;
}

// int fstat(int fd, struct stat *st)
uint64 sys_fstat(void)
{
    struct file* f;
    uint64 st; // user pointer to struct stat

    argaddr(1, &st);
    if (argfd(0, 0, &f) < 0)
        return -1;
    return filestat(f, st);
}

// int link(char *old, char *new)
// 从已有的文件路径old 创建新的路径new 指向相同的inode
uint64 sys_link(void)
{
    char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
    struct minode *dp, *ip;

    // 获取第一个参数old, 第二个参数new
    if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
        return -1;

    begin_op(); //* 事务开始

    // 如果old对应的inode不存在, 则返回-1
    if ((ip = namei(old)) == 0) {
        end_op(); //* 事务结束
        return -1;
    }

    ilock(ip); // 获取inode锁

    // 如果old是目录, 则返回-1
    if (ip->type == I_DIR) {
        iunlockput(ip);
        end_op(); //* 事务结束
        return -1;
    }

    ip->nlink++; // 硬链接数+1

    iupdate(ip); // 更新inode信息
    iunlock(ip); // 释放inode锁

    // 获取new的父目录inode
    if ((dp = nameiparent(new, name)) == 0)
        goto bad;

    ilock(dp); // 获取父目录inode锁

    // 如果所处设备不同, 或者创建链接失败, 则返回-1
    if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
        iunlockput(dp);
        goto bad;
    }

    iunlockput(dp); // 释放父目录inode锁
    iput(ip);       // 释放inode锁
    end_op();       //* 事务结束

    return 0;

bad:
    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);
    end_op(); //* 事务结束
    return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int isdirempty(struct minode* dp)
{
    int off;
    struct dirent de;

    for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
        if (readi(dp, false, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("isdirempty: readi");
        if (de.inum != 0)
            return 0;
    }
    return 1;
}

// int unlink(char *file)
// 移除硬链接 (系统调用)
uint64 sys_unlink(void)
{
    struct minode *ip, *dp;
    struct dirent de;
    char name[DIRSIZ], path[MAXPATH];
    uint off;

    // 获取第一个参数path
    if (argstr(0, path, MAXPATH) < 0)
        return -1;

    begin_op(); //* 事务开始

    // 获取path的父目录inode, 记为dp
    if ((dp = nameiparent(path, name)) == 0) {
        end_op(); //* 事务结束
        return -1;
    }

    ilock(dp); // 获取父目录inode锁

    // "." or ".." 不能被删除
    if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
        goto bad;

    // 获取path对应的inode, 记为ip
    if ((ip = dirlookup(dp, name, &off)) == 0)
        goto bad;

    ilock(ip); // 获取inode锁

    if (ip->nlink < 1)
        panic("unlink: nlink < 1");

    // 如果ip是目录, 且不为空, 则返回-1
    if (ip->type == I_DIR && !isdirempty(ip)) {
        iunlockput(ip);
        goto bad;
    }

    memset(&de, 0, sizeof(de)); // 清空初始化目录项
    if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        panic("unlink: writei");

    // 如果ip是目录, 则dp的硬链接数-1
    if (ip->type == I_DIR) {
        dp->nlink--;
        iupdate(dp);
    }
    iunlockput(dp);

    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);

    end_op(); //* 事务结束

    return 0;

bad:
    iunlockput(dp);
    end_op(); //* 事务结束
    return -1;
}

// 创建inode
static minode* create(char* path, short type, short major, short minor)
{
    char name[DIRSIZ];

    minode* dp; // 获取path对应父目录的inode
    if ((dp = nameiparent(path, name)) == 0)
        return 0;

    ilock(dp); //* 锁定父目录inode

    minode* ip; // 确保路径对应文件 当前不存在
    if ((ip = dirlookup(dp, name, 0)) != NULL) {
        iunlockput(dp);
        ilock(ip);
        if (type == I_FILE && (ip->type == I_FILE || ip->type == I_DEVICE))
            return ip;
        iunlockput(ip);
        return 0;
    }

    // 分配新的inode
    if ((ip = ialloc(dp->dev, type)) == NULL) {
        iunlockput(dp);
        return 0;
    }

    ilock(ip); //* 锁定新inode
    ip->major = major;
    ip->minor = minor;
    ip->nlink = 1;
    iupdate(ip); // 将inode写回硬盘

    // 如果创建的是文件夹
    // 创建 . 和 .. 文件项
    if (type == I_DIR)
        if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
            goto fail;

    // 向父目录添加新inode
    if (dirlink(dp, name, ip->inum) < 0)
        goto fail;

    // 如果创建的是文件夹
    // 增加其父目录的引用 (..)
    if (type == I_DIR) {
        dp->nlink++;
        iupdate(dp);
    }

    iunlockput(dp); //* 释放父目录inode

    return ip;

fail:
    // something went wrong. de-allocate ip.
    ip->nlink = 0;
    iupdate(ip);
    iunlockput(ip);
    iunlockput(dp);
    return 0;
}

// 打开文件, 返回文件描述符
// int open(char *file, int flags)
uint64 sys_open(void)
{
    char path[MAXPATH];
    int fd, flags;
    struct file* f;
    minode* mip;
    int n;

    argint(1, &flags);
    if ((n = argstr(0, path, MAXPATH)) < 0)
        return -1;

    begin_op(); //* 事务开始

    // 如果有创建标志
    if (flags & O_CREATE) {
        mip = create(path, I_FILE, 0, 0);
        if (mip == 0) {
            end_op(); //* 事务结束
            return -1;
        }
    }

    else {
        if ((mip = namei(path)) == 0) {
            end_op(); //* 事务结束
            return -1;
        }
        ilock(mip);
        if (mip->type == I_DIR && flags != O_RDONLY) {
            iunlockput(mip);
            end_op(); //* 事务结束
            return -1;
        }
    }

    // 确保设备号合法
    if (mip->type == I_DEVICE && (mip->major < 0 || mip->major >= NDEV)) {
        iunlockput(mip);
        end_op(); //* 事务结束
        return -1;
    }

    if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
        if (f)
            fileclose(f);
        iunlockput(mip);
        end_op(); //* 事务结束
        return -1;
    }

    if (mip->type == I_DEVICE) {
        f->type = FD_DEVICE;
        f->major = mip->major;
    } else {
        f->type = FD_INODE;
        f->off = 0;
    }

    f->mip = mip;
    f->readable = !(flags & O_WRONLY);
    f->writable = (flags & O_WRONLY) || (flags & O_RDWR);

    if ((flags & O_TRUNC) && mip->type == I_FILE) {
        itrunc(mip);
    }

    iunlock(mip);
    end_op(); //* 事务结束

    return fd;
}

// int mkdir(char *dir)
uint64 sys_mkdir(void)
{
    char path[MAXPATH];
    struct minode* ip;

    begin_op(); //* 事务开始
    if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, I_DIR, 0, 0)) == 0) {
        end_op(); //* 事务结束
        return -1;
    }
    iunlockput(ip);
    end_op(); //* 事务结束
    return 0;
}

// int mknod(char *file, int mode, int dev)
uint64 sys_mknod(void)
{
    struct minode* ip;
    char path[MAXPATH];
    int major, minor;

    begin_op(); //* 事务开始
    argint(1, &major);
    argint(2, &minor);
    if ((argstr(0, path, MAXPATH)) < 0 || (ip = create(path, I_DEVICE, major, minor)) == 0) {
        end_op(); //* 事务结束
        return -1;
    }
    iunlockput(ip);
    end_op(); //* 事务结束
    return 0;
}

// int chdir(const char *dir)
uint64 sys_chdir(void)
{
    char path[MAXPATH];
    struct minode* ip;
    struct proc* p = myproc();

    begin_op(); //* 事务开始
    if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
        end_op(); //* 事务结束
        return -1;
    }
    ilock(ip);
    if (ip->type != I_DIR) {
        iunlockput(ip);
        end_op(); //* 事务结束
        return -1;
    }
    iunlock(ip);
    iput(p->cwd);
    end_op(); //* 事务结束
    p->cwd = ip;
    return 0;
}

// int exec(char *file, char *argv[])
uint64 sys_exec(void)
{
    char path[MAXPATH], *argv[MAXARG];

    // 将trapframe->a1的值 写入uargv
    uint64 uargv;
    argaddr(1, &uargv);

    // 将trapframe->a0的值 作为字符串写入path
    if (argstr(0, path, MAXPATH) < 0)
        return -1;

    // 开始解析参数
    memset(argv, 0, sizeof(argv));
    for (int i = 0;; i++) {
        // 确保参数个数不超过最大值
        if (i >= NELEM(argv))
            goto bad;

        // 从用户地址 获取第i个参数, 并将其值写入uarg
        uint64 uarg;
        if (fetchaddr(uargv + sizeof(uint64) * i, (uint64*)&uarg) < 0)
            goto bad;

        // 如果uarg为0 则表示参数解析完毕
        if (uarg == 0) {
            argv[i] = 0;
            break;
        }

        // 分配新内存
        argv[i] = kalloc();
        if (argv[i] == 0)
            goto bad;

        if (fetchstr(uarg, argv[i], PGSIZE) < 0)
            goto bad;
    }

    int ret = exec(path, argv);

    // 释放参数内存
    for (int i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);

    // 返回后会将argc保存到a0寄存器
    // 即main(argc, argv)的第一个参数
    return ret;

bad:
    for (int i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);
    return -1;
}

// int pipe(int p[])
uint64 sys_pipe(void)
{
    uint64 fdarray; // user pointer to array of two integers
    int fd0, fd1;
    struct proc* p = myproc();

    argaddr(0, &fdarray);

    struct file *rf, *wf;
    if (pipealloc(&rf, &wf) < 0)
        return -1;

    fd0 = -1;
    if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
        if (fd0 >= 0)
            p->ofile[fd0] = 0;
        fileclose(rf);
        fileclose(wf);
        return -1;
    }
    if (copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0
        || copyout(p->pagetable, fdarray + sizeof(fd0), (char*)&fd1, sizeof(fd1)) < 0) {
        p->ofile[fd0] = 0;
        p->ofile[fd1] = 0;
        fileclose(rf);
        fileclose(wf);
        return -1;
    }
    return 0;
}
