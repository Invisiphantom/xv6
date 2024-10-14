//
// formatted console output -- printf, panic.
//

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

volatile int panicked = 0;

// 加锁避免交错并发执行printf
static struct {
    struct spinlock lock;
    int locking;
} pr;

static char digits[] = "0123456789abcdef";

// base: 进制   sign: 符号
static void printint(long long xx, int base, int sign)
{
    char buf[16];
    int i;
    unsigned long long x;

    // 有符号数取绝对值, 并将符号标记
    if (sign && (sign = (xx < 0)))
        x = -xx;
    else
        x = xx;

    // 按照进制逐位拆分
    i = 0;
    do {
        buf[i++] = digits[x % base];
    } while ((x /= base) != 0);

    // 填充符号
    if (sign)
        buf[i++] = '-';

    // 打印缓冲区到终端
    while (--i >= 0)
        consputc(buf[i]);
}

static void printptr(uint64 x)
{
    int i;
    consputc('0');
    consputc('x');
    for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// 打印到终端
int printf(char* fmt, ...)
{
    va_list ap; // 可变参数列表
    int i, cx, c0, c1, c2, locking;
    char* s;

    // 如果锁功能启用, 则获取锁
    locking = pr.locking;
    if (locking)
        acquire(&pr.lock);

    va_start(ap, fmt); // 初始化可变参数列表

    for (i = 0; (cx = fmt[i] & 0xff) != 0; i++) {
        // 如果不是%字符, 直接输出
        if (cx != '%') {
            consputc(cx);
            continue;
        }

        i++; // 获取%后的字符
        c0 = fmt[i + 0] & 0xff;
        c1 = c2 = 0;

        if (c0) // 如果不是NULL
            c1 = fmt[i + 1] & 0xff;
        if (c1) // 如果不是NULL
            c2 = fmt[i + 2] & 0xff;

        // 32位有符号整数 (%d)
        if (c0 == 'd') {
            printint(va_arg(ap, int), 10, 1);
        }

        // 64位有符号整数 (%ld %lld)
        else if (c0 == 'l' && c1 == 'd') {
            printint(va_arg(ap, uint64), 10, 1);
            i += 1;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'd') {
            printint(va_arg(ap, uint64), 10, 1);
            i += 2;
        }

        // 32位无符号整数 (%u)
        else if (c0 == 'u') {
            printint(va_arg(ap, int), 10, 0);
        }

        // 64位无符号整数 (%lu %llu)
        else if (c0 == 'l' && c1 == 'u') {
            printint(va_arg(ap, uint64), 10, 0);
            i += 1;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'u') {
            printint(va_arg(ap, uint64), 10, 0);
            i += 2;
        }

        // 32位十六进制数 (%x)
        else if (c0 == 'x') {
            printint(va_arg(ap, int), 16, 0);
        }

        // 64位十六进制数 (%lx %llx)
        else if (c0 == 'l' && c1 == 'x') {
            printint(va_arg(ap, uint64), 16, 0);
            i += 1;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'x') {
            printint(va_arg(ap, uint64), 16, 0);
            i += 2;
        }

        // 指针地址 (%p)
        else if (c0 == 'p') {
            printptr(va_arg(ap, uint64));
        }

        // 字符串 (%s)
        else if (c0 == 's') {
            if ((s = va_arg(ap, char*)) == 0)
                s = "(null)";
            for (; *s; s++)
                consputc(*s);
        }

        // 单字符 (%%)
        else if (c0 == '%') {
            consputc('%');
        }

        // 结束
        else if (c0 == 0) {
            break;
        }

        // 打印未知字符
        else {
            consputc('%');
            consputc(c0);
        }
    }
    va_end(ap); // 结束可变参数列表

    // 如果锁功能启用, 则释放锁
    if (locking)
        release(&pr.lock);

    return 0;
}

void panic(char* s)
{
    pr.locking = 0;
    printf("panic: ");
    printf("%s\n", s);
    panicked = 1; // freeze uart output from other CPUs
    for (;;)
        ;
}

void printfinit(void)
{
    initlock(&pr.lock, "pr"); // 初始化pr锁
    pr.locking = 1;           // 启用pr锁
}
