#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void trapinit(void) {
    initlock(&tickslock, "time");
}

// 设置stvec异常处理跳转到kernelvec.S
void trapinithart(void) {
    w_stvec((uint64)kernelvec);
}

// 处理来自用户空间的中断、异常或系统调用
// 在trampoline.S中被uservec()调用
void usertrap(void) {
    int which_dev = 0;

    // 如果不是来自用户模式
    if ((r_sstatus() & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // 更新stvec寄存器的内核异常处理程序为kernelvec
    w_stvec((uint64)kernelvec);

    // 获取之前正在执行的进程
    struct proc* p = myproc();

    // 保存用户PC
    p->trapframe->epc = r_sepc();

    if (r_scause() == 8) {
        // 系统调用

        if (killed(p))
            exit(-1);

        // 将用户PC更新为ecall的下一条指令
        p->trapframe->epc += 4;

        // sepc, scause, sstatus已经处理完毕
        intr_on();  // 重新开启中断

        syscall();  // 处理系统调用
    } else if ((which_dev = devintr()) != 0) {
        // 处理外部设备中断 (键盘, 磁盘, 定时器)
    } else {
        printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
        printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
        setkilled(p);
    }

    if (killed(p))
        exit(-1);

    // 如果是定时器中断, 就让出CPU
    if (which_dev == 2)
        yield();

    usertrapret();
}

//
// return to user space
//
void usertrapret(void) {
    struct proc* p = myproc();

    // we're about to switch the destination of traps from
    // kerneltrap() to usertrap(), so turn off interrupts until
    // we're back in user space, where usertrap() is correct.
    intr_off();

    // send syscalls, interrupts, and exceptions to uservec in trampoline.S
    uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
    w_stvec(trampoline_uservec);

    // set up trapframe values that uservec will need when
    // the process next traps into the kernel.
    p->trapframe->kernel_satp = r_satp();          // kernel page table
    p->trapframe->kernel_sp = p->kstack + PGSIZE;  // process's kernel stack
    p->trapframe->kernel_trap = (uint64)usertrap;
    p->trapframe->kernel_hartid = r_tp();  // hartid for cpuid()

    // set up the registers that trampoline.S's sret will use
    // to get to user space.

    // set S Previous Privilege mode to User.
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP;  // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE;  // enable interrupts in user mode
    w_sstatus(x);

    // set S Exception Program Counter to the saved user pc.
    w_sepc(p->trapframe->epc);

    // tell trampoline.S the user page table to switch to.
    uint64 satp = MAKE_SATP(p->pagetable);

    // jump to userret in trampoline.S at the top of memory, which
    // switches to the user page table, restores user registers,
    // and switches to user mode with sret.
    uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
    ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void kerneltrap() {
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
void clockintr() {
    if (cpuid() == 0) {
        acquire(&tickslock);
        ticks++;
        wakeup(&ticks);
        release(&tickslock);
    }

    // 设置下一次定时器中断(大约是 0.1秒)
    w_stimecmp(r_time() + 1000000);
}

// 判断当前是外部中断还是软中断, 并处理
// 如果是定时器中断: 返回2
// 如果是其他设备中断: 返回1
// 如果不是识别的中断: 返回0
int devintr() {
    uint64 scause = r_scause();

    if (scause == 0x8000000000000009L) {
        // 当前是内核态外部中断 (PLIC)

        // irq 指示当前处理的中断
        int irq = plic_claim();

        if (irq == UART0_IRQ)
            uartintr();  // 处理键盘输入
        else if (irq == VIRTIO0_IRQ)
            virtio_disk_intr();  // 处理磁盘中断
        else if (irq)
            printf("unexpected interrupt irq=%d\n", irq);

        // PLIC只允许每个设备同时最多产生一个中断
        // 现在告诉PLIC 该设备现在可以再次中断
        if (irq)
            plic_complete(irq);

        return 1;
    } else if (scause == 0x8000000000000005L) {
        // 定时器中断
        clockintr();
        return 2;
    } else {
        return 0;
    }
}
