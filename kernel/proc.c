#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc* initproc;

int nextpid = 1; // 分配pid
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc* p);

extern char trampoline[]; // trampoline.S

// 帮助确保 对等待中的父进程的唤醒不会丢失
// 在使用p->parent时遵守内存模型
// 必须在任何p->lock之前获取
struct spinlock wait_lock;

// 为每个进程分配内核栈, 并映射到内核虚拟内存高地址
void proc_mapstacks(pagetable_t kpgtbl)
{
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++) {
        char* pa = kalloc();
        if (pa == 0)
            panic("kalloc");
        uint64 va = KSTACK((int)(p - proc));
        // 映射内核栈到内核虚拟内存高地址 va=va, pa=pa, size=PGSIZE, perm=可读可写
        kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    }
}

// 初始化进程表
void procinit(void)
{
    struct proc* p;

    initlock(&pid_lock, "nextpid");
    initlock(&wait_lock, "wait_lock");
    for (p = proc; p < &proc[NPROC]; p++) {
        initlock(&p->lock, "proc");          // 初始化进程锁
        p->state = UNUSED;                   // 未使用状态
        p->kstack = KSTACK((int)(p - proc)); // 内核栈的起始地址
    }
}

// 调用时必须关闭中断
// 以防止与其他CPU上的同进程发生竞争
inline int cpuid()
{
    int id = r_tp();
    return id;
}

// 返回当前CPU的cpu结构体
// 调用时必顼关闭中断
inline struct cpu* mycpu(void)
{
    int id = cpuid();
    struct cpu* c = (struct cpu*)&cpus[id];
    return c;
}

// 返回当前CPU执行的进程, 如果没有则返回0
struct proc* myproc(void)
{
    push_off(); //* 禁用中断
    struct cpu* c = mycpu();
    struct proc* p = c->proc;
    pop_off(); //* 恢复之前的中断状态
    return p;
}

// 分配新的pid
int allocpid()
{
    int pid;

    acquire(&pid_lock);
    pid = nextpid;
    nextpid = nextpid + 1;
    release(&pid_lock);

    return pid;
}

// 在进程表中查找UNUSED进程
// 如果找到, 初始化内核运行所需的状态并返回 (持有p->lock的锁)
// 如果没有空闲进程, 或内存分配失败, 返回0
static struct proc* allocproc(void)
{
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock); //* 获取进程锁
        if (p->state == UNUSED)
            goto found; // 找到空闲进程
        else
            release(&p->lock);
    }
    return 0;

found:
    p->pid = allocpid(); // 分配新的pid
    p->state = USED;     // 更新状态为USED

    // 分配 trapframe 页
    if ((p->trapframe = (struct trapframe*)kalloc()) == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // 配置用户态页表 (trapframe数据页 trampoline代码段)
    p->pagetable = proc_pagetable(p);
    if (p->pagetable == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // 清空进程上下文
    memset(&p->context, 0, sizeof(p->context));

    p->context.ra = (uint64)forkret;    // 设置swtch返回后跳转到forkret
    p->context.sp = p->kstack + PGSIZE; // 设置内核栈指针

    return p;
}

// 释放进程结构体和相关数据, 包括用户页 (需持有p->lock)
static void freeproc(struct proc* p)
{
    // 释放 trapframe 页
    if (p->trapframe)
        kfree((void*)p->trapframe);
    p->trapframe = 0;

    // 释放用户页表
    if (p->pagetable)
        proc_freepagetable(p->pagetable, p->sz);

    // 清空进程结构体
    p->pagetable = 0;
    p->sz = 0;
    p->pid = 0;
    p->parent = 0;
    p->name[0] = 0;
    p->chan = 0;
    p->killed = 0;
    p->xstate = 0;
    p->state = UNUSED;
}

// 用户虚拟内存布局 (proc.c->proc_pagetable)
// >低地址
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe)
//   TRAMPOLINE (内核代码段trampoline.S)
// >高地址

// 对于给定的进程创建一个用户页表
// 暂时只有trampoline和trapframe页
pagetable_t proc_pagetable(struct proc* p)
{
    pagetable_t pagetable;

    // 分配并清空一个页
    pagetable = uvmcreate();
    if (pagetable == 0)
        return 0;

    // 映射 trampoline 代码段到最高用户虚拟地址 (用于stvec)
    // va=TRAMPOLINE, pa=trampoline, size=PGSIZE, perm=内核可读可执行
    if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) < 0) {
        uvmfree(pagetable, 0);
        return 0;
    }

    // 映射 p->trapframe 数据页到 TRAMPLINE 的相邻低地址
    // va=TRAPFRAME, pa=p->trapframe, size=PGSIZE, perm=内核可读可写
    if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe), PTE_R | PTE_W) < 0) {
        uvmunmap(pagetable, TRAMPOLINE, 1, 0);
        uvmfree(pagetable, 0);
        return 0;
    }

    return pagetable;
}

