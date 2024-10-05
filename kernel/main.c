#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

// start.c 跳转到此处 (S-mode)
void main() {
    if (cpuid() == 0) {
        consoleinit();  // 初始化终端 (UART)
        printfinit();   // 初始化printf

        printf("\n");
        printf("xv6 kernel is booting\n");
        printf("\n");

        kinit();        // 初始化kalloc
        kvminit();      // 初始化内核页表
        kvminithart();  // 当前CPU 设置satp 启用Sv39分页

        procinit();  // 初始化进程表

        trapinit();      // 初始化计时器中断锁
        trapinithart();  // 当前CPU 设置stvec跳转到kernelvec.S

        plicinit();      // 设置外部中断优先级 (UART VirtIO)
        plicinithart();  // 当前CPU 启用UART和VirtIO中断

        binit();             // 初始化cache链环
        iinit();             // 初始化inode锁
        fileinit();          // 初始化文件系统锁
        virtio_disk_init();  // 初始化virtio硬盘

        userinit();  // 初始化第一个用户进程

        __sync_synchronize();  // 内存屏障

        started = 1;
    } else {
        while (started == 0)
            ;
        __sync_synchronize();  // 内存屏障
        printf("hart %d starting\n", cpuid());
        kvminithart();   // 当前CPU 设置satp 启用Sv39分页
        trapinithart();  // 当前CPU 设置stvec跳转到kernelvec.S
        plicinithart();  // 当前CPU 启用UART和VirtIO中断
    }

    scheduler();  // 启动调度!
}
