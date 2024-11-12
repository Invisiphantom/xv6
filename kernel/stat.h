#include "types.h"

enum { I_FREE = 0, I_DIR, I_FILE, I_DEVICE };
struct stat {
    uint dev;    // 设备号
    uint inum;   // 索引编号
    short type;  // 索引类型
    short nlink; // 硬链接数
    uint64 size; // 文件大小 (字节)
};
