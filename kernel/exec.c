#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t*, uint64, struct minode*, uint, uint);

int flags2perm(int flags)
{
    int perm = 0;
    if (flags & 0x1)
        perm = PTE_X;
    if (flags & 0x2)
        perm |= PTE_W;
    return perm;
}

// int exec(char *file, char *argv[])
int exec(char* path, char** argv)
{
    uint64 sz = 0, sp, stackbase;
    pagetable_t pagetable = NULL;

    begin_op(); //* 事务开始

    // 获取路径对应inode
    struct minode* mip;
    if ((mip = namei(path)) == 0) {
        end_op(); //* 事务结束
        return -1;
    }

    ilock(mip); //** 获取inode锁

    // 读取并检查ELF文件头
    struct elfhdr elf;
    if (readi(mip, false, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
        goto bad;
    if (elf.magic != ELF_MAGIC)
        goto bad;

    // 创建一个新的用户页表
    struct proc* p = myproc();
    if ((pagetable = proc_pagetable(p)) == 0)
        goto bad;

    // 加载用户程序到内存
    struct proghdr ph;
    for (int i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        // 读取第i个程序头
        if (readi(mip, false, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
            goto bad;
        // 跳过不可加载的段
        if (ph.type != ELF_PROG_LOAD)
            continue;
        // 确保段在内存大小>=文件大小
        if (ph.memsz < ph.filesz)
            goto bad;
        // 确保段的内存大小非负
        if (ph.vaddr + ph.memsz < ph.vaddr)
            goto bad;
        // 确保段的虚拟地址页对齐
        if (ph.vaddr % PGSIZE != 0)
            goto bad;

        uint64 sz1; // 扩展用户内存
        if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
            goto bad;
        sz = sz1;

        // 加载段到内存
        if (loadseg(pagetable, ph.vaddr, mip, ph.off, ph.filesz) < 0)
            goto bad;
    }

    iunlockput(mip); //** 释放inode锁 (减引用)
    mip = NULL;

    end_op(); //* 事务结束

    p = myproc();
    uint64 oldsz = p->sz;

    // 在下一页边界分配一些页
    // 将第一个页设置为不可访问作为栈保护
    // 使用其余的作为用户栈
    sz = PGROUNDUP(sz);

    uint64 sz1; // 扩展2页用户内存 (保护页+用户栈)
    if ((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK + 1) * PGSIZE, PTE_W)) == 0)
        goto bad;
    sz = sz1;

    // 将保护页设置为不可访问
    uvmclear(pagetable, sz - (USERSTACK + 1) * PGSIZE);

    sp = sz;                             // 栈指针
    stackbase = sp - USERSTACK * PGSIZE; // 栈页的基地址

    // 加载参数字符串, 准备ustack
    uint64 argc;
    uint64 ustack[MAXARG];
    for (argc = 0; argv[argc]; argc++) {
        if (argc >= MAXARG)
            goto bad;
        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16; // 16字节对齐
        // 确保sp不会超出栈页
        if (sp < stackbase)
            goto bad;
        // 将参数字符串复制到用户栈
        if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
            goto bad;
        ustack[argc] = sp;
    }
    ustack[argc] = 0;

    // 加载argv[]指针数组
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16; // 16字节对齐
    // 确保sp不会超出栈页
    if (sp < stackbase)
        goto bad;
    // 将argv[]指针数组复制到用户栈
    if (copyout(pagetable, sp, (char*)ustack, (argc + 1) * sizeof(uint64)) < 0)
        goto bad;

    // 将argv保存到a1寄存器
    // 即main(argc, argv)的第二个参数
    p->trapframe->a1 = sp;

    // 记录程序名称用于调试
    char *s, *last;
    for (last = s = path; *s; s++)
        if (*s == '/')
            last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));

    // 切换到新的页表
    pagetable_t oldpagetable = p->pagetable;
    p->pagetable = pagetable;

    p->sz = sz;                              // 更新用户内存大小
    p->trapframe->epc = elf.entry;           // 设置程序入口地址
    p->trapframe->sp = sp;                   // 设置用户栈指针
    proc_freepagetable(oldpagetable, oldsz); // 清空旧页表

    // 返回后会将argc保存到a0寄存器
    // 即main(argc, argv)的第一个参数
    return argc;

bad:
    if (pagetable)
        proc_freepagetable(pagetable, sz);
    if (mip) {
        iunlockput(mip);
        end_op(); //* 事务结束
    }
    return -1;
}

// 加载程序段到页表的虚拟地址
// (需要va页对齐 并且已映射到页表)
static int loadseg(pagetable_t pagetable, uint64 va, struct minode* mip, uint offset, uint sz)
{
    for (int i = 0; i < sz; i += PGSIZE) {
        // 获取对应的物理地址
        uint64 pa = walkaddr(pagetable, va + i);
        if (pa == 0)
            panic("loadseg: 页表地址未映射");

        // 该页剩余大小
        uint n;
        if (sz - i < PGSIZE)
            n = sz - i;
        else
            n = PGSIZE;

        // 读取数据到对应物理地址
        if (readi(mip, false, (uint64)pa, offset + i, n) != n)
            return -1;
    }
    return 0;
}
