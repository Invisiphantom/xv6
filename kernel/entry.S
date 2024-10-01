        
        # qemu -kernel 将内核加载到 0x80000000
        # 并使每个 hart（即 CPU）跳转到该地址。
        # kernel.ld 会将以下代码放置在 0x80000000。

.section .text
.global _entry
_entry:
        # 给C语言代码设置栈
        # stack0在start.c中声明 (.bss段)
        # 每个CPU的栈大小为4096字节
        # 栈是从高地址向低地址增长的
        # sp = stack0 + (hartid+1) * 4096
        la sp, stack0
        li a0, 1024*4
        csrr a1, mhartid
        addi a1, a1, 1
        mul a0, a0, a1
        add sp, sp, a0
        # 跳转到 start() in start.c
        call start
spin:
        j spin
