
#define ELF_MAGIC 0x464C457FU // 小端序"\x7FELF"

// ELF文件头
struct elfhdr {
    uint magic;    // ELF_MAGIC
    uchar elf[12]; // 版本信息

    ushort type;    // 文件类型, 例如: 0(无类型)、1(可重定位文件)、2(可执行文件)等
    ushort machine; // 机器架构类型, 例如: EM_X86_64(x86-64)、EM_RISCV(RISC-V)等
    uint version;   // 版本信息, 通常为 1(表示原始的 ELF 版本)

    uint64 entry; // 程序入口点地址, 程序开始执行的位置
    uint64 phoff; // 程序头部表的偏移量, 从文件开头到程序头部表的字节数
    uint64 shoff; // 节头部表的偏移量, 从文件开头到节头部表的字节数

    uint flags;       // 特殊标志, 提供与文件相关的特性信息
    ushort ehsize;    // ELF 文件头的大小, 单位为字节
    ushort phentsize; // 每个程序头表项的大小, 单位为字节
    ushort phnum;     // 程序头表的条目数
    ushort shentsize; // 每个节头表项的大小, 单位为字节
    ushort shnum;     // 节头表的条目数
    ushort shstrndx;  // 节名称字符串表的索引, 用于查找节名称
};

// ELF程序头
struct proghdr {
    uint32 type;  // 程序头类型, 指示该段的用途(如可执行段、共享段等)
    uint32 flags; // 段的标志, 指定段的访问权限(如可读、可写、可执行)

    uint64 off;   // 从文件开始到该段在文件中的偏移量(以字节为单位)
    uint64 vaddr; // 段在内存中的虚拟地址, 程序加载后该段应存放的位置
    uint64 paddr; // 段在物理内存中的地址(通常在用户空间中不使用)

    uint64 filesz; // 段在文件中的大小(以字节为单位), 表示实际包含的数据大小
    uint64 memsz;  // 段在内存中的大小(以字节为单位), 可能大于 filesz, 以便在内存中填充
    uint64 align;  // 段的对齐要求, 通常为 2 的幂, 指定该段在内存和文件中的对齐方式
};

// 程序头类型
#define ELF_PROG_LOAD 1 // 可加载程序段

// 程序头标志位
#define ELF_PROG_FLAG_EXEC 1  // 可执行
#define ELF_PROG_FLAG_WRITE 2 // 可写
#define ELF_PROG_FLAG_READ 4  // 可读
