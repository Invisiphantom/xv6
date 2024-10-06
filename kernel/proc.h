
// 内核上下文切换需要保存的寄存器
struct context {
    uint64 ra;  // 函数返回地址(Return address)
    uint64 sp;  // 进程栈指针(Stack pointer)

    // 被调用者保存寄存器
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
};

// 每个CPU的状态
struct cpu {
    struct proc* proc;       // 当前CPU上运行的进程, 或者为空
    struct context context;  // 当前保存的调度器上下文 scheduler() <proc.c>
    int off_num;             // 中断禁用的次数 (push_off增加计数 pop_off减少计数)
    int intr_enable;         // 中断在 push_off 之前是否被启用
};

extern struct cpu cpus[NCPU];

struct trapframe {
    // usertrapret() 设置下述变量
    /*   0 */ uint64 kernel_satp;    // 保存内核页表寄存器
    /*   8 */ uint64 kernel_sp;      // 内核栈顶 (从高地址向低地址增长)
    /*  16 */ uint64 kernel_trap;    // 函数地址usertrap()
    /*  24 */ uint64 epc;            // 保存用户中断处PC
    /*  32 */ uint64 kernel_hartid;  // 保存CPU的ID

    /*  40 */ uint64 ra;
    /*  48 */ uint64 sp;
    /*  56 */ uint64 gp;
    /*  64 */ uint64 tp;
    /*  72 */ uint64 t0;
    /*  80 */ uint64 t1;
    /*  88 */ uint64 t2;
    /*  96 */ uint64 s0;
    /* 104 */ uint64 s1;
    /* 112 */ uint64 a0;
    /* 120 */ uint64 a1;
    /* 128 */ uint64 a2;
    /* 136 */ uint64 a3;
    /* 144 */ uint64 a4;
    /* 152 */ uint64 a5;
    /* 160 */ uint64 a6;
    /* 168 */ uint64 a7;
    /* 176 */ uint64 s2;
    /* 184 */ uint64 s3;
    /* 192 */ uint64 s4;
    /* 200 */ uint64 s5;
    /* 208 */ uint64 s6;
    /* 216 */ uint64 s7;
    /* 224 */ uint64 s8;
    /* 232 */ uint64 s9;
    /* 240 */ uint64 s10;
    /* 248 */ uint64 s11;
    /* 256 */ uint64 t3;
    /* 264 */ uint64 t4;
    /* 272 */ uint64 t5;
    /* 280 */ uint64 t6;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// 每个进程的状态
struct proc {
    struct spinlock lock;

    // 当使用下述变量时必须持有p->lock
    enum procstate state;  // 当前进程状态
    void* chan;            // 如果非零, 则进程正在 chan 上睡眠
    int killed;            // 如果非零, 则进程已被终止
    int xstate;            // 退出状态, 将返回给父进程的 wait
    int pid;               // Process ID

    // 当使用下述变量时必须持有wait_lock
    struct proc* parent;  // 父进程

    // 下述变量是进程私有的, 所以不需要持有p->lock
    uint64 kstack;                // 内核栈的虚拟地址
    uint64 sz;                    // 进程内存大小(字节)
    pagetable_t pagetable;        // 用户页表
    struct trapframe* trapframe;  // data page for trampoline.S
    struct context context;       // 进程上下文
    struct file* ofile[NOFILE];   // 已打开的文件
    struct minode* cwd;           // 当前工作目录 (inode)
    char name[16];                // 进程名 (debugging)
};