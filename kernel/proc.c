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

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc* p);

extern char trampoline[];  // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// 为每个进程分配内核栈, 并映射到内核虚拟内存高地址
void proc_mapstacks(pagetable_t kpgtbl) {
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
void procinit(void) {
    struct proc* p;

    initlock(&pid_lock, "nextpid");
    initlock(&wait_lock, "wait_lock");
    for (p = proc; p < &proc[NPROC]; p++) {
        initlock(&p->lock, "proc");           // 初始化进程锁
        p->state = UNUSED;                    // 未使用状态
        p->kstack = KSTACK((int)(p - proc));  // 内核栈的KVM地址
    }
}

// 被调用时必须关闭中断
// 以防止与其他CPU上的同进程发生竞争
int cpuid() {
    int id = r_tp();
    return id;
}

// 返回当前CPU的cpu结构体
// 被调用时必顼关闭中断
struct cpu* mycpu(void) {
    int id = cpuid();
    struct cpu* c = (struct cpu*)&cpus[id];
    return c;
}

// 返回当前CPU执行的进程, 如果没有则返回0
struct proc* myproc(void) {
    push_off();
    struct cpu* c = mycpu();
    struct proc* p = c->proc;
    pop_off();
    return p;
}

// 分配新的pid
int allocpid() {
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
static struct proc* allocproc(void) {
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock); // 获取进程锁
        if (p->state == UNUSED)
            goto found;  // 找到空闲进程
        else
            release(&p->lock);
    }
    return 0;

found:
    p->pid = allocpid();  // 分配新的pid
    p->state = USED;      // 更新状态为USED

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

    memset(&p->context, 0, sizeof(p->context));
    p->context.ra = (uint64)forkret;     // 设置上下文返回地址为forkret
    p->context.sp = p->kstack + PGSIZE;  // 设置内核栈指针 (高地址向低地址增长)

    return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void freeproc(struct proc* p) {
    if (p->trapframe)
        kfree((void*)p->trapframe);
    p->trapframe = 0;
    if (p->pagetable)
        proc_freepagetable(p->pagetable, p->sz);
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
pagetable_t proc_pagetable(struct proc* p) {
    pagetable_t pagetable;

    // 分配并清空一个页
    pagetable = uvmcreate();
    if (pagetable == 0)
        return 0;

    // 映射 trampoline 代码段到最高用户虚拟地址 (用于stvec)
    // va=TRAMPOLINE, pa=trampoline.S, size=PGSIZE, perm=内核可读可执行
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

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, sz);
}

// od -t xC user/initcode
// 一段用户态程序, 调用exec("/init") <user/initcode.S>
uchar initcode[] = {0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02, 0x97, 0x05, 0x00, 0x00, 0x93,
                    0x85, 0x35, 0x02, 0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00, 0x93, 0x08,
                    0x20, 0x00, 0x73, 0x00, 0x00, 0x00, 0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e,
                    0x69, 0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// 初始化第一个用户进程
void userinit(void) {
    struct proc* p;

    // 从进程表中分配空闲进程
    // 1. 获取进程锁
    // 2. 分配新的pid
    // 3. 更新状态为USED
    // 4. 分配 trapframe 页
    // 5. 配置用户态页表 (trapframe数据页 trampoline代码段)
    // 6. 设置上下文返回地址为forkret
    // 7. 设置内核栈指针 p->kstack + PGSIZE
    p = allocproc();
    initproc = p;

    // 分配用户页 映射到用户虚拟地址0, 并填充initcode
    uvmfirst(p->pagetable, initcode, sizeof(initcode));
    p->sz = PGSIZE;

    // 从内核态到用户态的准备工作
    p->trapframe->epc = 0;      // 设置trapframe->epc指向initcode
    p->trapframe->sp = PGSIZE;  // 设置trapframe->sp 指向用户栈顶

    // 设置进程名和当前工作目录
    safestrcpy(p->name, "initcode", sizeof(p->name));
    p->cwd = namei("/");

    // 更新进程状态为RUNNABLE
    p->state = RUNNABLE;

    release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n) {
    uint64 sz;
    struct proc* p = myproc();

    sz = p->sz;
    if (n > 0) {
        if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
            return -1;
        }
    } else if (n < 0) {
        sz = uvmdealloc(p->pagetable, sz, sz + n);
    }
    p->sz = sz;
    return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void) {
    int i, pid;
    struct proc* np;
    struct proc* p = myproc();

    // Allocate process.
    if ((np = allocproc()) == 0) {
        return -1;
    }

    // Copy user memory from parent to child.
    if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
        freeproc(np);
        release(&np->lock);
        return -1;
    }
    np->sz = p->sz;

    // copy saved user registers.
    *(np->trapframe) = *(p->trapframe);

    // Cause fork to return 0 in the child.
    np->trapframe->a0 = 0;

    // increment reference counts on open file descriptors.
    for (i = 0; i < NOFILE; i++)
        if (p->ofile[i])
            np->ofile[i] = filedup(p->ofile[i]);
    np->cwd = idup(p->cwd);

    safestrcpy(np->name, p->name, sizeof(p->name));

    pid = np->pid;

    release(&np->lock);

    acquire(&wait_lock);
    np->parent = p;
    release(&wait_lock);

    acquire(&np->lock);
    np->state = RUNNABLE;
    release(&np->lock);

    return pid;
}

