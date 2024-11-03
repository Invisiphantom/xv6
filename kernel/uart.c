

// UART-16550a 设备驱动代码
// 文档: http://byterunner.com/16550.html
// 通用异步收发器 (Universal Asynchronous Receiver-Transmitter)

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// 获取UART控制寄存器 的内存地址
#define Reg(reg) ((volatile unsigned char*)(UART0 + (reg)))

// UART控制寄存器
#define RHR 0 // 接收保持寄存器 单字符读 (Receive Holding Reg)
#define THR 0 // 发送保持寄存器 单字符写 (Transmit Holding Reg)

#define IER 1                  // 中断使能寄存器
#define IER_RX_ENABLE (1 << 0) // 启用接收中断
#define IER_TX_ENABLE (1 << 1) // 启用发送中断

#define FCR 2                    // FIFO 控制寄存器
#define FCR_FIFO_ENABLE (1 << 0) // 是否启用
#define FCR_FIFO_CLEAR (3 << 1)  // clear the content of the two FIFOs

#define ISR 2 // Interrupt Status Reg (中断状态寄存器)

#define LCR 3                   // Line Control Reg (线路控制寄存器)
#define LCR_EIGHT_BITS (3 << 0) // 收发比特长度为8位
#define LCR_BAUD_LATCH (1 << 7) // 波特率设置模式

#define LSR 5                 // Line Status Reg (线路状态寄存器)
#define LSR_RX_READY (1 << 0) // input is waiting to be read from RHR
#define LSR_TX_IDLE (1 << 5)  // THR此时空闲, 可以接受字符发送

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// THR的发送缓冲区
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];

uint64 uart_tx_w; // 可写入位置 uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE]
uint64 uart_tx_r; // 欲读取位置 uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE]

extern volatile int panicked; // 来自printf.c

void uartstart();

// 初始化 UART 控制器
void uartinit(void)
{
    // 关闭中断
    WriteReg(IER, 0x00);

    // 进入 波特率设置模式
    WriteReg(LCR, LCR_BAUD_LATCH);

    // 设置波特率为 38.4K
    WriteReg(0, 0x03); // LSB(低有效位)
    WriteReg(1, 0x00); // MSB(高有效位)

    // 退出 波特率设置模式
    // 设置数据位为8位, 无校验位
    WriteReg(LCR, LCR_EIGHT_BITS);

    // 重置并启用 FIFOs
    WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

    // 启用发送中断和接受中断
    WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);

    // 初始化UART锁
    initlock(&uart_tx_lock, "uart");
}

// 用户态UART输出
// 向输出缓冲区添加单字符, 并告诉UART开始发送
// 如果输出缓冲区已满, 则休眠进程来等待空间
// sys_write -> <file.c>filewrite() --> <console.c>consolewrite()
void uartputc(int c)
{
    acquire(&uart_tx_lock); //*

    // 如果内核崩溃, 则无限循环
    if (panicked)
        for (;;)
            ;

    // 如果当前缓冲区已满, 则先等待uartstart()发送
    while (uart_tx_w == uart_tx_r + UART_TX_BUF_SIZE)
        sleep(&uart_tx_r, &uart_tx_lock); //*

    // 将字符写入缓冲区
    uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE] = c;
    uart_tx_w += 1;

    // 将缓冲区字符发送到UART
    uartstart();

    release(&uart_tx_lock); //*
}

// 内核态UART输出
// console.c->consputc()中调用
void uartputc_sync(int c)
{
    push_off(); //* 禁用中断

    if (panicked)
        for (;;)
            ;

    // 循环直到THR空闲
    while ((ReadReg(LSR) & LSR_TX_IDLE) == 0)
        ;

    // 将字符写入THR
    WriteReg(THR, c);

    pop_off(); //* 恢复之前的中断状态
}

// 如果UART空闲, 并且发送缓冲区中有字符, 则发送
// 调用者必须持有uart_tx_lock
void uartstart()
{
    while (1) {
        // 如果缓冲区已空, 则退出
        if (uart_tx_w == uart_tx_r)
            return;

        // 如果THR还正在发送, 则退出等待下次中断
        if ((ReadReg(LSR) & LSR_TX_IDLE) == 0)
            return;

        // 从缓冲区中取出一个字符
        int c = uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE];
        uart_tx_r += 1;

        // 唤醒可能正在等待缓冲区的uartputc()
        wakeup(&uart_tx_r);

        // 将字符写入THR
        WriteReg(THR, c);
    }
}

// 从UART读取单个字符
// 如果没有字符可读, 则返回-1
int uartgetc(void)
{
    // 如果有字符可读, 则返回RHR
    if (ReadReg(LSR) & 0x01)
        return ReadReg(RHR);
    else
        return -1;
}

// 处理终端输入中断, 读取输入字符, 并处理输出缓冲区
// 在trap.c->devintr()的键盘中断调用
void uartintr(void)
{
    // 读取并处理输入字符
    while (1) {
        // 从UART读取单字符
        int c = uartgetc();

        // 如果没有字符可读, 则退出
        if (c == -1)
            break;

        // 根据字符, 处理终端行为
        consoleintr(c);
    }

    // 发送缓冲区中的字符
    acquire(&uart_tx_lock);
    uartstart();
    release(&uart_tx_lock);
}
