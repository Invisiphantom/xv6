
// 文件系统实现:
//  + FS.img: 文件系统映像 (mkfs.c)
//  + Dev+blockno: 虚拟硬盘块设备 (virtio_disk.c)
//  + Bcache: 缓存链环 (bio.c)
//  + Log: 多步更新的崩溃恢复 (log.c)
//  + Inodes: inode分配器, 读取, 写入, 元数据 (fs.c)
//  + Directories: 具有特殊内容的inode(其他inode的列表) (fs.c)
//  + PathNames: 方便命名的路径, 如 /usr/rtm/xv6/fs.c (fs.c)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [          0 |           1 | 2       31 | 32        44 |           45 | 46     1999 ]

// https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
// https://github.com/qemu/qemu/blob/master/include/standard-headers/linux/virtio_mmio.h

// MMIO 控制寄存器 起始为0x10001000 (virtio_mmio.h) 
#define VIRTIO_MMIO_MAGIC_VALUE 0x000       // 魔数 ("virt"的小端序0x74726976)
#define VIRTIO_MMIO_VERSION 0x004           // 设备版本 (2)
#define VIRTIO_MMIO_DEVICE_ID 0x008         // 设备类型 (1:网卡 2:硬盘)
#define VIRTIO_MMIO_VENDOR_ID 0x00c         // 子系统供应者ID ("QEMU"的小端序0x554d4551)
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010   // 设备特性启用 [Sel*32:Sel*32+31]
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020   //
#define VIRTIO_MMIO_QUEUE_SEL 0x030         // select queue, write-only
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034     // max size of current queue, read-only
#define VIRTIO_MMIO_QUEUE_NUM 0x038         // size of current queue, write-only
#define VIRTIO_MMIO_QUEUE_READY 0x044       // ready bit
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050      // write-only
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060  // read-only
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064     // write-only
#define VIRTIO_MMIO_STATUS 0x070            // read/write
#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080    // physical address for descriptor table, write-only
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_DRIVER_DESC_LOW 0x090  // physical address for available ring, write-only
#define VIRTIO_MMIO_DRIVER_DESC_HIGH 0x094
#define VIRTIO_MMIO_DEVICE_DESC_LOW 0x0a0  // physical address for used ring, write-only
#define VIRTIO_MMIO_DEVICE_DESC_HIGH 0x0a4

// status register bits, from qemu virtio_config.h
#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
#define VIRTIO_CONFIG_S_DRIVER 2
#define VIRTIO_CONFIG_S_DRIVER_OK 4
#define VIRTIO_CONFIG_S_FEATURES_OK 8

// 5.2.3 Feature bits
// 设备特性比特位 (VIRTIO_MMIO_DEVICE_FEATURES)
#define VIRTIO_BLK_F_RO 5               // Disk is read-only
#define VIRTIO_BLK_F_SCSI 7             // Device supports scsi packet commands
#define VIRTIO_BLK_F_CONFIG_WCE 11      // Writeback mode available in config
#define VIRTIO_BLK_F_MQ 12              // Support more than one vq
#define VIRTIO_F_ANY_LAYOUT 27          // Device accepts arbitrary descriptor layouts
#define VIRTIO_RING_F_INDIRECT_DESC 28  // Support indirect buffer descriptors
#define VIRTIO_RING_F_EVENT_IDX 29

// this many virtio descriptors.
// must be a power of two.
#define NUM 8

// 描述符结构体 (规范)
struct virtq_desc {
    uint64 addr;   // 描述符指向的数据地址
    uint32 len;    // 描述符所指向数据的长度
    uint16 flags;  // 描述符的标志位
    uint16 next;   // 链接到下一个描述符的索引
};
#define VRING_DESC_F_NEXT 1   // 是否有下一个描述符
#define VRING_DESC_F_WRITE 2  // 是否要设备写addr[len]

// 可用环结构体 (规范)
struct virtq_avail {
    uint16 flags;      // 始终为零
    uint16 idx;        // 指示下一个可写入的描述符条目的索引
    uint16 ring[NUM];  // 描述符链头的描述符编号
    uint16 unused;     // 未使用的字段
};

// 已用环条目结构体 (规范)
// 设备用来记录已完成的请求
struct virtq_used_elem {
    uint32 id;  // 已完成请求的描述符链索引
    uint32 len;
};

// 已用环结构体 (规范)
struct virtq_used {
    uint16 flags;                     // 始终为零
    uint16 idx;                       // 设备添加 ring[] 条目时递增
    struct virtq_used_elem ring[NUM]; // 已使用条目, 包含状态信息
};

// these are specific to virtio block devices, e.g. disks,
// described in Section 5.2 of the spec.

#define VIRTIO_BLK_T_IN 0   // read the disk
#define VIRTIO_BLK_T_OUT 1  // write the disk

// 硬盘请求的第一个描述符格式
// 后跟两个描述符, 包含块数据和单字节状态
struct virtio_blk_req {
    uint32 type;      // 读写类型 (VIRTIO_BLK_T_IN | VIRTIO_BLK_T_OUT)
    uint32 reserved;  // 保留位
    uint64 sector;    // 扇区号
};