// 释放进程的页表, 以及释放其引用的物理内存
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
    uvmunmap(pagetable, TRAMPOLINE, 1, 0); // 移除trampoline页映射
    uvmunmap(pagetable, TRAPFRAME, 1, 0);  // 移除trapframe页映射
    uvmfree(pagetable, sz);                // 释放用户页表及其物理内存
}

// od -t xC user/initcode
// 一段用户态程序, 调用exec("/init") <user/initcode.S>
uchar initcode[] = { 0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02, 0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00, 0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00, 0xef, 0xf0, 0x9f,
    0xff, 0x2f, 0x69, 0x6e, 0x69, 0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// 初始化第一个用户进程
void userinit(void)
{
    struct proc* p;

    // 从进程表中分配空闲进程
    // 1. 获取进程锁
    // 2. 分配新的pid
    // 3. 更新状态为USED
    // 4. 分配 trapframe 页
    // 5. 配置用户态页表 (trapframe数据页 trampoline代码段)
    // 6. 设置swtch返回后跳转到forkret
    // 7. 设置内核栈指针 p->kstack + PGSIZE
    p = allocproc();
    initproc = p;

    // 分配用户页填充initcode, 并映射到用户虚拟地址0
    uvmfirst(p->pagetable, initcode, sizeof(initcode));
    p->sz = PGSIZE;

    // 从内核态到用户态的准备工作
    p->trapframe->epc = 0;     // 设置trapframe->epc指向initcode
    p->trapframe->sp = PGSIZE; // 设置trapframe->sp 指向用户栈顶

    // 设置进程名和当前工作目录
    safestrcpy(p->name, "initcode", sizeof(p->name));
    p->cwd = namei("/");

    // 更新状态为RUNNABLE, 等待调度
    p->state = RUNNABLE;

    // 释放allocproc()中获取的进程锁
    release(&p->lock);
}

// sbrk() 的系统调用实现
// 将用户内存增加或减少n字节
int growproc(int n)
{
    uint64 sz;
    struct proc* p = myproc();

    sz = p->sz;

    // 如果是增加
    if (n > 0) {
        // oldsz=sz, newsz=sz+n, xperm=PTE_W
        if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
            return -1;
        }
    }

    // 如果是减少
    else if (n < 0) {
        sz = uvmdealloc(p->pagetable, sz, sz + n);
    }

    // 更新进程内存大小
    p->sz = sz;
    return 0;
}

// 从父进程拷贝创建一个新的子进程
// 设置子进程内核栈数据, 以便返回fork()系统调用
int fork(void)
{
    struct proc* p = myproc();

    //* 分配新的空闲进程 (获取np->lock)
    struct proc* np;
    if ((np = allocproc()) == 0)
        return -1;

    // 将父进程的用户内存 复制到子进程
    if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
        freeproc(np);
        release(&np->lock);
        return -1;
    }

    np->sz = p->sz;

    // 拷贝trapframe数据页
    *(np->trapframe) = *(p->trapframe);

    // 设置子进程fork的返回值为0
    np->trapframe->a0 = 0;

    // 增加对已打开文件描述符的引用计数
    for (int i = 0; i < NOFILE; i++)
        if (p->ofile[i])
            np->ofile[i] = filedup(p->ofile[i]);
    np->cwd = idup(p->cwd);

    // 拷贝进程名
    safestrcpy(np->name, p->name, sizeof(p->name));

    int pid = np->pid;

    release(&np->lock); //* 释放子进程锁

    // 设置子进程的父进程
    acquire(&wait_lock);
    np->parent = p;
    release(&wait_lock);

    // 更新子进程状态为RUNNABLE, 等待调度
    acquire(&np->lock);
    np->state = RUNNABLE;
    release(&np->lock);

    // 父进程返回子进程pid
    return pid;
}

// 将p的弃子交给init (调用者必须持有wait_lock)
void reparent(struct proc* p)
{
    struct proc* pp;

    for (pp = proc; pp < &proc[NPROC]; pp++) {
        if (pp->parent == p) {
            pp->parent = initproc;
            wakeup(initproc);
        }
    }
}