// 将p的弃子交给init (调用者必须持有wait_lock)
void reparent(struct proc* p) {
    struct proc* pp;

    for (pp = proc; pp < &proc[NPROC]; pp++) {
        if (pp->parent == p) {
            pp->parent = initproc;
            wakeup(initproc);
        }
    }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status) {
    struct proc* p = myproc();

    if (p == initproc)
        panic("init exiting");

    // Close all open files.
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->ofile[fd]) {
            struct file* f = p->ofile[fd];
            fileclose(f);
            p->ofile[fd] = 0;
        }
    }

    begin_op();
    iput(p->cwd);
    end_op();
    p->cwd = 0;

    acquire(&wait_lock);

    // 将p的弃子交给init
    reparent(p);

    // Parent might be sleeping in wait().
    wakeup(p->parent);

    acquire(&p->lock);

    p->xstate = status;
    p->state = ZOMBIE;

    release(&wait_lock);

    // 跳转到sched(), 不会返回
    sched();
    panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr) {
    struct proc* pp;
    int havekids, pid;
    struct proc* p = myproc();

    acquire(&wait_lock);

    for (;;) {
        // Scan through table looking for exited children.
        havekids = 0;
        for (pp = proc; pp < &proc[NPROC]; pp++) {
            if (pp->parent == p) {
                // make sure the child isn't still in exit() or swtch().
                acquire(&pp->lock);

                havekids = 1;
                if (pp->state == ZOMBIE) {
                    // Found one.
                    pid = pp->pid;
                    if (addr != 0 && copyout(p->pagetable, addr, (char*)&pp->xstate, sizeof(pp->xstate)) < 0) {
                        release(&pp->lock);
                        release(&wait_lock);
                        return -1;
                    }
                    freeproc(pp);
                    release(&pp->lock);
                    release(&wait_lock);
                    return pid;
                }
                release(&pp->lock);
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || killed(p)) {
            release(&wait_lock);
            return -1;
        }

        // Wait for a child to exit.
        sleep(p, &wait_lock);  // DOC: wait-sleep
    }
}

//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
// 每个CPU从 main.c 跳转到此处 (S-mode)

void scheduler(void) {
    struct proc* p;
    struct cpu* c = mycpu();

    c->proc = 0;
    for (;;) {
        intr_on();  // 启用设备中断 (防止死锁)

        // 遍历寻找可运行进程
        int found = 0;
        for (p = proc; p < &proc[NPROC]; p++) {
            // 获取进程锁
            acquire(&p->lock);
            // 如果进程是RUNNABLE状态
            if (p->state == RUNNABLE) {
                // 由进程负责释放锁 并在返回到调度器之前重新获取锁
                p->state = RUNNING;
                c->proc = p;

                // 将当前调度器状态保存到cpu, 并切换到进程p
                swtch(&c->context, &p->context);

                // Process is done running for now.
                // It should have changed its p->state before coming back.
                c->proc = 0;
                found = 1;
            }
            release(&p->lock);
        }

        // 如果没有找到可运行的进程
        if (found == 0) {
            intr_on();            // 启用设备中断
            asm volatile("wfi");  // Wait For Interrupt
        }
    }
}

// 保存当前进程上下文, 并切换到调度器上下文
// 之前必须持有p->lock, 并且更新了proc->state为RUNNABLE
// 保存和恢复intena是因为intena是这个内核线程的属性, 而不是这个CPU的属性
// 它应该是proc->intena和proc->noff, 但这会在一些地方出问题, 因为锁被持有但没有进程
void sched(void) {
    int intena;
    struct proc* p = myproc();

    // 必须持有p->lock
    if (!holding(&p->lock))
        panic("sched p->lock");
    // 必须持有cpu->lock
    if (mycpu()->noff != 1)
        panic("sched locks");
    // 必须是RUNNABLE状态
    if (p->state == RUNNING)
        panic("sched running");
    // 必须是中断关闭状态
    if (intr_get())
        panic("sched interruptible");

    intena = mycpu()->intena;               // 暂存中断状态
    swtch(&p->context, &mycpu()->context);  // 保存当前进程上下文, 并切换到调度器
    mycpu()->intena = intena;               // 恢复中断状态
}

// 让出CPU, 并将当前进程状态设置为RUNNABLE
void yield(void) {
    struct proc* p = myproc();
    acquire(&p->lock);
    p->state = RUNNABLE;
    sched();
    release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void) {
    static int first = 1;

    // Still holding p->lock from scheduler.
    release(&myproc()->lock);

    if (first) {
        // 文件系统初始化必须在常规进程的上下文中运行(例如, 因为它调用sleep)
        // 因此不能直接在main()中执行
        fsinit(ROOTDEV);

        first = 0;
        // ensure other cores see first=0.
        __sync_synchronize();  // 内存屏障
    }

    usertrapret();
}

// 自动释放锁并在chan上休眠
// 当醒来时重新获取锁
void sleep(void* chan, struct spinlock* lk) {
    struct proc* p = myproc();

    // Must acquire p->lock in order to
    // change p->state and then call sched.
    // Once we hold p->lock, we can be
    // guaranteed that we won't miss any wakeup
    // (wakeup locks p->lock),
    // so it's okay to release lk.

    acquire(&p->lock);  // DOC: sleeplock1
    release(lk);

    // Go to sleep.
    p->chan = chan;
    p->state = SLEEPING;

    sched();

    // Tidy up.
    p->chan = 0;

    // Reacquire original lock.
    release(&p->lock);
    acquire(lk);
}

// 唤醒所有在chan上休眠的进程
// 必须在没有任何p->lock的情况下调用
void wakeup(void* chan) {
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++) {
        if (p != myproc()) {
            acquire(&p->lock);
            if (p->state == SLEEPING && p->chan == chan) {
                p->state = RUNNABLE;
            }
            release(&p->lock);
        }
    }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid) {
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->pid == pid) {
            p->killed = 1;
            if (p->state == SLEEPING) {
                // Wake process from sleep().
                p->state = RUNNABLE;
            }
            release(&p->lock);
            return 0;
        }
        release(&p->lock);
    }
    return -1;
}

