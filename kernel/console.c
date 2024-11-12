#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100
#define C(x) ((x) - '@')

// 将单字符直接写到UART
// 用于printf和终端回显
void consputc(int c)
{
    // 如果用户输入了退格, 就用空格覆盖
    if (c == BACKSPACE) {
        uartputc_sync('\b');
        uartputc_sync(' ');
        uartputc_sync('\b');
    } else
        uartputc_sync(c);
}

#define INPUT_BUF_SIZE 128
struct {
    struct spinlock lock;
    char buf[INPUT_BUF_SIZE]; // 终端行缓冲区
    uint r;                   // 读取索引
    uint w;                   // 已写部分的索引
    uint e;                   // 当前行的编辑索引
} cons;

// 终端设备的写函数 devsw[CONSOLE].write
// file.c->filewrite()中调用, 返回写入的字符数
int consolewrite(int user_src, uint64 src, int n)
{
    int i = 0;
    for (char c; i < n; i++) {
        // 将用户空间的字符 拷贝到内核空间&c
        // dst=&c, user_src=user_src, src=src+i, len=1
        if (either_copyin(&c, user_src, src + i, 1) == -1)
            break;
        uartputc(c);
    }
    return i;
}

// 终端设备的读函数 devsw[CONSOLE].read
int consoleread(int user_dst, uint64 dst, int n)
{
    // 记录期望读取的字符数
    uint target = n;

    acquire(&cons.lock); //*

    while (n > 0) {
        // 如果终端缓冲区此时为空
        while (cons.r == cons.w) {
            // 如果进程有终止标志, 则直接返回-1
            if (killed(myproc())) {
                release(&cons.lock); //*
                return -1;
            }

            // 休眠等待终端输入响应
            sleep(&cons.r, &cons.lock);
        }

        // 从终端缓冲区取出一个字符
        char c = cons.buf[cons.r % INPUT_BUF_SIZE];
        cons.r++;

        // Ctrl+D 文件结束符
        if (c == C('D')) {
            // 如果已经读取了一些字符
            // 那么还原^D, 供下次读取
            if (n < target)
                cons.r--;
            break;
        }

        // 将输入字符 拷贝到 用户空间缓冲区
        if (either_copyout(user_dst, dst, &c, 1) == -1)
            break;

        // 继续下一个字符
        dst++;
        n--;

        // 如果遇到换行符, 则结束读取
        if (c == '\n')
            break;
    }
    release(&cons.lock); //*

    return target - n;
}

// 通过UART实现的终端输入输出 (每次读取一行)

// 实现下述特殊输入字符:
//   Enter  -- end of line
//   Ctrl+H -- 退格
//   Ctrl+U -- kill line
//   Ctrl+D -- 文件结束符
//   Ctrl+P -- 打印进程列表

// 终端输入中断处理函数
// 处理退格/删除, 追加到cons.buf
// 如果一整行已经到达, 就唤醒consoleread()
// 在 <trap.c>devintr() -> <uart.c>uartintr() 中调用
void consoleintr(int c)
{
    acquire(&cons.lock); //* 获取终端锁

    switch (c) {
        // Ctrl+P 打印进程列表
        case C('P'):
            procdump();
            break;

        // Ctrl+U 删除整行
        case C('U'):
            while (cons.e != cons.w && cons.buf[(cons.e - 1) % INPUT_BUF_SIZE] != '\n') {
                cons.e--;
                consputc(BACKSPACE);
            }
            break;

        // Ctrl+H 退格键 删除一个字符
        case C('H'):
        case '\x7f':
            if (cons.e != cons.w) {
                cons.e--;
                consputc(BACKSPACE);
            }
            break;

        // 其他情况 追加到cons.buf
        default:
            // 如果字符有效, 且缓冲区未满, 则追加到缓冲区
            if (c != 0 && cons.e - cons.r < INPUT_BUF_SIZE) {
                c = (c == '\r') ? '\n' : c; // 将\r转换为\n

                // 回显给用户
                consputc(c);

                // 将字符添加到缓冲区, 用于consoleread()
                cons.buf[cons.e % INPUT_BUF_SIZE] = c;
                cons.e++;

                // 如果遇到换行符|结束符, 或者缓冲区已满
                if (c == '\n' || c == C('D') || cons.e - cons.r == INPUT_BUF_SIZE) {
                    cons.w = cons.e; // 更新已写部分的索引
                    wakeup(&cons.r); // 唤醒consoleread()
                }
            }
            break;
    }

    release(&cons.lock); //* 释放终端锁
}

void consoleinit(void)
{
    initlock(&cons.lock, "cons"); // 初始化终端锁

    uartinit(); // 初始化UART控制器

    // 设置终端设备的读写函数
    devsw[CONSOLE].read = consoleread;
    devsw[CONSOLE].write = consolewrite;
}
