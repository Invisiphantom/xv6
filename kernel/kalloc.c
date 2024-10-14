
// 简易物理内存分配器
// 用于用户进程, 内核栈, 页表, 管道缓冲区
// 直接分配整个4096字节页

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void* pa_start, void* pa_end);

// kernel程序的结束地址 (kernel.ld)
extern char end[];

struct run {
    struct run* next;
};

struct {
    struct spinlock lock;
    struct run* freelist;
} kmem;

void kinit()
{
    initlock(&kmem.lock, "kmem");   // 初始化kmem锁
    freerange(end, (void*)PHYSTOP); // 释放所有堆页到空闲链表
}

void freerange(void* pa_start, void* pa_end)
{
    char* p = (char*)PGROUNDUP((uint64)pa_start); // 对齐PGSIZE
    for (; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
        kfree(p); // 释放到空闲链表
}

// 释放pa指向的物理内存页, 通常应该由kalloc()返回
void kfree(void* pa)
{
    struct run* r;

    // 如果未对齐PGSIZE, 或者小于end, 或者大于等于PHYSTOP, 则panic
    if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

    // 用垃圾填充以捕获悬空引用
    memset(pa, 1, PGSIZE);

    // 连接到空闲链表
    r = (struct run*)pa;
    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
}

// 直接分配4096字节的物理内存页
void* kalloc(void)
{
    struct run* r;

    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r)
        kmem.freelist = r->next;
    release(&kmem.lock);

    if (r) // 填充垃圾
        memset((char*)r, 5, PGSIZE);
    return (void*)r;
}
