#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock; // 定时中断计数锁
uint ticks;                // 定时器中断计数

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void trapinit(void) { initlock(&tickslock, "time"); }

// 设置内核异常处理stvec指向kernelvec
void trapinithart(void) { w_stvec((uint64)kernelvec); }

// 处理来自用户空间的中断、异常或系统调用
// trampoline.S->uservec 跳转到此处 (S-mode)
void usertrap(void)
{
    int which_dev = 0;

    // 确保中断来自用户态
    if ((r_sstatus() & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // 设置内核异常处理stvec指向kernelvec
    w_stvec((uint64)kernelvec);

    // 获取用户进程信息
    struct proc* p = myproc();

    // 保存用户PC
    p->trapframe->epc = r_sepc();

    // 如果是用户系统调用
    if (r_scause() == 8) {
        // 如果进程有终止标志, 则exit(-1)
        if (killed(p))
            exit(-1);

        // 将用户PC更新为ecall的下一条指令
        p->trapframe->epc += 4;

        // sepc, scause, sstatus已经处理完毕
        intr_on(); // 重新开启中断

        syscall(); // 处理系统调用
    }

    // 如果是设备中断 (键盘, 硬盘, 定时器)
    else if ((which_dev = devintr()) != 0) {

    }

    else {
        printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
        printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
        setkilled(p);
    }

    if (killed(p))
        exit(-1);

    // 如果是定时器中断, 就让出CPU
    if (which_dev == 2)
        yield();

    // 从内核态返回到用户态
    usertrapret();
}

// 从内核态返回到用户态
void usertrapret(void)
{
    struct proc* p = myproc();

    // 因为要将trap的目的地从kerneltrap()切换到usertrap()
    // 所以要在返回到用户空间之前关闭中断
    intr_off();

    // 设置用户异常处理stvec指向uservec (trampoline.S)
    uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
    w_stvec(trampoline_uservec);

    // 设置uservec()需要使用的值
    p->trapframe->kernel_satp = r_satp();         // 内核页表 (satp)
    p->trapframe->kernel_sp = p->kstack + PGSIZE; // 进程内核栈 (sp)
    p->trapframe->kernel_trap = (uint64)usertrap; // 用户中断处理函数 (jr)
    p->trapframe->kernel_hartid = r_tp();         // 当前CPU的ID (tp)

    // 设置sret将进入用户模式, 并启用中断
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP; // sstatus.SPP=0
    x |= SSTATUS_SPIE; // sstatus.SPIE=1
    w_sstatus(x);

    // 设置sret将跳转到的用户PC
    w_sepc(p->trapframe->epc);

    // 设置用户页表寄存器为 {Sv39, p->pagetable}
    uint64 satp = MAKE_SATP(p->pagetable);

    // 执行 trampoline.S->userret(satp)
    uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
    ((void (*)(uint64))trampoline_userret)(satp);
}

// kernelvec.S 跳转到此处
void kerneltrap()
{
    int which_dev = 0;
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    if ((sstatus & SSTATUS_SPP) == 0)
        panic("kerneltrap: not from supervisor mode");
    if (intr_get() != 0)
        panic("kerneltrap: interrupts enabled");

    if ((which_dev = devintr()) == 0) {
        // interrupt or trap from an unknown source
        printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
        panic("kerneltrap");
    }

    // give up the CPU if this is a timer interrupt.
    if (which_dev == 2 && myproc() != 0)
        yield();

    // the yield() may have caused some traps to occur,
    // so restore trap registers for use by kernelvec.S's sepc instruction.
    w_sepc(sepc);
    w_sstatus(sstatus);
}

// 定时器中断处理
// trap.c->devintr 跳转到这里
void clockintr()
{
    // 唤醒睡眠进程 (sysproc.c->sys_sleep)
    if (cpuid() == 0) {
        acquire(&tickslock);
        ticks++;
        wakeup(&ticks);
        release(&tickslock);
    }

    // 设置下一次定时器中断(大约是1秒)
    w_stimecmp(r_time() + 1000000);
}

// 判断并处理 当前的设备中断
// 定时器中断:2  其他设备中断:1  未知中断:0
// usertrap, kerneltrap 跳转到这里
int devintr()
{
    uint64 scause = r_scause();

    // 如果是外部设备中断 (PLIC)
    if (scause == 0x8000000000000009L) {
        // 从PLIC获取当前中断类型
        int irq = plic_claim();

        // 处理终端输入中断 (UART)
        if (irq == UART0_IRQ)
            uartintr();

        // 处理硬盘中断 (VirtIO)
        else if (irq == VIRTIO0_IRQ)
            virtio_disk_intr();

        // 未知外部中断
        else if (irq)
            printf("devintr: 未知外部中断 irq=%d\n", irq);

        // 告诉PLIC当前中断已完成
        // (PLIC 只允许同时最多产生一个中断)
        if (irq)
            plic_complete(irq);

        return 1;
    }

    // 如果是定时器中断
    else if (scause == 0x8000000000000005L) {
        clockintr();
        return 2;
    }

    else
        return 0;
}
