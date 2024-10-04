
 
# qemu -kernel 将内核加载到 0x80000000
# 并使每个 CPU 跳转到该地址
# kernel.ld 会将以下代码放置在 0x80000000

.section .text
.global _entry
_entry:
        # 栈从高地址向低地址增长
        # stack0在start.c中声明 (4096字节/CPU)
        la sp, stack0           # sp = stack0
        li a0, 1024*4           # a0 = 4096
        csrr a1, mhartid        # a1 = mhartid
        addi a1, a1, 1          # a1 = mhartid + 1
        mul a0, a0, a1          # a0 = 4096 * (mhartid + 1)
        add sp, sp, a0          # sp = stack0 + 4096 * (mhartid + 1)
        call start              # --> start.c

spin:
        j spin
