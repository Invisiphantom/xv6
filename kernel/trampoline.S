

#include "riscv.h"
#include "memlayout.h"

.section trampsec
.globl trampoline
.globl usertrap
trampoline:

.align 4
.globl uservec
uservec:    
        # trap.c->usertrapret 将用户stvec设置到此处
        # stvec 跳转到此处 (U-mode -> S-mode)
        # 此时已处于内核态, 但还在操作用户态页表

        # 暂存a0到sscratch
        csrw sscratch, a0

        # p->trapframe的用户虚拟地址
        li a0, TRAPFRAME
        
        # 保存用户寄存器到 trapframe (proc.h)
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

	# 保存 a0 到 trapframe->a0
        csrr t0, sscratch
        sd t0, 112(a0)

        # 获取内核栈指针
        ld sp, 8(a0) # trapframe->kernel_sp

        # 获取内核CPU的ID
        ld tp, 32(a0) # trapframe->kernel_hartid

        # 获取用户中断处理函数地址
        ld t0, 16(a0) # trapframe->kernel_trap

        # 获取内核页表地址
        ld t1, 0(a0) # trapframe->kernel_satp

        # 切换到内核页表
        sfence.vma zero, zero
        csrw satp, t1
        sfence.vma zero, zero

        # 跳转到 trap.c->usertrap
        jr t0

.globl userret
userret:
        # trap.c->usertrapret(satp) 跳转到此处 (S-mode)
        # 参数 a0 = {Sv39, p->pagetable}
        # -exec x/20i $pc

        # 切换到用户页表
        sfence.vma zero, zero
        csrw satp, a0
        sfence.vma zero, zero

        # p->trapframe的用户虚拟地址
        li a0, TRAPFRAME

        # 从 trapframe 恢复用户寄存器 (proc.h)
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

	# 恢复寄存器 a0
        ld a0, 112(a0)
        
        # 跳转到用户进程PC
        sret # (S-mode -> U-mode)
