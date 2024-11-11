
// 文件系统实现:
//  + UART: 串口输入输出 (printf.c console.c uart.c)
//  -------------------------------------------------
//  + FS.img: 文件系统映像 (mkfs.c)
//  + VirtIO: 虚拟硬盘驱动 (virtio.h virtio_disk.c)
//  + BCache: LRU缓存链环 (buf.h bio.c)
//  + Log: 两步提交的日志系统 (log.c)
//  + Inode Dir Path: 硬盘文件系统实现 (stat.h fs.h fs.c)
//  + File SysCall: 文件系统调用 (file.h file.c pipe.c sysfile.c)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [      0     |      1      | 2       31 | 32        44 |      45      | 46     1999 ]

// 虚拟硬盘驱动 QEMU Memory Mapped I/O (MMIO) Interface of Virtio
// qemu -drive file=fs.img,if=none,format=raw,id=x0
//      -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
// https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

// VirtIO寄存器的内存地址
#define R(r) ((volatile uint32*)(VIRTIO0 + (r)))

static struct disk {
    // DMA描述符表, 用于指示设备读写硬盘操作
    struct virtq_desc* desc; // (init分配内存 NUM个)

    // 环形缓冲区, 驱动在其写入需要设备处理的描述符编号 (仅包括每个链的头部)
    struct virtq_avail* avail; // (init分配内存 NUM个)

    // 环形缓冲区, 设备在其写入已处理的描述符编号 (仅包括每个链的头部)
    struct virtq_used* used; // (init分配内存 NUM个)

    char free[NUM];       // 此描述符是否空闲
    uint16 last_used_idx; // 用来判断disk.used->idx是否更新

    // 跟踪正在进行的操作的信息
    // 以便在完成中断到达时使用
    // 按链的第一个描述符进行索引
    struct {
        struct buf* b;
        char status; // 状态结果
    } info[NUM];

    // 硬盘请求头 (和描述符一一配对)
    struct virtio_blk_req ops[NUM];

    struct spinlock vdisk_lock; // 虚拟硬盘锁
} disk;

void virtio_disk_init(void)
{
    uint32 status = 0;

    initlock(&disk.vdisk_lock, "virtio_disk"); // 初始化虚拟硬盘锁

    // 魔数:virt  设备版本:2  设备类型:2  供应者:QEMU
    if (*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 || *R(VIRTIO_MMIO_VERSION) != 2
        || *R(VIRTIO_MMIO_DEVICE_ID) != 2 || *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
        panic("could not find virtio disk");
    }

    // 重置设备状态
    *R(VIRTIO_MMIO_STATUS) = status;

    // 设置ACKNOWLEDGE状态位
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *R(VIRTIO_MMIO_STATUS) = status;

    // 设置DRIVER状态位
    status |= VIRTIO_CONFIG_S_DRIVER;
    *R(VIRTIO_MMIO_STATUS) = status;

    // 获取设备支持的特性
    uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);

    // 选择驱动使用的特性 (禁用不需要的特性)
    features &= ~(1 << VIRTIO_BLK_F_RO);
    features &= ~(1 << VIRTIO_BLK_F_SCSI);
    features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
    features &= ~(1 << VIRTIO_BLK_F_MQ);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
    *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

    // 通知设备特性 已经协商完成
    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // 重新读取状态 确保FEATURES_OK已设置
    status = *R(VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
        panic("virtio disk FEATURES_OK unset");

    // 初始化为队列0
    *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

    // 确保队列0未被使用
    if (*R(VIRTIO_MMIO_QUEUE_READY))
        panic("virtio disk should not be ready");

    // 读取最大队列大小
    uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0)
        panic("virtio disk has no queue 0");
    if (max < NUM)
        panic("virtio disk max queue too short");

    disk.desc = kalloc();  // 分配描述符表
    disk.avail = kalloc(); // 分配待处理环
    disk.used = kalloc();  // 分配已处理环
    if (!disk.desc || !disk.avail || !disk.used)
        panic("virtio disk kalloc");

    // 清空描述符, 待处理环, 已处理环
    memset(disk.desc, 0, PGSIZE);
    memset(disk.avail, 0, PGSIZE);
    memset(disk.used, 0, PGSIZE);

    // 设置队列大小为NUM
    *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

    // 通知设备描述符, 待处理环, 已处理环的物理地址
    *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
    *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
    *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
    *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
    *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
    *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

    // 通知设备队列已准备好
    *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

    // 初始化所有描述符为未使用
    for (int i = 0; i < NUM; i++)
        disk.free[i] = 1;

    // 设置DRIVER_OK状态位 (驱动加载完成)
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // plic.c和trap.c处理来自VIRTIO0_IRQ的中断
}

// 返回一个空闲描述符的索引 (持有锁)
// 如果没有空闲描述符, 则返回-1
static int alloc_desc()
{
    for (int i = 0; i < NUM; i++)
        if (disk.free[i]) {
            disk.free[i] = 0;
            return i;
        }
    return -1;
}

// 释放单个描述符 (持有锁)
static void free_desc(int i)
{
    if (i >= NUM)
        panic("free_desc: index out of range");
    if (disk.free[i])
        panic("free_desc: already free");
    disk.desc[i].addr = 0;
    disk.desc[i].len = 0;
    disk.desc[i].flags = 0;
    disk.desc[i].next = 0;
    disk.free[i] = 1;

    // 唤醒等待空闲描述符的进程
    wakeup(&disk.free[0]);
}

