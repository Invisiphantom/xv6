
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
                        "beginsPattern": ".", // 通过background的强制结束
                        "endsPattern": "." // 来提醒launch.json启动gdb
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

