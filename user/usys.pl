#!/usr/bin/perl -w

# 生成 usys.S 系统调用函数入口 (kernel/syscall.c)

print "\n#include \"kernel/syscall.h\"\n";

sub entry {
    my $name = shift;
    print ".global $name\n";
    print "${name}:\n";
    print " li a7, SYS_${name}\n";
    print " ecall # (U-mode -> S-mode)\n";
    print " ret\n\n";
}
	
entry("fork");
entry("exit");
entry("wait");
entry("pipe");
entry("read");
entry("write");
entry("close");
entry("kill");

print "# initcode 执行 exec(init, argv) 跳转到此处 (U-mode)\n";
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
