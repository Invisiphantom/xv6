#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S 需要给每个CPU一个栈
__attribute__((aligned(16))) char stack0[4096 * NCPU];

// entry.S 跳转到此处 (M-mode)
void start() {
    // 将MPP位设置为01, 使得mret切换为S-mode
    uint64 x = r_mstatus();
    x &= ~MSTATUS_MPP_MASK;  // 清除MPP位
    x |= MSTATUS_MPP_S;      // 设置MPP位为S-mode
    w_mstatus(x);

    // 设置mepc为main(), 用于mret跳转
    // 需要内存模型为 gcc -mcmodel=medany
    w_mepc((uint64)main);

    w_satp(0);          // 关闭页表, 直接访问物理地址
    w_medeleg(0xffff);  // 将所有异常委托给S-mode
    w_mideleg(0xffff);  // 将所有中断委托给S-mode

    // 启用S-mode 外部中断, 定时器中断, 软中断
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

    // 允许S-mode访问所有物理内存
    w_pmpaddr0(0x3fffffffffffffull);
    w_pmpcfg0(0xf);

    // 启用定时器中断
    timerinit();

    // 将每个CPU的hartid保存到tp寄存器中, 用于cpuid()
    int id = r_mhartid();
    w_tp(id);

    // mret切换为S-mode, 并跳转到main()
    asm volatile("mret");
}

// 请求每个CPU启用定时器中断
void timerinit() {
    // 启用M-mode的定时器中断
    w_mie(r_mie() | MIE_STIE);

    // 启用 Sstc 扩展 (S-mode stimecmp)
    w_menvcfg(r_menvcfg() | (1L << 63));

    // 允许S-mode使用stimecmp和time
    w_mcounteren(r_mcounteren() | 2);

    // 设定第一次定时器中断 (大约0.1秒)
    w_stimecmp(r_time() + 1000000);
}
