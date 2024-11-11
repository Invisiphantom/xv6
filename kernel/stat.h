#include "types.h"

#define T_FREE 0   // 空闲
#define T_DIR 1    // 目录
#define T_FILE 2   // 文件
#define T_DEVICE 3 // 设备

struct stat {
    uint dev;    // 设备号
    uint ino;    // 索引编号
    short type;  // 文件类型
    short nlink; // 硬链接数
    uint64 size; // 文件大小 (字节)
};
