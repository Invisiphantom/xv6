#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "stat.h"
#include "fs.h"

// 内核页表
pagetable_t kernel_pagetable;

// 内核程序代码段结束地址 (kernel.ld)
extern char etext[];

extern char trampoline[]; // trampoline.S

// 创建内核页表 (大部分直接映射)
pagetable_t kvmmake(void)
{
    pagetable_t kpgtbl;

    // 分配并清空页表空间
    kpgtbl = (pagetable_t)kalloc();
    memset(kpgtbl, 0, PGSIZE);

    // UART寄存器 va=UART0, pa=UART0, size=PGSIZE, perm=可读可写
    kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

    // VirtIO设备 va=VIRTIO0, pa=VIRTIO0, size=PGSIZE, perm=可读可写
    kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

    // PLIC控制器 va=PLIC, pa=PLIC, size=0x4000000, perm=可读可写
    kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

    // 映射内核代码段 (可读, 可执行) va=KERNBASE, pa=KERNBASE, size=etext-KERNBASE
    kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

    // 映射内核其余物理内存 (可读, 可写) va=etext, pa=etext, size=PHYSTOP-etext
    kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

    // 映射trampoline页到内核最高虚拟地址 va=TRAMPOLINE, pa=trampoline, size=PGSIZE,
    // perm=可读可执行
    kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 为每个进程分配并映射一个内核栈
    proc_mapstacks(kpgtbl);

    return kpgtbl;
}

// 初始化内核页表
void kvminit(void) { kernel_pagetable = kvmmake(); }

// 设置satp寄存器, 启用Sv39分页
void kvminithart()
{
    sfence_vma();
    w_satp(MAKE_SATP(kernel_pagetable));
    sfence_vma();
}

// https://learningos.cn/uCore-Tutorial-Guide-2022S/_images/sv39-full.png
// Sv39有三级页表, 每个页表页包含512个64位PTE
//   63:39 -- 全零
//   38:30 -- 第2级页表索引 (9 bits)
//   29:21 -- 第1级页表索引 (9 bits)
//   20:12 -- 第0级页表索引 (9 bits)
//   11:0  -- 页内偏移 (12 bits)

// 查找页表, 返回虚拟地址va 对应的页表项
// alloc  1:分配新页表  0:不进行分配
pte_t* walk(pagetable_t pagetable, uint64 va, int alloc)
{
    if (va >= MAXVA)
        panic("walk");

    for (int level = 2; level > 0; level--) {
        // 获取pagetable页表中, va的第level级索引项
        pte_t* pte = &pagetable[PX(level, va)];

        // 如果是有效项, 则获取下一级页表
        if (*pte & PTE_V)
            pagetable = (pagetable_t)PTE2PA(*pte);

        // 如果不是有效项
        else {
            // 给下一级页表分配一页内存
            pagetable = (pde_t*)kalloc();

            // 确保alloc为真 且分配成功
            if (!alloc || (pagetable == 0))
                return 0;

            // 清空页表
            memset(pagetable, 0, PGSIZE);

            // 填充页表项, 指向新分配的下一级页表
            *pte = PA2PTE(pagetable) | PTE_V;
        }
    }

    // 返回最后一级页表项的地址
    return &pagetable[PX(0, va)];
}

// 查找用户页表, 返回虚拟地址va 对应的物理地址
uint64 walkaddr(pagetable_t pagetable, uint64 va)
{
    pte_t* pte;
    uint64 pa;

    if (va >= MAXVA)
        return 0;

    // 返回va对应的最后一级页表项
    pte = walk(pagetable, va, 0);

    // 确保页表项存在
    if (pte == 0)
        return 0;
    // 确保页表项有效
    if ((*pte & PTE_V) == 0)
        return 0;
    // 确保页表项允许用户访问
    if ((*pte & PTE_U) == 0)
        return 0;

    pa = PTE2PA(*pte);
    return pa;
}

// 添加一个映射到内核页表 (在booting时使用)
// 不刷新TLB或启用分页
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
    if (mappages(kpgtbl, va, sz, pa, perm) != 0)
        panic("kvmmap");
}