// 退出当前进程, 不会返回
// 退出进程会保持ZOMBIE状态, 直到其父进程调用wait回收
void exit(int status)
{
    struct proc* p = myproc();

    // 确保不是init进程
    if (p == initproc)
        panic("init exiting");

    // 关闭所有文件描述符
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->ofile[fd]) {
            struct file* f = p->ofile[fd];
            fileclose(f);
            p->ofile[fd] = NULL;
        }
    }

    // 释放工作目录
    begin_op(); //* 事务开始
    iput(p->cwd);
    end_op(); //* 事务结束
    p->cwd = 0;

    // 获取等待锁
    acquire(&wait_lock);

    // 将p的弃子交给init
    reparent(p);

    // 唤醒父进程
    wakeup(p->parent);

    acquire(&p->lock);
    p->xstate = status; // 记录退出状态位
    p->state = ZOMBIE;  // 更新状态为ZOMBIE
    release(&wait_lock);

    sched(); // 接受调度

    panic("zombie exit");
}

// 等待子进程退出
// 保存退出状态并返回其 pid
// 如果没有子进程，则返回 -1
// addr: 存储子进程退出状态
int wait(uint64 addr)
{
    struct proc* pp;
    int havekids, pid;
    struct proc* p = myproc();

    acquire(&wait_lock); // 获取等待锁

    for (;;) {
        havekids = 0;

        // 遍历所有进程, 寻找p的子进程
        for (pp = proc; pp < &proc[NPROC]; pp++) {
            if (pp->parent == p) {
                acquire(&pp->lock); // 获取子进程锁
                havekids = 1;       // 标记存在子进程

                // 如果子进程已经退出, 则释放子进程资源
                if (pp->state == ZOMBIE) {
                    pid = pp->pid;

                    // 从内核地址xstate 复制数据到 用户地址addr
                    if (addr != 0 && copyout(p->pagetable, addr, (char*)&pp->xstate, sizeof(pp->xstate)) < 0) {
                        release(&pp->lock);
                        release(&wait_lock);
                        return -1;
                    }

                    // 释放子进程所有资源
                    freeproc(pp);

                    release(&pp->lock);
                    release(&wait_lock);
                    return pid;
                }
                release(&pp->lock);
            }
        }

        // 如果没有子进程, 则直接返回-1
        if (!havekids || killed(p)) {
            release(&wait_lock);
            return -1;
        }

        // 等待子进程退出
        sleep(p, &wait_lock);
    }
}

// 进程的总调度循环体
// 每个CPU从 main.c 跳转到此处 (S-mode)
void scheduler(void)
{
    struct proc* p;
    struct cpu* c = mycpu();

    c->proc = 0;
    for (;;) {
        intr_on(); // 启用设备中断 (防止死锁)

        // 遍历寻找可运行进程
        int found = 0;
        for (p = proc; p < &proc[NPROC]; p++) {
            acquire(&p->lock); //* 获取进程锁

            // 如果进程是RUNNABLE状态
            if (p->state == RUNNABLE) {
                p->state = RUNNING;
                c->proc = p;

                // 由进程负责释放锁 并在返回到调度器之前重新获取锁
                // 将当前调度器状态保存到cpu, 并切换到进程p
                swtch(&c->context, &p->context);

                // 进程执行完毕, 返回到调度器 (持有p->lock)
                c->proc = 0;
                found = 1;
            }
            release(&p->lock); //* 释放进程锁
        }

        // 如果没有找到可运行的进程
        if (found == 0) {
            intr_on();           // 启用设备中断
            asm volatile("wfi"); // Wait For Interrupt
        }
    }
}

// 保存当前进程上下文, 切换到调度器上下文
// 之前必须持有p->lock, 并确保进程不处于RUNNING状态
void sched(void)
{
    int intr_enable;
    struct proc* p = myproc();

    // 确保持有p->lock
    if (holding(&p->lock) == 0)
        panic("sched p->lock");
    // 确保不处于中断关闭状态
    if (mycpu()->off_num != 1)
        panic("sched locks");
    // 确保不处于RUNNING状态
    if (p->state == RUNNING)
        panic("sched running");
    // 确保是中断关闭状态
    if (intr_get())
        panic("sched interruptible");

    intr_enable = mycpu()->intr_enable;    // 暂存当前中断状态
    swtch(&p->context, &mycpu()->context); // 保存当前进程上下文, 并切换到调度器上下文
    mycpu()->intr_enable = intr_enable;    // 恢复当前中断状态
}

