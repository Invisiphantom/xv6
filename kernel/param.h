

#define NPROC 64                   // 进程的最大数量
#define NCPU 8                     // CPU的最大数量
#define NOFILE 16                  // 每个进程的最大打开文件数
#define NFILE 100                  // open files per system
#define NINODE 50                  // 内存-索引数
#define NDEV 10                    // 最大主设备号
#define ROOTDEV 1                  // 根目录设备号
#define MAXARG 32                  // max exec arguments
#define MAXOPBLOCKS 10             // 文件系统单次可写入的最大块数
#define LOGSIZE (MAXOPBLOCKS * 3)  // 硬盘中的最大日志块数
#define NBUF (MAXOPBLOCKS * 3)     // size of disk block cache
#define FSSIZE 2000                // size of file system in blocks
#define MAXPATH 128                // maximum file path name
#define USERSTACK 1                // user stack pages