// 释放描述符链 (持有锁)
static void free_chain(int i)
{
    for (;;) {
        int flag = disk.desc[i].flags;
        int nxt = disk.desc[i].next;
        free_desc(i);

        // 如果有下一个描述符, 继续释放
        if (flag & VRING_DESC_F_NEXT)
            i = nxt;
        else
            break;
    }
}

// 分配三个描述符 (持有锁)
// 如果没有空闲描述符, 则返回-1
static int alloc3_desc(int* idx)
{
    for (int i = 0; i < 3; i++) {
        idx[i] = alloc_desc();

        // 如果没有空闲描述符
        if (idx[i] < 0) {
            // 释放已分配的描述符, 返回-1
            for (int j = 0; j < i; j++)
                free_desc(idx[j]);
            return -1;
        }
    }
    return 0;
}

// 虚拟硬盘读写
// 0:读取  1:写入
void virtio_disk_rw(struct buf* b, int write)
{
    // 计算块号blockno对应的块区号
    uint64 sector = b->blockno * (BSIZE / 512);

    acquire(&disk.vdisk_lock); //* 获取虚拟硬盘锁

    // 规范的第5.2.6.4节指出, 传统的块操作使用三个描述符:
    // 1. 用于指示 读/写 保留位 块区号
    // 2. 用于指向 内存数据区域
    // 3. 用于指导 单字节的操作结果

    int idx[3];
    for (;;) {
        // 分配三个空闲描述符
        if (alloc3_desc(idx) == 0)
            break;
        // 如果没有空闲描述符, 则休眠等待
        sleep(&disk.free[0], &disk.vdisk_lock);
    }

    // 配置三个描述符 (用于QEMU->virtio-blk.c读取)

    // 获取对应的硬盘请求头
    struct virtio_blk_req* buf0 = &disk.ops[idx[0]];

    // 读写类型
    if (write)
        buf0->type = VIRTIO_BLK_T_OUT; // 写入硬盘
    else
        buf0->type = VIRTIO_BLK_T_IN; // 读取硬盘
    buf0->reserved = 0;               // 保留位
    buf0->sector = sector;            // 块区号

    // 配置 idx[0] 描述符
    disk.desc[idx[0]].addr = (uint64)buf0;                 // 请求头的地址
    disk.desc[idx[0]].len = sizeof(struct virtio_blk_req); // 请求头的长度
    disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;           // 指示后面还有描述符
    disk.desc[idx[0]].next = idx[1];                       // 下一个描述符的索引

    // 配置 idx[1] 描述符
    disk.desc[idx[1]].addr = (uint64)b->data; // bio缓冲链块数据地址
    disk.desc[idx[1]].len = BSIZE;            // 长度为块大小 (buf.h)
    if (write)
        disk.desc[idx[1]].flags = 0; // 从buf读数据
    else
        disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // 将数据写入buf
    disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;     // 指示后面还有描述符
    disk.desc[idx[1]].next = idx[2];                  // 下一个描述符的索引

    // 配置 idx[2] 描述符
    disk.info[idx[0]].status = 0xff; // 如果成功将清除此状态
    disk.desc[idx[2]].addr = (uint64)&disk.info[idx[0]].status; // 状态结果的地址
    disk.desc[idx[2]].len = 1;                                  // 长度为1字节
    disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // 将结果写入info.status
    disk.desc[idx[2]].next = 0;                   // 指示没有下一个描述符

    // virtio_disk_intr()
    b->disk = 1;             // 硬盘正在使用buf
    disk.info[idx[0]].b = b; // 记录buf

    // 通知设备 需要处理的描述符链中的第一个索引 是idx[0]
    disk.avail->ring[disk.avail->idx % NUM] = idx[0];

    __sync_synchronize(); // 内存屏障

    // 指示下一个可写入的描述符条目的索引
    disk.avail->idx += 1;

    __sync_synchronize(); // 内存屏障

    // 通知设备有新的可用请求 (队列编号)
    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

    // 休眠等待硬盘中断 virtio_disk_intr() 通知请求已完成
    while (b->disk == true) {
        sleep(b, &disk.vdisk_lock); //* 等待硬盘中断
    }

    // 释放描述符
    disk.info[idx[0]].b = 0;
    free_chain(idx[0]);

    release(&disk.vdisk_lock); //* 释放虚拟硬盘锁
}

// trap.c->devintr 识别硬盘中断后跳转到这里
void virtio_disk_intr()
{
    acquire(&disk.vdisk_lock); //* 获取虚拟硬盘锁

    // 设备在被告知 此中断处理完成之前, 不会产生新中断
    // 在这种情况下, 我们可能在此次中断需要处理多个新完成的条目
    // 而在之后的中断中没有任何事情要做, 这并无害处
    *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

    __sync_synchronize(); // 内存屏障

    // 当设备添加条目到已处理环时, 会递增disk.used->idx
    // 处理已完成的请求 [last_used_idx: disk.used->idx]
    while (disk.last_used_idx != disk.used->idx) {
        __sync_synchronize(); // 内存屏障

        // 获取描述符链的首索引
        int id = disk.used->ring[disk.last_used_idx % NUM].id;

        if (disk.info[id].status != 0)
            panic("virtio_disk_intr status");

        struct buf* b = disk.info[id].b;
        b->disk = false; // 处理结束, 硬盘释放buf
        wakeup(b);   // 唤醒正在等待该buf的进程

        // 继续处理下一个请求
        disk.last_used_idx += 1;
    }

    release(&disk.vdisk_lock); //* 释放虚拟硬盘锁
}