// 让出CPU, 并将当前进程状态设置为RUNNABLE
void yield(void)
{
    struct proc* p = myproc();
    acquire(&p->lock); // *
    p->state = RUNNABLE;
    sched();
    release(&p->lock); // *
}

// 新分配进程调度 swtch 后跳转到此处 (S-mode)
void forkret(void)
{
    // 函数内部静态变量
    static int first = 1; // 标记是否为第一个用户进程

    // 释放在scheduler()中获取的进程锁
    release(&myproc()->lock); // *

    // 如果是第一个用户进程, 则初始化文件系统
    if (first) {
        // 文件系统初始化必须在常规进程的上下文中运行(例如调用sleep)
        // 因此不能直接在main()中执行
        fsinit(ROOTDEV);

        first = 0;

        // 内存屏障(确保其他CPU看到)
        __sync_synchronize();
    }

    // 进行从内核态返回到用户态
    usertrapret();
}

// 释放锁lk 并在chan上休眠, 醒来时重新获取锁
void sleep(void* chan, struct spinlock* lk)
{
    struct proc* p = myproc();

    // 必须持有p->lock才能修改p->state
    acquire(&p->lock);
    release(lk);

    // 更新为SLEEPING状态
    p->chan = chan;
    p->state = SLEEPING;

    // 进行调度 (持有p->lock)
    sched();

    // 唤醒后重新获取lk
    p->chan = 0;
    release(&p->lock);
    acquire(lk);
}

// 唤醒所有在chan上休眠的进程
// 必须在没有任何p->lock的情况下调用
void wakeup(void* chan)
{
    struct proc* p;

    // 遍历所有进程, 启用所有在chan上休眠的进程
    for (p = proc; p < &proc[NPROC]; p++) {
        if (p != myproc()) {
            acquire(&p->lock);
            if (p->state == SLEEPING && p->chan == chan)
                p->state = RUNNABLE;
            release(&p->lock);
        }
    }
}

// 终止给定pid的进程
int kill(int pid)
{
    struct proc* p;

    // 遍历所有进程, 查找pid
    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->pid == pid) {
            // 设置killed标志
            p->killed = 1;

            // 唤醒如果在睡眠的进程
            if (p->state == SLEEPING)
                p->state = RUNNABLE;

            release(&p->lock);
            return 0;
        }
        release(&p->lock);
    }
    return -1;
}

// 设置进程p的killed标志为1
void setkilled(struct proc* p)
{
    acquire(&p->lock);
    p->killed = 1;
    release(&p->lock);
}

// 返回进程p的killed标志
int killed(struct proc* p)
{
    acquire(&p->lock);
    int k = p->killed;
    release(&p->lock);
    return k;
}

// user_dst=1 : dst是用户虚拟地址
// user_dst=0 : dst是内核地址
int either_copyout(int user_dst, uint64 dst, void* src, uint64 len)
{
    struct proc* p = myproc();

    // 如果是用户地址, 则调用copyout
    if (user_dst)
        return copyout(p->pagetable, dst, src, len);

    // 如果是内核地址, 则直接拷贝
    else {
        memmove((char*)dst, src, len);
        return 0;
    }
}

// 从用户或内核地址src复制数据到dst
// user_src  1:从用户空间  0:从内核空间
int either_copyin(void* dst, int user_src, uint64 src, uint64 len)
{
    struct proc* p = myproc();

    // 如果是用户地址, 则调用copyin
    if (user_src)
        // 从页表对应虚拟地址 src 复制 len 字节到 dst
        // pagetable=p->pagetable, dst=dst, srcva=src, len=len
        return copyin(p->pagetable, dst, src, len);

    // 如果是内核地址, 则直接拷贝
    else {
        memmove(dst, (char*)src, len);
        return 0;
    }
}

// 打印进程列表 (调试用)
// 当用户在控制台上按下Ctrl+P时运行
// 为了避免进一步卡住已经卡住的机器, 不使用锁
void procdump(void)
{
    static char* states[] = {

        [UNUSED] "unused",
        [USED] "used",
        [SLEEPING] "sleep ",
        [RUNNABLE] "runble",
        [RUNNING] "run   ",
        [ZOMBIE] "zombie"
    };

    printf("\n");
    for (struct proc* p = proc; p < &proc[NPROC]; p++) {
        if (p->state == UNUSED)
            continue;

        char* state;
        if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
            state = states[p->state];
        else
            state = "???";

        printf("%d %s %s", p->pid, state, p->name);
        printf("\n");
    }
}