// pagetable: 首级页表所在的物理页地址
// va: 开始的虚拟地址  size: 映射的总大小 (字节)
// pa: 开始的物理地址  perm: 页表项的权限 (riscv.h)
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
    uint64 a, last;
    pte_t* pte;

    // 检查页对齐以及长度非零
    if ((va % PGSIZE) != 0)
        panic("mappages: va not aligned");
    if ((size % PGSIZE) != 0)
        panic("mappages: size not aligned");
    if (size == 0)
        panic("mappages: size");

    a = va;                    // 首个虚拟页地址
    last = va + size - PGSIZE; // 末个虚拟页地址
    for (;;) {
        // 获取va对应的最后一级页表项 (如果不存在, 则逐级创建)
        if ((pte = walk(pagetable, a, 1)) == 0)
            return -1;

        // 确保此页表项 未映射物理地址
        if (*pte & PTE_V)
            panic("mappages: remap");

        // 设置页表项 (物理地址 + 权限位 + 有效位)
        *pte = PA2PTE(pa) | perm | PTE_V;

        // 如果所有页都映射完毕, 则退出
        if (a == last)
            break;

        // 继续设置下一页
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

// 移除从va开始的npages映射 va必须页对齐 映射必须存在
// do_free 1:释放物理内存  0:不进行释放
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
    pte_t* pte;

    // 确保va页对齐
    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    // 遍历所有va
    for (uint64 a = va; a < va + npages * PGSIZE; a += PGSIZE) {
        // 确保页表项存在
        if ((pte = walk(pagetable, a, 0)) == 0)
            panic("uvmunmap: walk");

        // 确保页表项有效
        if ((*pte & PTE_V) == 0)
            panic("uvmunmap: not mapped");

        // 确保是叶子页项 (存在RWX位)
        if (PTE_FLAGS(*pte) == PTE_V)
            panic("uvmunmap: not a leaf");

        // 如果需要释放物理内存, 则释放
        if (do_free) {
            uint64 pa = PTE2PA(*pte);
            kfree((void*)pa);
        }

        // 清空页表项
        *pte = 0;
    }
}

// 分配并清空一个用户页表
pagetable_t uvmcreate()
{
    pagetable_t pagetable;
    pagetable = (pagetable_t)kalloc();
    if (pagetable == 0)
        return 0;
    memset(pagetable, 0, PGSIZE);
    return pagetable;
}

// 加载用户态initcode到页表的地址0, 用于第一个进程
void uvmfirst(pagetable_t pagetable, uchar* initcode, uint sz)
{
    char* mem;

    // 确保initcode不超过一页
    if (sz >= PGSIZE)
        panic("uvmfirst: more than a page");

    // 分配并清空一页内存
    mem = kalloc();
    memset(mem, 0, PGSIZE);

    // va=0, pa=mem, size=PGSIZE, perm=可读可写可执行 (用户权限)
    mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);

    // 拷贝initcode到mem物理地址
    memmove(mem, initcode, sz);
}

// 增加进程的用户内存 oldsz->newsz (oldsz和newsz不需要页对齐)
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
    char* mem;

    // 确保newsz大于oldsz
    if (newsz < oldsz)
        return oldsz;

    // 向上对齐PGSIZE, 得到需新分配的起始页
    oldsz = PGROUNDUP(oldsz);

    for (uint64 a = oldsz; a < newsz; a += PGSIZE) {
        // 分配一页新内存并清空
        mem = kalloc();
        if (mem == 0) {
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
        memset(mem, 0, PGSIZE);

        // 将新内存映射到页表
        // va=a, pa=mem, size=PGSIZE, perm=可读可写可执行
        if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) != 0) {
            kfree(mem);
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
    }
    return newsz;
}

// 减少进程的用户内存 oldsz->newsz (oldsz和newsz不需要页对齐)
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
    // 确保newsz小于oldsz
    if (newsz >= oldsz)
        return oldsz;

    // 如果出现页减少, 则移除页表映射, 并释放物理内存
    if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
        int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
    }

    return newsz;
}

