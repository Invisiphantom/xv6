#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buf[512];

void cat(int fd) {
    int n;

    // 从文件描述符fd中读取数据, 并写入标准输出
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write(1, buf, n) != n) {
            fprintf(2, "cat: write error\n");
            exit(1);
        }
    }
    if (n < 0) {
        fprintf(2, "cat: read error\n");
        exit(1);
    }
}

int main(int argc, char* argv[]) {
    int fd, i;

    // 如果没有参数, 则打印标准输入
    if (argc <= 1) {
        cat(0);  // stdin
        exit(0);
    }

    // 逐个打开文件并打印
    for (i = 1; i < argc; i++) {
        if ((fd = open(argv[i], O_RDONLY)) < 0) {
            fprintf(2, "cat: cannot open %s\n", argv[i]);
            exit(1);
        }
        cat(fd);
        close(fd);
    }
    exit(0);
}
