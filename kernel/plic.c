#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// RISC-V 平台级中断控制器
// Platform Level Interrupt Controller (PLIC)


void plicinit(void) {
    // 将需要接受的中断优先级设置为非零(否则被禁用)
    *(uint32*)(PLIC + UART0_IRQ * 4) = 1;
    *(uint32*)(PLIC + VIRTIO0_IRQ * 4) = 1;
}

void plicinithart(void) {
    int hart = cpuid();

    // 为每个CPU启用UART和VirtIO中断
    *(uint32*)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

    // 将每个CPU的优先级阈值设置为0
    *(uint32*)PLIC_SPRIORITY(hart) = 0;
}

// 询问PLIC我们应该处理哪个中断
int plic_claim(void) {
    int hart = cpuid();
    int irq = *(uint32*)PLIC_SCLAIM(hart);
    return irq;
}

// 告诉PLIC我们已经处理了这个中断
void plic_complete(int irq) {
    int hart = cpuid();
    *(uint32*)PLIC_SCLAIM(hart) = irq;
}
