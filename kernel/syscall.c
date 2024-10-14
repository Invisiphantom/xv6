#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// 获取当前进程地址中 addr处 的uint64
int fetchaddr(uint64 addr, uint64* ip)
{
    struct proc* p = myproc();

    if (addr >= p->sz
        || addr + sizeof(uint64) > p->sz) // both tests needed, in case of overflow
        return -1;

    // 从用户空间 复制数据到 内核空间
    // dst=ip, srcva=addr, len=sizeof(uint64)
    if (copyin(p->pagetable, (char*)ip, addr, sizeof(*ip)) != 0)
        return -1;
    return 0;
}

// 获取当前进程地址中 addr处 以null结尾的字符串
// 返回字符串长度(不包括null), 失败则返回-1
int fetchstr(uint64 addr, char* buf, int max)
{
    struct proc* p = myproc();
    if (copyinstr(p->pagetable, buf, addr, max) < 0)
        return -1;
    return strlen(buf);
}

static uint64 argraw(int n)
{
    struct proc* p = myproc(); // 获取当前进程
    switch (n) {
        case 0:
            return p->trapframe->a0; // 第一个参数
        case 1:
            return p->trapframe->a1; // 第二个参数
        case 2:
            return p->trapframe->a2; // 第三个参数
        case 3:
            return p->trapframe->a3; // 第四个参数
        case 4:
            return p->trapframe->a4; // 第五个参数
        case 5:
            return p->trapframe->a5; // 第六个参数
    }
    panic("argraw");
    return -1;
}

// 获取第n个系统调用参数, 并将其作为32位int返回
void argint(int n, int* ip) { *ip = argraw(n); }

// 获取第n个系统调用参数, 并将其值写入ip
void argaddr(int n, uint64* ip) { *ip = argraw(n); }

// 获取第n个系统调用参数, 并将其作为以null结尾的字符串返回
// 将其拷贝到buf中, 至多存max个字符
// 如果成功就返回字符串长度(包括null), 失败则返回-1
int argstr(int n, char* buf, int max)
{
    uint64 addr;
    argaddr(n, &addr);
    return fetchstr(addr, buf, max);
}

// 系统调用函数原型
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_sleep(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);

// 系统调用函数映射表
static uint64 (*syscalls[])(void) = {
    [SYS_fork] sys_fork,
    [SYS_exit] sys_exit,
    [SYS_wait] sys_wait,
    [SYS_pipe] sys_pipe,
    [SYS_read] sys_read,
    [SYS_kill] sys_kill,
    [SYS_exec] sys_exec,
    [SYS_fstat] sys_fstat,
    [SYS_chdir] sys_chdir,
    [SYS_dup] sys_dup,
    [SYS_getpid] sys_getpid,
    [SYS_sbrk] sys_sbrk,
    [SYS_sleep] sys_sleep,
    [SYS_uptime] sys_uptime,
    [SYS_open] sys_open,
    [SYS_write] sys_write,
    [SYS_mknod] sys_mknod,
    [SYS_unlink] sys_unlink,
    [SYS_link] sys_link,
    [SYS_mkdir] sys_mkdir,
    [SYS_close] sys_close,
};

// 处理系统调用
void syscall(void)
{
    int num;
    struct proc* p = myproc();

    num = p->trapframe->a7; // 系统调用编号

    if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
        // 如果num在合法范围内, 并且对应的系统调用函数存在
        // 通过编号来访问系统调用, 并将返回值存储在a0中
        p->trapframe->a0 = syscalls[num]();
    } else {
        printf("%d %s: unknown sys call %d\n", p->pid, p->name, num);
        p->trapframe->a0 = -1;
    }
}