// 递归地释放页表页, 所有叶子映射必须已经被移除
void freewalk(pagetable_t pagetable)
{
    // 遍历所有页表项
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];

        // 如果是目录项, 递归释放
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            uint64 child = PTE2PA(pte);
            freewalk((pagetable_t)child);
            pagetable[i] = 0;
        }

        // 如果是叶子项, 则报错
        else if (pte & PTE_V)
            panic("freewalk: leaf");
    }

    // 释放页表页
    kfree((void*)pagetable);
}

// 释放用户页表, 以及释放其引用的物理内存
void uvmfree(pagetable_t pagetable, uint64 sz)
{
    if (sz > 0) // 移除代码段的虚拟内存映射
        uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
    freewalk(pagetable); // 递归释放无叶子项的页表页
}

// 给定父进程的页表, 将其内存复制到子进程的页表中
// 既复制页表页, 又复制物理内存页
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
    pte_t* pte;
    uint64 pa, i;
    uint flags;
    char* mem;

    for (i = 0; i < sz; i += PGSIZE) {
        // 确保页表项存在
        if ((pte = walk(old, i, 0)) == 0)
            panic("uvmcopy: pte should exist");

        // 确保页表项有效
        if ((*pte & PTE_V) == 0)
            panic("uvmcopy: page not present");

        // 获取物理地址和权限位
        pa = PTE2PA(*pte);
        flags = PTE_FLAGS(*pte);

        // 分配一页新内存
        if ((mem = kalloc()) == 0)
            goto err;

        // 拷贝数据
        memmove(mem, (char*)pa, PGSIZE);

        // 将新内存映射到新页表
        // va=i, pa=mem, size=PGSIZE, perm=flags
        if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
            kfree(mem);
            goto err;
        }
    }
    return 0;

err:
    uvmunmap(new, 0, i / PGSIZE, 1);
    return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va)
{
    pte_t* pte;

    pte = walk(pagetable, va, 0);
    if (pte == 0)
        panic("uvmclear");
    *pte &= ~PTE_U;
}

// 从内核空间 复制数据到 用户空间
// 从 srcva 复制 len 字节到给定页表中的虚拟地址 dstva
int copyout(pagetable_t pagetable, uint64 dstva, char* src, uint64 len)
{
    uint64 n, va0, pa0;
    pte_t* pte;

    while (len > 0) {
        va0 = PGROUNDDOWN(dstva);
        if (va0 >= MAXVA)
            return -1;
        pte = walk(pagetable, va0, 0);
        if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 || (*pte & PTE_W) == 0)
            return -1;
        pa0 = PTE2PA(*pte);
        n = PGSIZE - (dstva - va0);
        if (n > len)
            n = len;
        memmove((void*)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
    return 0;
}

// 从用户空间 复制数据到 内核空间
// 从给定页表中的虚拟地址 srcva 复制 len 字节到 dst
int copyin(pagetable_t pagetable, char* dst, uint64 srcva, uint64 len)
{
    while (len > 0) {
        uint64 va0 = PGROUNDDOWN(srcva);       // srcva所在页的首地址
        uint64 pa0 = walkaddr(pagetable, va0); // 获取va0对应的物理地址

        if (pa0 == 0)
            return -1;

        // srcva到页末的字节数
        uint64 n = PGSIZE - (srcva - va0);
        if (n > len)
            n = len;

        // 从物理地址拷贝数据
        memmove(dst, (void*)(pa0 + (srcva - va0)), n);

        // 继续下一页
        len -= n;
        dst += n;
        srcva = va0 + PGSIZE;
    }
    return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char* dst, uint64 srcva, uint64 max)
{
    uint64 n, va0, pa0;
    int got_null = 0;

    while (got_null == 0 && max > 0) {
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0)
            return -1;
        n = PGSIZE - (srcva - va0);
        if (n > max)
            n = max;

        char* p = (char*)(pa0 + (srcva - va0));
        while (n > 0) {
            if (*p == '\0') {
                *dst = '\0';
                got_null = 1;
                break;
            } else {
                *dst = *p;
            }
            --n;
            --max;
            p++;
            dst++;
        }

        srcva = va0 + PGSIZE;
    }
    if (got_null) {
        return 0;
    } else {
        return -1;
    }
}
