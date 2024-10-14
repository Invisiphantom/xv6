// 物理内存布局

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtio disk
// 80000000 -- boot ROM 跳转到此处 (M-mode)
//             -kernel 参数指定的内核程序加载到此处

// 80000000 -- entry.S
// 然后是内核程序的text段和data段
// end -- 内核空闲页链表的开始地址
// PHYSTOP -- 内核程序的最大物理内存地址

// UART寄存器 物理内存地址
#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio mmio 接口
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// PLIC (Platform-Level Interrupt Controller)
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)

// PLIC S-mode 中断使能寄存器
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart) * 0x100)
// PLIC S-mode 中断优先级寄存器
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart) * 0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart) * 0x2000)

// 0x80000000->PHYSTOP 内核与用户的内存页
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 128 * 1024 * 1024)

// 内核虚拟内存布局 (vm.c->kvmmake)
// >低地址
//      UART
//      VirtIO
//      PLIC
//      内核代码段
//      内核其余物理内存
//      ...
//      proc[NPROC-1]内核栈
//      guard page
//      proc[NPROC-2]内核栈
//      guard page
//      ...
//      proc[0]内核栈
//      guard page
//      TRAMPOLINE
// >高地址
// 依次映射每个进程的内核栈到内核空间
#define KSTACK(p) (TRAMPOLINE - ((p) + 1) * 2 * PGSIZE)

// 用户虚拟内存布局 (proc.c->proc_pagetable)
// >低地址
//   代码段
//   数据段
//   用户栈空间
//   用户堆空间
//   ...
//   TRAPFRAME (p->trapframe)
//   TRAMPOLINE (内核代码段trampoline.S)
// >高地址
#define TRAMPOLINE (MAXVA - PGSIZE) // trampoline页映射到最高虚拟地址, 用于用户和内核空间
#define TRAPFRAME (TRAMPOLINE - PGSIZE) // trapframe页映射到trampoline页的相邻低地址
