
# xv6环境配置

https://github.com/mit-pdos/xv6-riscv  
https://pdos.csail.mit.edu/6.S081/2024/xv6/book-riscv-rev4.pdf
https://www.cnblogs.com/KatyuMarisaBlog/p/14366115.html

```bash
git clone https://github.com/Invisiphantom/xv6.git
sudo apt install -y binutils-riscv64-linux-gnu gcc-riscv64-linux-gnu qemu-system-riscv64 gdb-multiarch bear
make qemu

echo "add-auto-load-safe-path /home/ethan/xv6/.gdbinit" > ~/.gdbinit
make qemu-gdb
gdb-multiarch kernel/kernel

make clean
bear -- make qemu
mv compile_commands.json .vscode/

-exec file ./user/_init # 调试用户程序
```

.vscode/tasks.json  
```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "xv6build",
            "type": "shell",
            "command": "make qemu-gdb",
            "isBackground": true,
            "problemMatcher": [
                {
                    "pattern": [
                        {
                            "regexp": ".",
                            "file": 1,
                            "location": 1,
                            "message": 1
                        }
                    ],
                    "background": {
                        // 通过background的强制结束
                        // 来提醒launch.json启动gdb
                        "beginsPattern": ".",
                        "endsPattern": "Now run 'gdb' in another window."
                    }
                }
            ]
        }
    ]
}
```

.vscode/launch.json  
```json
{
    "configurations": [
        {
            "preLaunchTask": "xv6build",
            "MIMode": "gdb",
            "name": "xv6debug",
            "type": "cppdbg",
            "request": "launch",
            "stopAtEntry": true,
            "cwd": "${workspaceFolder}",
            "program": "${workspaceFolder}/kernel/kernel",
            "miDebuggerPath": "/usr/bin/gdb-multiarch",
            "miDebuggerServerAddress": "127.0.0.1:26002",
        }
    ]
}
```

.vscode/c_cpp_properties.json  
```json
{
    "configurations": [
        {
            "name": "Linux",
            "intelliSenseMode": "linux-gcc-x64",
            "compilerPath": "/usr/bin/riscv64-linux-gnu-gcc",
            "compileCommands": "${workspaceFolder}/.vscode/compile_commands.json"
        }
    ],
    "version": 4
}
```


# xv6系统调用


|     | System Call                                      | Desc                                                       |
| --- | ------------------------------------------------ | ---------------------------------------------------------- |
| 1   | `int fork()`                                     | 复制新进程, 父进程返回子进程的pid, 子进程返回0             |
| 2   | `int exit(int status)`                           | 终止当前进程, 将status传递给wait()等待进程                 |
| 3   | `int wait(int* status)`                          | 等待子进程退出, 返回子进程的pid, 用status存储其退出状态    |
| 4   | `int pipe(int p[])`                              | 创建管道, 读:p[0], 写:p[1], 所有写端关闭后, 读端返回EOF    |
| 5   | `int read(int fd, void* buf, int n)`             | 从文件描述符fd中读取n字节到buf, 返回读取的字节数, EOF返回0 |
| 6   | `int kill(int pid)`                              | 终止进程pid                                                |
| 7   | `int exec(char* file, char* argv[])`             | 加载可执行文件file, 传递参数argv, 只在出错时返回           |
| 8   | `int fstat(int fd, struct stat* st)`             | 将fd的状态存储到st                                         |
| 9   | `int chdir(const char* dir)`                     | 改变当前工作目录                                           |
| 10  | `int dup(int fd)`                                | 复制文件描述符fd, 但共享offset, 返回新的文件描述符         |
| 11  | `int getpid()`                                   | 返回当前进程的pid                                          |
| 12  | `char* sbrk(int n)`                              | 扩展进程内存n字节, 返回新内存的起始地址                    |
| 13  | `int sleep(int n)`                               | 休眠n秒CPU时钟                                             |
| 14  | `int uptime()`                                   | 返回系统启动以来的时间                                     |
| 15  | `int open(const char* file, int flags)`          | 打开文件, flags表示读/写, 返回文件描述符                   |
| 16  | `int write(int fd, const char* buf, int n)`      | 将buf中的n字节写入文件描述符fd, 返回写入的字节数           |
| 17  | `int mknod(const char* file, int mode, int dev)` | 创建设备文件                                               |
| 18  | `int unlink(const char* file)`                   | 删除文件链接                                               |
| 19  | `int link(const char* file1, const char* file2)` | 给file1创建新的硬链接file2                                 |
| 20  | `int mkdir(const char* dir)`                     | 创建新目录                                                 |
| 21  | `int close(int fd)`                              | 关闭文件描述符fd                                           |





# xv6虚拟地址空间

```cpp
// 内核虚拟内存布局 (vm.c->kvmmake)
// >低地址
//      UART
//      VirtIO
//      PLIC
//      内核代码段
//      内核其余物理内存
//      ...
//      proc[NPROC-1]内核栈
//      guard page
//      proc[NPROC-2]内核栈
//      guard page
//      ...
//      proc[0]内核栈
//      guard page
//      TRAMPOLINE
// >高地址
// 依次映射每个进程的内核栈到内核空间
#define KSTACK(p) (TRAMPOLINE - ((p) + 1) * 2 * PGSIZE)

// 用户虚拟内存布局 (proc.c->proc_pagetable)
// >低地址
//   代码段
//   数据段
//   用户栈空间
//   用户堆空间
//   ...
//   TRAPFRAME (p->trapframe)
//   TRAMPOLINE (内核代码段trampoline.S)
// >高地址
#define TRAMPOLINE (MAXVA - PGSIZE)      // trampoline页映射到最高虚拟地址, 用于用户和内核空间
#define TRAPFRAME (TRAMPOLINE - PGSIZE)  // trapframe页映射到trampoline页的相邻低地址
```


# xv6文件系统

```cpp
// 文件系统实现:
//  + UART: 串口输入输出 (printf.c, console.c, uart.c)
//  + FS.img: 文件系统映像 (mkfs.c)
//  + Dev+blockno: 虚拟硬盘块设备 (virtio_disk.c)
//  + Bcache: 缓存链环 (bio.c, buf.h)
//  + Log: 多步更新的崩溃恢复 (log.c)
//  + Inode: inode分配器, 读取, 写入, 元数据 (fs.c)
//  + Directory: 具有特殊内容的inode(其他inode的列表) (fs.c)
//  + Path: 方便命名的路径, 如 /usr/rtm/xv6/fs.c (fs.c)
//  + File SysCall: 文件系统调用 (sysfile.c, pipe.c, file.c, file.h)

// 硬盘布局
// [ boot block | super block | log blocks | inode blocks | free bit map | data blocks ]
// [      0     |      1      | 2       31 | 32        44 |      45      | 46     1999 ]
```