void setkilled(struct proc* p) {
    acquire(&p->lock);
    p->killed = 1;
    release(&p->lock);
}

int killed(struct proc* p) {
    int k;

    acquire(&p->lock);
    k = p->killed;
    release(&p->lock);
    return k;
}

// user_dst=1 : dst是用户虚拟地址
// user_dst=0 : dst是内核地址
int either_copyout(int user_dst, uint64 dst, void* src, uint64 len) {
    struct proc* p = myproc();
    // 如果是用户地址, 则调用copyout
    if (user_dst) {
        return copyout(p->pagetable, dst, src, len);
    }
    // 如果是内核地址, 则直接拷贝
    else {
        memmove((char*)dst, src, len);
        return 0;
    }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void* dst, int user_src, uint64 src, uint64 len) {
    struct proc* p = myproc();
    if (user_src) {
        return copyin(p->pagetable, dst, src, len);
    } else {
        memmove(dst, (char*)src, len);
        return 0;
    }
}

// 打印进程列表 (调试用)
// 当用户在控制台上按下Ctrl+P时运行
// 为了避免进一步卡住已经卡住的机器, 不使用锁
void procdump(void) {
    static char* states[] = {[UNUSED] "unused",   [USED] "used",      [SLEEPING] "sleep ",
                             [RUNNABLE] "runble", [RUNNING] "run   ", [ZOMBIE] "zombie"};
    struct proc* p;
    char* state;

    printf("\n");
    for (p = proc; p < &proc[NPROC]; p++) {
        if (p->state == UNUSED)
            continue;
        if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
            state = states[p->state];
        else
            state = "???";
        printf("%d %s %s", p->pid, state, p->name);
        printf("\n");
    }
}
