c++ 程序，建议的多环境部署方案为：制定rpath 和定制loader。把lib 集中在一个目录。

因而gdb 时，为了自然而然地看到所有符号，应该在进程working dir 中执行gdb 命令。即 `cd /proc/$pid/cwd && gdb -p $pid`
