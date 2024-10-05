
#include "syscall.h"

.globl start
start:
        # exec(init, argv)
        la a0, init
        la a1, argv
        li a7, SYS_exec
        ecall

exit:
        # 循环执行 exit()
        li a7, SYS_exit
        ecall
        jal exit

init: # init[] = "/init\0"
  .string "/init\0"

.p2align 2
argv: # argv[] = { init, 0 }
  .quad init
  .quad 0
