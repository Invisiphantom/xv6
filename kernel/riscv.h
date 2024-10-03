#ifndef __ASSEMBLER__

// which hart (core) is this?
static inline uint64 r_mhartid() {
    uint64 x;
    asm volatile("csrr %0, mhartid" : "=r"(x));
    return x;
}

// Machine Status Register, mstatus

#define MSTATUS_MPP_MASK (3L << 11)  // previous mode.
#define MSTATUS_MPP_M (3L << 11)
#define MSTATUS_MPP_S (1L << 11)
#define MSTATUS_MPP_U (0L << 11)
#define MSTATUS_MIE (1L << 3)  // machine-mode interrupt enable.

static inline uint64 r_mstatus() {
    uint64 x;
    asm volatile("csrr %0, mstatus" : "=r"(x));
    return x;
}

static inline void w_mstatus(uint64 x) {
    asm volatile("csrw mstatus, %0" : : "r"(x));
}

// machine exception program counter, holds the
// instruction address to which a return from
// exception will go.
static inline void w_mepc(uint64 x) {
    asm volatile("csrw mepc, %0" : : "r"(x));
}

// 全局状态
// Supervisor Status Register, sstatus

#define SSTATUS_SPP (1L << 8)   // Previous mode, 1=Supervisor, 0=User
#define SSTATUS_SPIE (1L << 5)  // Supervisor Previous Interrupt Enable
#define SSTATUS_UPIE (1L << 4)  // User Previous Interrupt Enable
#define SSTATUS_SIE (1L << 1)   // Supervisor Interrupt Enable
#define SSTATUS_UIE (1L << 0)   // User Interrupt Enable

static inline uint64 r_sstatus() {
    uint64 x;
    asm volatile("csrr %0, sstatus" : "=r"(x));
    return x;
}

static inline void w_sstatus(uint64 x) {
    asm volatile("csrw sstatus, %0" : : "r"(x));
}

// Supervisor Interrupt Pending
static inline uint64 r_sip() {
    uint64 x;
    asm volatile("csrr %0, sip" : "=r"(x));
    return x;
}

static inline void w_sip(uint64 x) {
    asm volatile("csrw sip, %0" : : "r"(x));
}

// S-mode 中断使能
#define SIE_SEIE (1L << 9)  // 外部设备中断
#define SIE_STIE (1L << 5)  // 定时器中断
#define SIE_SSIE (1L << 1)  // 软中断

static inline uint64 r_sie() {
    uint64 x;
    asm volatile("csrr %0, sie" : "=r"(x));
    return x;
}

static inline void w_sie(uint64 x) {
    asm volatile("csrw sie, %0" : : "r"(x));
}

// Machine-mode Interrupt Enable
#define MIE_STIE (1L << 5)  // supervisor timer
static inline uint64 r_mie() {
    uint64 x;
    asm volatile("csrr %0, mie" : "=r"(x));
    return x;
}

static inline void w_mie(uint64 x) {
    asm volatile("csrw mie, %0" : : "r"(x));
}

// 异常发生处地址
// supervisor exception program counter, holds the
// instruction address to which a return from
// exception will go.
static inline void w_sepc(uint64 x) {
    asm volatile("csrw sepc, %0" : : "r"(x));
}

static inline uint64 r_sepc() {
    uint64 x;
    asm volatile("csrr %0, sepc" : "=r"(x));
    return x;
}

// Machine Exception Delegation
static inline uint64 r_medeleg() {
    uint64 x;
    asm volatile("csrr %0, medeleg" : "=r"(x));
    return x;
}

static inline void w_medeleg(uint64 x) {
    asm volatile("csrw medeleg, %0" : : "r"(x));
}

// Machine Interrupt Delegation
static inline uint64 r_mideleg() {
    uint64 x;
    asm volatile("csrr %0, mideleg" : "=r"(x));
    return x;
}

static inline void w_mideleg(uint64 x) {
    asm volatile("csrw mideleg, %0" : : "r"(x));
}

// S-mode 异常处理程序地址
// Supervisor Trap-Vector Base Address
// low two bits are mode.
static inline void w_stvec(uint64 x) {
    asm volatile("csrw stvec, %0" : : "r"(x));
}

static inline uint64 r_stvec() {
    uint64 x;
    asm volatile("csrr %0, stvec" : "=r"(x));
    return x;
}

// S-mode 比较计时寄存器 (计时器中断)
// Supervisor Timer Comparison Register
static inline uint64 r_stimecmp() {
    uint64 x;
    // asm volatile("csrr %0, stimecmp" : "=r" (x) );
    asm volatile("csrr %0, 0x14d" : "=r"(x));
    return x;
}

