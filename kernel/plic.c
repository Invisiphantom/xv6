#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// RISC-V 平台级中断控制器 (PLIC)
// the riscv Platform Level Interrupt Controller (PLIC)

void plicinit(void) {
    // set desired IRQ priorities non-zero (otherwise disabled).
    *(uint32*)(PLIC + UART0_IRQ * 4) = 1;
    *(uint32*)(PLIC + VIRTIO0_IRQ * 4) = 1;
}

void plicinithart(void) {
    int hart = cpuid();

    // set enable bits for this hart's S-mode
    // for the uart and virtio disk.
    *(uint32*)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

    // set this hart's S-mode priority threshold to 0.
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
