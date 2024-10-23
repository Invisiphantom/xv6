

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
#define RHR 0 // 接收保持寄存器 (Receive Holding Reg)
#define THR 0 // 发送保持寄存器 (Transmit Holding Reg)

#define IER 1                  // 中断使能寄存器
#define IER_RX_ENABLE (1 << 0) // 接收中断
#define IER_TX_ENABLE (1 << 1) // 发送中断

#define FCR 2                    // FIFO 控制寄存器
#define FCR_FIFO_ENABLE (1 << 0) // 是否启用
#define FCR_FIFO_CLEAR (3 << 1)  // clear the content of the two FIFOs

#define ISR 2 // Interrupt Status Reg (中断状态寄存器)

#define LCR 3                   // Line Control Reg (线路控制寄存器)
#define LCR_EIGHT_BITS (3 << 0) // 收发比特长度为8位
#define LCR_BAUD_LATCH (1 << 7) // 波特率设置模式

#define LSR 5                 // Line Status Reg (线路状态寄存器)
#define LSR_RX_READY (1 << 0) // input is waiting to be read from RHR
#define LSR_TX_IDLE (1 << 5)  // THR can accept another character to send

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// the transmit output buffer.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
uint64 uart_tx_w; // write next to uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE]
uint64 uart_tx_r; // read next from uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE]

extern volatile int panicked; // from printf.c

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

// add a character to the output buffer and tell the
// UART to start sending if it isn't already.
// blocks if the output buffer is full.
// because it may block, it can't be called
// from interrupts; it's only suitable for use
// by write().
void uartputc(int c)
{
    acquire(&uart_tx_lock);

    if (panicked) {
        for (;;)
            ;
    }
    while (uart_tx_w == uart_tx_r + UART_TX_BUF_SIZE) {
        // buffer is full.
        // wait for uartstart() to open up space in the buffer.
        sleep(&uart_tx_r, &uart_tx_lock);
    }
    uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE] = c;
    uart_tx_w += 1;
    uartstart();
    release(&uart_tx_lock);
}

// alternate version of uartputc() that doesn't
// use interrupts, for use by kernel printf() and
// to echo characters. it spins waiting for the uart's
// output reg to be empty.
void uartputc_sync(int c)
{
    push_off();

    if (panicked) {
        for (;;)
            ;
    }

    // wait for Transmit Holding Empty to be set in LSR.
    while ((ReadReg(LSR) & LSR_TX_IDLE) == 0)
        ;
    WriteReg(THR, c);

    pop_off();
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it.
// caller must hold uart_tx_lock.
// called from both the top- and bottom-half.
void uartstart()
{
    while (1) {
        if (uart_tx_w == uart_tx_r) {
            // transmit buffer is empty.
            ReadReg(ISR);
            return;
        }

        if ((ReadReg(LSR) & LSR_TX_IDLE) == 0) {
            // the UART transmit holding reg is full,
            // so we cannot give it another byte.
            // it will interrupt when it's ready for a new byte.
            return;
        }

        int c = uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE];
        uart_tx_r += 1;

        // maybe uartputc() is waiting for space in the buffer.
        wakeup(&uart_tx_r);

        WriteReg(THR, c);
    }
}

// read one input character from the UART.
// return -1 if none is waiting.
int uartgetc(void)
{
    if (ReadReg(LSR) & 0x01) {
        // input data is ready.
        return ReadReg(RHR);
    } else {
        return -1;
    }
}

// 被devintr()调用
// 当有新输入字符, 或者输出缓冲区有空间时
// 处理uart中断, 读取输入字符, 并处理输出缓冲区
void uartintr(void)
{
    // 读取并处理输入字符
    while (1) {
        int c = uartgetc();
        if (c == -1)
            break;
        consoleintr(c);
    }

    // 发送缓冲区中的字符
    acquire(&uart_tx_lock);
    uartstart();
    release(&uart_tx_lock);
}