static inline void w_stimecmp(uint64 x) {
    // asm volatile("csrw stimecmp, %0" : : "r" (x));
    asm volatile("csrw 0x14d, %0" : : "r"(x));
}

// Machine Environment Configuration Register
static inline uint64 r_menvcfg() {
    uint64 x;
    // asm volatile("csrr %0, menvcfg" : "=r" (x) );
    asm volatile("csrr %0, 0x30a" : "=r"(x));
    return x;
}

static inline void w_menvcfg(uint64 x) {
    // asm volatile("csrw menvcfg, %0" : : "r" (x));
    asm volatile("csrw 0x30a, %0" : : "r"(x));
}

// Physical Memory Protection
static inline void w_pmpcfg0(uint64 x) {
    asm volatile("csrw pmpcfg0, %0" : : "r"(x));
}

static inline void w_pmpaddr0(uint64 x) {
    asm volatile("csrw pmpaddr0, %0" : : "r"(x));
}

// 使用Sv39分页模式
#define SATP_SV39 (8L << 60)
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12))

// supervisor address translation and protection;
// holds the address of the page table.
static inline void w_satp(uint64 x) {
    asm volatile("csrw satp, %0" : : "r"(x));
}

static inline uint64 r_satp() {
    uint64 x;
    asm volatile("csrr %0, satp" : "=r"(x));
    return x;
}

// 异常发生原因
// Supervisor Trap Cause
static inline uint64 r_scause() {
    uint64 x;
    asm volatile("csrr %0, scause" : "=r"(x));
    return x;
}

// Supervisor Trap Value
static inline uint64 r_stval() {
    uint64 x;
    asm volatile("csrr %0, stval" : "=r"(x));
    return x;
}

// Machine-mode Counter-Enable
static inline void w_mcounteren(uint64 x) {
    asm volatile("csrw mcounteren, %0" : : "r"(x));
}

static inline uint64 r_mcounteren() {
    uint64 x;
    asm volatile("csrr %0, mcounteren" : "=r"(x));
    return x;
}

// machine-mode cycle counter
static inline uint64 r_time() {
    uint64 x;
    asm volatile("csrr %0, time" : "=r"(x));
    return x;
}

// 启用设备中断
static inline void intr_on() {
    w_sstatus(r_sstatus() | SSTATUS_SIE);
}

// 关闭设备中断
static inline void intr_off() {
    w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}

// 是否开启设备中断 1:开启  0:关闭
static inline int intr_get() {
    uint64 x = r_sstatus();
    return (x & SSTATUS_SIE) != 0;
}

static inline uint64 r_sp() {
    uint64 x;
    asm volatile("mv %0, sp" : "=r"(x));
    return x;
}

// read and write tp, the thread pointer, which xv6 uses to hold
// this core's hartid (core number), the index into cpus[].
static inline uint64 r_tp() {
    uint64 x;
    asm volatile("mv %0, tp" : "=r"(x));
    return x;
}

static inline void w_tp(uint64 x) {
    asm volatile("mv tp, %0" : : "r"(x));
}

static inline uint64 r_ra() {
    uint64 x;
    asm volatile("mv %0, ra" : "=r"(x));
    return x;
}

// 清空TLB, 使页表项写入生效
static inline void sfence_vma() {
    // the zero, zero means flush all TLB entries.
    asm volatile("sfence.vma zero, zero");
}

typedef uint64 pte_t;
typedef uint64* pagetable_t;  // 512 PTEs

#endif  // __ASSEMBLER__

#define PGSIZE 4096  // 每页大小
#define PGSHIFT 12   // 页内地址偏移

// 对齐到PGSIZE
#define PGROUNDUP(sz) (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE - 1))

#define PTE_V (1L << 0)  // 有效位
#define PTE_R (1L << 1)  // 内核可读位
#define PTE_W (1L << 2)  // 内核可写位
#define PTE_X (1L << 3)  // 内核可执行位
#define PTE_U (1L << 4)  // 用户访问位

#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10)  // 用物理地址 构造 页表项
#define PTE2PA(pte) (((pte) >> 10) << 12)        // 提取页表项 中的 物理地址
#define PTE_FLAGS(pte) ((pte) & 0x3FF)           // 提取页表项 中的 标志位

// 提取虚拟地址中的三个9位页表索引
#define PXMASK 0x1FF                              // 9 bits
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))  // 每级索引偏移量
#define PX(level, va) ((((uint64)(va)) >> PXSHIFT(level)) & PXMASK)

// MAXVA 实际上比 Sv39 允许的最大值少一位
// 以避免处理高位设置时的符号扩展问题
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))
