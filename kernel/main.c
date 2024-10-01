#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

// start.c 将M-mode切换为S-mode后, 跳转到此处
void main() {
    if (cpuid() == 0) {
        consoleinit();  // 初始化终端 (UART)
        printfinit();   // 初始化printf

        printf("\n");
        printf("xv6 kernel is booting\n");
        printf("\n");

        kinit();        // 初始化内存分配器
        kvminit();      // 初始化内核页表
        kvminithart();  // 当前CPU 启用Sv39分页

        procinit();  // 初始化进程表

        trapinit();      // 初始化计时器中断锁
        trapinithart();  // 当前CPU 设置stvec跳转到kernelvec.S

        plicinit();      // 设置外部中断优先级 (UART VirtIO)
        plicinithart();  // 当前CPU 启用UART和VirtIO中断

        binit();             // buffer cache
        iinit();             // inode table
        fileinit();          // file table
        virtio_disk_init();  // emulated hard disk
        
        userinit();          // first user process

        __sync_synchronize();  // 同步内存

        started = 1;
    } else {
        while (started == 0)
            ;
        __sync_synchronize();  // 同步内存
        printf("hart %d starting\n", cpuid());
        kvminithart();   // 当前CPU 启用Sv39分页
        trapinithart();  // 当前CPU 设置stvec跳转到kernelvec.S
        plicinithart();  // 当前CPU 启用UART和VirtIO中断
    }

    scheduler();
}
