
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char* argv[] = {"sh", 0};

// -exec file ./user/_init
// initcode->usys.S->exec.c 执行 exec(init, argv) 跳转到此处 (U-mode)
int main(void) {
    int pid, wpid;

    if (open("console", O_RDWR) < 0) {
        mknod("console", CONSOLE, 0);
        open("console", O_RDWR);
    }
    dup(0);  // stdout
    dup(0);  // stderr

    for (;;) {
        printf("init: starting sh\n");

        // 父进程: 返回子进程pid
        // 子进程: 返回0
        pid = fork();

        // 确保fork成功
        if (pid < 0) {
            printf("init: fork failed\n");
            exit(1);
        }

        // 子进程
        if (pid == 0) {
            exec("sh", argv);  // 加载sh程序
            printf("init: exec sh failed\n");
            exit(1);
        }

        for (;;) {
            // 等待子进程退出, 返回子进程的pid
            wpid = wait((int*)0);

            // 如果是sh进程退出, 则重新启动
            if (wpid == pid) {
                break;
            }
            // 报错
            else if (wpid < 0) {
                printf("init: wait returned an error\n");
                exit(1);
            }
            // 其他进程退出, 无需处理
            else {
            }
        }
    }
}
