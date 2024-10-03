        #
        # low-level code to handle traps from user space into
        # the kernel, and returns from kernel to user.
        #
        # the kernel maps the page holding this code
        # at the same virtual address (TRAMPOLINE)
        # in user and kernel space so that it continues
        # to work when it switches page tables.
        # kernel.ld causes this code to start at 
        # a page boundary.
        #

#include "riscv.h"
#include "memlayout.h"

.section trampsec
.globl trampoline
.globl usertrap
trampoline:
.align 4
.globl uservec
uservec:    
        # trap.c 将 stvec 地址设置为此处
        # 所以用户态 traps 跳转到此处
        # 此时已经是内核态, 但是页表还属于用户态

        # 将a0保存到sscratch, 以便使用a0获取TRAPFRAME
        csrw sscratch, a0

        # 每个进程都有一个独立的p->trapframe内存区域
        # 但是在每个进程的用户页表中, 它映射到相同的虚拟地址(TRAPFRAME)
        li a0, TRAPFRAME
        
        # 保存用户寄存器到 TRAPFRAME
        sd ra, 40(a0)
        sd sp, 48(a0)
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)

	# 保存 a0 到 p->trapframe->a0
        csrr t0, sscratch
        sd t0, 112(a0)

        # 获取内核态栈指针 from p->trapframe->kernel_sp
        ld sp, 8(a0)

        # 获取内核态硬件线程ID from p->trapframe->kernel_hartid
        ld tp, 32(a0)

        # 获取函数地址usertrap() from p->trapframe->kernel_trap
        ld t0, 16(a0)

        # 获取内核态页表地址 from p->trapframe->kernel_satp.
        ld t1, 0(a0)

        # 等待上述内存操作完成, 此时还在使用用户页表
        sfence.vma zero, zero

        # 加载satp为内核态页表
        csrw satp, t1

        # 从 TLB 中刷新现已过时的用户态条目
        sfence.vma zero, zero

        # 跳转到 usertrap()
        jr t0

.globl userret
userret:
        # userret(pagetable)
        # called by usertrapret() in trap.c to
        # switch from kernel to user.
        # a0: user page table, for satp.

        # switch to the user page table.
        sfence.vma zero, zero
        csrw satp, a0
        sfence.vma zero, zero

        li a0, TRAPFRAME

        # restore all but a0 from TRAPFRAME
        ld ra, 40(a0)
        ld sp, 48(a0)
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

	# restore user a0
        ld a0, 112(a0)
        
        # return to user mode and user pc.
        # usertrapret() set up sstatus and sepc.
        sret
