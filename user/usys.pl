#!/usr/bin/perl -w

# 生成 usys.S 系统调用函数入口 (kernel/syscall.c)

print "\n#include \"kernel/syscall.h\"\n\n\n";

sub entry {
    my $name = shift;
    print ".global $name\n";
    print "${name}:\n";
    print "\tli a7, SYS_${name}\n";
    print "\tecall # (U-mode -> S-mode)\n";
    print "\tret\n\n";
}
	
entry("fork");
entry("exit");
entry("wait");
entry("pipe");
entry("read");
entry("write");
entry("close");
entry("kill");
entry("exec");
entry("open");
entry("mknod");
entry("unlink");
entry("fstat");
entry("link");
entry("mkdir");
entry("chdir");
entry("dup");
entry("getpid");
entry("sbrk");
entry("sleep");
entry("uptime");
