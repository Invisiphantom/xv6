#include "types.h"

#define T_DIR 1     // 目录
#define T_FILE 2    // 文件
#define T_DEVICE 3  // 设备

struct stat {
    int dev;      // 所属硬盘设备
    uint ino;     // Inode编号
    short type;   // 文件类型
    short nlink;  // 硬链接数
    uint64 size;  // 文件大小（字节）
};
