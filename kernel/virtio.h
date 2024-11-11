
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

// https://blog.csdn.net/qq_45226456/article/details/133583975
// https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
// https://github.com/qemu/qemu/blob/master/include/standard-headers/linux/virtio_mmio.h

// MMIO 控制寄存器 起始为0x10001000 (virtio_mmio.h)
#define VIRTIO_MMIO_MAGIC_VALUE 0x000 // 魔数 ("virt"的小端序0x74726976)
#define VIRTIO_MMIO_VERSION 0x004     // 设备版本 (2)
#define VIRTIO_MMIO_DEVICE_ID 0x008   // 设备类型 (1:网卡 2:硬盘)
#define VIRTIO_MMIO_VENDOR_ID 0x00c   // 子系统ID ("QEMU"小端序0x554d4551)

#define VIRTIO_MMIO_DEVICE_FEATURES 0x010 // 查看设备支持的特性 (读)
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020 // 设置驱动使用的特性 (写)

#define VIRTIO_MMIO_QUEUE_SEL 0x030     // 选择的队列编号 (写)
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034 // 查看队列最大容量 (读)
#define VIRTIO_MMIO_QUEUE_NUM 0x038     // 设置当前队列容量 (写)
#define VIRTIO_MMIO_QUEUE_READY 0x044   // 队列是否准备好 (读写)
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050  // 通知设备队列有buffer可用 (写)

#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060 // 设备中断状态 (读)
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064    // 确认中断 (写)

#define VIRTIO_MMIO_STATUS 0x070 // 设备状态 (读写)

#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080   // 描述符表的物理低地址 (写)
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084  // 描述符表的物理高地址 (写)
#define VIRTIO_MMIO_DRIVER_DESC_LOW 0x090  // 待处理环的物理低地址 (写)
#define VIRTIO_MMIO_DRIVER_DESC_HIGH 0x094 // 待处理环的物理高地址 (写)
#define VIRTIO_MMIO_DEVICE_DESC_LOW 0x0a0  // 已处理环的物理低地址 (写)
#define VIRTIO_MMIO_DEVICE_DESC_HIGH 0x0a4 // 已处理环的物理高地址 (写)

// 状态寄存器位 (qemu->virtio_config.h)
#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1 // OS已经发现有效虚拟设备
#define VIRTIO_CONFIG_S_DRIVER 2      // OS已经知道如何驱动设备
#define VIRTIO_CONFIG_S_FEATURES_OK 8 // 设备特征/具体工作方式协商完毕
#define VIRTIO_CONFIG_S_DRIVER_OK 4   // 驱动已经准备好

// 设备特性比特位 (5.2.3 Feature bits)
#define VIRTIO_BLK_F_RO 5              // 块设备只读
#define VIRTIO_BLK_F_SCSI 7            // 虚拟块设备支持SCSI包命令
#define VIRTIO_BLK_F_CONFIG_WCE 11     // Writeback mode available in config
#define VIRTIO_BLK_F_MQ 12             // Support more than one vq
#define VIRTIO_F_ANY_LAYOUT 27         // Device accepts arbitrary descriptor layouts
#define VIRTIO_RING_F_INDIRECT_DESC 28 // Support indirect buffer descriptors
#define VIRTIO_RING_F_EVENT_IDX 29

// 描述符数量
#define NUM 8

// 描述符结构体 (规范)
struct virtq_desc {
    uint64 addr;  // 描述符指向的数据地址
    uint32 len;   // 描述符所指向数据的长度
    uint16 flags; // 描述符的标志位
    uint16 next;  // 链接到下一个描述符的索引
};
#define VRING_DESC_F_NEXT 1  // 是否有下一个描述符
#define VRING_DESC_F_WRITE 2 // 是否要设备写addr[len]

// 待处理环结构体 (规范)
struct virtq_avail {
    uint16 flags;     // 始终为零
    uint16 idx;       // 指示下一个可写入的描述符条目的索引
    uint16 ring[NUM]; // 描述符链头的描述符编号
    uint16 unused;    // 未使用的字段
};

// 已处理环条目结构体 (规范)
// 设备用来记录已完成的请求
struct virtq_used_elem {
    uint32 id; // 已完成请求的描述符链索引
    uint32 len;
};

// 已处理环结构体 (规范)
struct virtq_used {
    uint16 flags;                     // 始终为零
    uint16 idx;                       // 设备添加 ring[] 条目时递增
    struct virtq_used_elem ring[NUM]; // 已使用条目, 包含状态信息
};

#define VIRTIO_BLK_T_IN 0  // 读块设备
#define VIRTIO_BLK_T_OUT 1 // 写块设备

// 硬盘请求的第一个描述符格式
// 后跟两个描述符, 包含块数据和单字节状态
struct virtio_blk_req {
    uint32 type;     // 读写类型 (VIRTIO_BLK_T_IN | VIRTIO_BLK_T_OUT)
    uint32 reserved; // 保留位
    uint64 sector;   // 块区号
};
