
// 文件系统实现:
//  + FS.img: 文件系统映像 (mkfs.c)
//  + Dev+blockno: 虚拟硬盘块设备 (virtio_disk.c)
//  + Bcache: 缓存链环 (bio.c)
//  + Log: 多步更新的崩溃恢复 (log.c)
//  + Inode: inode分配器, 读取, 写入, 元数据 (fs.c)
//  + Directory: 具有特殊内容的inode(其他inode的列表) (fs.c)
//  + Path: 方便命名的路径, 如 /usr/rtm/xv6/fs.c (fs.c)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [          0 |           1 | 2       31 | 32        44 |           45 | 46     1999 ]

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

// the address of virtio mmio register r.
#define R(r) ((volatile uint32*)(VIRTIO0 + (r)))

static struct disk {
    // DMA描述符集, 用于指示设备读写硬盘操作
    // 多数命令需要 由一对描述符 组成的链表
    struct virtq_desc* desc; // (init分配内存 NUM个)

    // 环形缓冲区, 驱动在其写入需要设备处理的描述符编号 (仅包括每个链的头部)
    struct virtq_avail* avail; // (init分配内存 NUM个)

    // 环形缓冲区, 设备在其写入已处理的描述符编号 (仅包括每个链的头部)
    struct virtq_used* used; // (init分配内存 NUM个)

    char free[NUM];  // 此描述符是否空闲
    uint16 used_idx; // 用来判断disk.used->idx是否更新

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

    // 设置设备特性
    uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
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

    // re-read status to ensure FEATURES_OK is set.
    status = *R(VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
        panic("virtio disk FEATURES_OK unset");

    // initialize queue 0.
    *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

    // ensure queue 0 is not in use.
    if (*R(VIRTIO_MMIO_QUEUE_READY))
        panic("virtio disk should not be ready");

    // check maximum queue size.
    uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0)
        panic("virtio disk has no queue 0");
    if (max < NUM)
        panic("virtio disk max queue too short");

    // allocate and zero queue memory.
    disk.desc = kalloc();  // 分配描述符
    disk.avail = kalloc(); // 分配可用环
    disk.used = kalloc();
    if (!disk.desc || !disk.avail || !disk.used)
        panic("virtio disk kalloc");
    memset(disk.desc, 0, PGSIZE);
    memset(disk.avail, 0, PGSIZE);
    memset(disk.used, 0, PGSIZE);

    // set queue size.
    *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

    // write physical addresses.
    *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
    *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
    *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
    *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
    *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
    *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

    // queue is ready.
    *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

    // all NUM descriptors start out unused.
    for (int i = 0; i < NUM; i++)
        disk.free[i] = 1;

    // tell device we're completely ready.
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
}

// 找到一个空闲描述符, 标记为非空闲, 并返回其索引
static int alloc_desc()
{
    for (int i = 0; i < NUM; i++) {
        if (disk.free[i]) {
            disk.free[i] = 0;
            return i;
        }
    }
    return -1;
}

// 释放单个描述符
static void free_desc(int i)
{
    if (i >= NUM)
        panic("free_desc 1");
    if (disk.free[i])
        panic("free_desc 2");
    disk.desc[i].addr = 0;
    disk.desc[i].len = 0;
    disk.desc[i].flags = 0;
    disk.desc[i].next = 0;
    disk.free[i] = 1;
    // 唤醒等待空闲描述符的进程
    wakeup(&disk.free[0]);
}

// 释放描述符链
static void free_chain(int i)
{
    while (1) {
        int flag = disk.desc[i].flags;
        int nxt = disk.desc[i].next;
        free_desc(i);
        if (flag & VRING_DESC_F_NEXT)
            i = nxt;
        else
            break;
    }
}

// 分配三个描述符 (不需要连续)
// 硬盘传输总是使用三个描述符
static int alloc3_desc(int* idx)
{
    for (int i = 0; i < 3; i++) {
        idx[i] = alloc_desc(); // 分配描述符

        // 如果分配失败, 则释放已分配并错误返回
        if (idx[i] < 0) {
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
    // 计算块号blockno对应的扇区号
    uint64 sector = b->blockno * (BSIZE / 512);

    acquire(&disk.vdisk_lock); // 获取虚拟硬盘锁

    // 规范的第5.2.6.4节指出, 传统的块操作使用三个描述符:
    // 1. 用于指示 读/写 保留位 扇区号
    // 2. 用于指向 内存数据区域
    // 3. 用于指导 单字节的操作结果

    // 分配三个描述符
    int idx[3];
    while (1) {
        if (alloc3_desc(idx) == 0)
            break; // 成功分配
        // 休眠等待空闲描述符
        sleep(&disk.free[0], &disk.vdisk_lock);
    }

    // 配置三个描述符
    // QEMU的virtio-blk.c读取它们

    // 获取对应的硬盘请求头
    struct virtio_blk_req* buf0 = &disk.ops[idx[0]];

    // idx[0]-读写类型
    if (write)
        buf0->type = VIRTIO_BLK_T_OUT; // 写入硬盘
    else
        buf0->type = VIRTIO_BLK_T_IN; // 读取硬盘
    buf0->reserved = 0;               // idx[0]-保留位
    buf0->sector = sector;            // idx[0]-扇区号

    // idx[0]
    disk.desc[idx[0]].addr = (uint64)buf0;                 // 请求头的地址
    disk.desc[idx[0]].len = sizeof(struct virtio_blk_req); // 请求头的长度
    disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;           // 指示后面还有描述符
    disk.desc[idx[0]].next = idx[1];                       // 下一个描述符的索引

    // idx[1]
    disk.desc[idx[1]].addr = (uint64)b->data; // bio缓冲链块数据地址
    disk.desc[idx[1]].len = BSIZE;            // 长度为块大小 (buf.h)
    if (write)
        disk.desc[idx[1]].flags = 0; // 从buf读数据
    else
        disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // 将数据写入buf
    disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;     // 指示后面还有描述符
    disk.desc[idx[1]].next = idx[2];                  // 下一个描述符的索引

    // idx[2]
    disk.info[idx[0]].status = 0xff; // 如果成功, virtio_disk_intr()将清除此状态
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

    // 等待硬盘中断 virtio_disk_intr() 通知请求已完成
    while (b->disk == 1) {
        // 休眠等待硬盘中断完成
        sleep(b, &disk.vdisk_lock);
    }

    // 释放描述符
    disk.info[idx[0]].b = 0;
    free_chain(idx[0]);

    release(&disk.vdisk_lock);
}

// 处理硬盘中断
// trap.c->devintr 识别中断并调用此函数
void virtio_disk_intr()
{
    acquire(&disk.vdisk_lock); // 获取虚拟硬盘锁

    // 设备在被告知 此中断处理完成之前, 不会产生新中断
    // 这可能与设备写入新条目到 "used" 环发生竞争
    // 在这种情况下, 我们可能在此中断中处理新完成的条目
    // 而在下一此中断中没有任何事情要做, 这并无害
    *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

    __sync_synchronize(); // 内存屏障

    // 当设备添加条目到已用环时, 会递增disk.used->idx

    // 处理已完成的请求 [used_idx: disk.used->idx]
    while (disk.used_idx != disk.used->idx) {
        __sync_synchronize(); // 内存屏障

        // 获取描述符链的首索引
        int id = disk.used->ring[disk.used_idx % NUM].id;

        if (disk.info[id].status != 0)
            panic("virtio_disk_intr status");

        struct buf* b = disk.info[id].b;
        b->disk = 0; // 处理结束, 硬盘释放buf
        wakeup(b);   // 唤醒正在等待该buf的进程

        // 继续处理下一个请求
        disk.used_idx += 1;
    }

    release(&disk.vdisk_lock); // 释放虚拟硬盘锁
}
