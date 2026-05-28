# Tide Worker GDB 调试技巧

## 目标

- 面向线上 `tide_worker` coredump 和 attach 调试
- 解决 bundle lib 场景下 `gdb` 符号、共享库、源码栈不对齐的问题
- 给出一套稳定的最小命令集，便于快速定位 `6511/7511` worker 的崩溃原因

## 背景

`tide_worker` 不是普通的系统安装二进制，而是 bundle 方式运行：

- 主程序在工作目录下运行
- 动态链接器通常是相对路径 `./lib/ld-linux-x86-64.so.2`
- 共享库也通过工作目录下的 `./lib/` 解析

因此：

- 不能在任意目录直接跑 `gdb elf core`
- 必须切到目标进程自己的 `cwd`
- 否则很容易出现符号不全、so 对不上、源码栈漂移、`libthread_db` 不匹配

## 快速流程

```text
+---------------------------+
| [Step 1] 找目标 worker    |
| ss -ltnp | grep 6511/7511 |
+---------------------------+
              |
              v
+----------------------------------+
| [Step 2] 取 bundle 工作目录      |
| readlink -f /proc/$pid/cwd       |
+----------------------------------+
              |
              v
+----------------------------------+
| [Step 3] 配对 core / elf         |
| /opt/tiger/cores/core.$suffix    |
| /opt/tiger/cores/elf.$suffix     |
+----------------------------------+
              |
              v
+----------------------------------+
| [Step 4] 在 worker cwd 下跑 gdb  |
| cd /proc/$pid/cwd                |
| gdb elf core                     |
+----------------------------------+
              |
              v
+----------------------------------+
| [Step 5] 先看主线程源码栈        |
| bt -> frame -> args -> locals    |
+----------------------------------+
              |
              v
+----------------------------------+
| [Step 6] 回源码确认生命周期      |
| 对象持有者 / 释放点 / 并发边界   |
+----------------------------------+
```

## 找进程

优先从端口反查：

```bash
ss -ltnp | grep -E ':(6511|7511)\s'
```

典型输出：

```text
LISTEN ... *:7511 ... users:(("tide_worker",pid=1813395,fd=35))
LISTEN ... *:6511 ... users:(("tide_worker",pid=1831799,fd=33))
```

如果已经知道 pid，确认工作目录：

```bash
readlink -f /proc/$pid/cwd
```

## 核心原则

### 1. 必须在目标 worker 的 `cwd` 执行 gdb

```text
+------------------------------+
| bundle worker 运行时         |
| ./src/runtime/.../tide_worker|
| ./lib/ld-linux-x86-64.so.2   |
| ./lib/*.so                   |
+------------------------------+
              |
              v
+--------------------------------------+
| gdb 的工作目录必须与运行目录一致     |
| 否则 ./lib 相对路径解析会漂移        |
+--------------------------------------+
              |
      +-------+-------+
      |               |
      v               v
+----------------+  +---------------------------+
| 正确           |  | 错误                      |
| cd /proc/$pid/ |  | cd repo-root              |
| cwd && gdb ... |  | gdb /opt/.../elf core     |
+----------------+  +---------------------------+
```

正确姿势：

```bash
cd /proc/$target_pid/cwd
gdb /opt/tiger/cores/elf.$suffix /opt/tiger/cores/core.$suffix
```

或批处理：

```bash
cd /proc/$target_pid/cwd
gdb -q -batch \
  /opt/tiger/cores/elf.$suffix \
  /opt/tiger/cores/core.$suffix \
  -ex 'set pagination off' \
  -ex 'thread 1' \
  -ex 'bt 20'
```

错误姿势：

```bash
cd /data24/otf/stream_engine
gdb /opt/tiger/cores/elf.$suffix /opt/tiger/cores/core.$suffix
```

这类做法在 bundle 项目里非常容易让 `./lib` 解析失败。

### 2. `core.*` 和 `elf.*` 必须严格配对

```text
+------------------------------+
| /opt/tiger/cores/            |
+------------------------------+
      |                  |
      v                  v
+------------------+  +------------------+
| core.$suffix     |  | elf.$suffix      |
+------------------+  +------------------+
      |                  |
      +--------+---------+
               |
               v
+----------------------------------+
| 必须同 suffix 配对分析           |
| 不要混用不同时间点的 elf / core  |
+----------------------------------+
```

目录 `/opt/tiger/cores/` 下：

- `core.$suffix`
- `elf.$suffix`

要按同一后缀配对使用。不要混用不同时间点的 `elf/core`。

先看文件是否都已写完：

```bash
ls -l /opt/tiger/cores/core.$suffix /opt/tiger/cores/elf.$suffix
```

如果 core 还在持续增长，先不要分析。

## 推荐命令

### 主线程源码栈

```bash
cd /proc/$target_pid/cwd
gdb -q -batch \
  /opt/tiger/cores/elf.$suffix \
  /opt/tiger/cores/core.$suffix \
  -ex 'set pagination off' \
  -ex 'thread 1' \
  -ex 'bt 20'
```

### 看崩点局部变量

```bash
cd /proc/$target_pid/cwd
gdb -q -batch \
  /opt/tiger/cores/elf.$suffix \
  /opt/tiger/cores/core.$suffix \
  -ex 'set pagination off' \
  -ex 'thread 1' \
  -ex 'frame 4' \
  -ex 'info args' \
  -ex 'info locals'
```

实际调试时，`frame` 号按栈深度调整，不要死记为 `4`。

### 看全部线程

```bash
cd /proc/$target_pid/cwd
gdb -q -batch \
  /opt/tiger/cores/elf.$suffix \
  /opt/tiger/cores/core.$suffix \
  -ex 'set pagination off' \
  -ex 'thread apply all bt 12'
```

### 过滤噪音

core 线程很多时，`gdb` 会打印大量 `New LWP`。可以过滤掉：

```bash
cd /proc/$target_pid/cwd
gdb -q -batch \
  /opt/tiger/cores/elf.$suffix \
  /opt/tiger/cores/core.$suffix \
  -ex 'set pagination off' \
  -ex 'thread 1' \
  -ex 'bt 20' \
  -ex 'frame 4' \
  -ex 'info args' \
  -ex 'info locals' 2>&1 \
| grep -v '^\[New LWP' \
| grep -v '^warning:' \
| grep -v '^Use `info auto-load'
```

## 常见诊断信号

### 1. 栈顶在 `memmove` / `memcpy`

例如：

```text
#0  __memmove_avx_unaligned_erms
#1  arrow::BufferBuilder::UnsafeAppend
#2  arrow::BaseBinaryBuilder<...>::AppendValues
#3  plugin::Source::Flush
```

这类栈首先要怀疑：

- 上层传给 Arrow 的 `string_view` / 指针已经失效
- payload 生命周期早于 `Flush()` 完成
- 某个 batch 已经被 revoke / ack / reclaim，但上层还在读旧视图

```text
+-------------------------------+
| [Crash Signature]             |
| memmove / memcpy              |
+-------------------------------+
              |
              v
+-------------------------------+
| Arrow append 在拷贝用户数据   |
| BufferBuilder::UnsafeAppend   |
+-------------------------------+
              |
              v
+--------------------------------------+
| 上游传入的 data 指针很可能已失效     |
| string_view / payload pointer 悬挂   |
+--------------------------------------+
```

### 2. 栈顶在 `free` / `jemalloc` / `tcache`

优先怀疑：

- double free
- use-after-free
- 跨线程提前释放
- close/revoke/reset 和业务线程并发释放

### 3. 栈顶在锁等待或条件变量

优先区分：

- 业务真实死锁
- 正常 drain / revoke wait
- 上游线程已经退出，下游还在等信号

## 结合运行态定位

### 线程名分布

```bash
cd /proc/$pid/task
for tid in $(ls); do cat $tid/comm; done | sort | uniq -c | sort -rn
```

用于快速看：

- `rdk:*` 线程是否异常多
- 是否仍有大量 poll / broker 线程
- 是否是 Kafka 线程模型放大的问题

### 日志文件实际路径

```bash
ls -l /proc/$pid/fd | grep logs || true
```

或：

```bash
for fd in /proc/$pid/fd/*; do readlink $fd; done | grep tide_worker || true
```

线上很多 worker 会把日志重定向到容器目录，别想当然去仓库目录找。

## 针对 consumer_v2 的特别检查

如果崩溃发生在 Kafka source 路径，优先检查这些生命周期边界：

```text
+--------------------------------+
| [Object] PublishedMessage      |
| payload / receiveMs            |
+--------------------------------+
                |
                v
+--------------------------------+
| [Operation] populateDispatch   |
| RecordView.payloadView         |
+--------------------------------+
                |
                v
+--------------------------------+
| [Object] SourceAdapter         |
| readMsg -> msgData/msgLen      |
+--------------------------------+
                |
                v
+---------------------------------------------+
| [Object] plugin::Source::m_slaBuffer        |
| store std::string_view, not owning payload  |
+---------------------------------------------+
                |
                v
+--------------------------------+
| [Operation] plugin::Source::   |
| Flush -> Arrow AppendValues    |
+--------------------------------+
                |
                v
+--------------------------------+
| [Operation] freeMessage/ack    |
| late ack / slot reclaim        |
+--------------------------------+
```

关键问题通常是：

- `payloadView` 只是视图，不拥有内存
- 真正 payload 由 `MsgSlotRing` 背后的消息对象持有
- 如果 revoke/ack/close 过早 `markDoneAndReclaim()`，`Flush()` 里再拷贝就会崩

进一步判断时，直接按下面这张图想：

```text
+--------------------------------+
| [Owner] MsgSlotRing            |
| 持有真实 payload 内存          |
+--------------------------------+
                |
                v
+--------------------------------+
| [View] RecordView.payloadView  |
| 只借用，不拥有                 |
+--------------------------------+
                |
                v
+---------------------------------------------+
| [View] plugin::Source::m_slaBuffer          |
| 保存 string_view，继续借用同一块内存        |
+---------------------------------------------+
                |
                v
+--------------------------------+
| [Copy] Flush -> AppendValues   |
| 这里才真正 copy 到 Arrow       |
+--------------------------------+

危险窗口：

+--------------------------------+
| revoke / close / ack 提前发生  |
+--------------------------------+
                |
                v
+--------------------------------+
| MsgSlotRing::markDoneAndReclaim|
+--------------------------------+
                |
                v
+--------------------------------+
| payload 内存已释放或复用       |
+--------------------------------+
                |
                v
+--------------------------------+
| Flush 继续读 string_view       |
+--------------------------------+
                |
                v
+--------------------------------+
| memmove / AppendValues SIGSEGV |
+--------------------------------+
```

## 建议输出模板

调完一个 core，建议至少记录：

```text
1. core/elf 后缀
2. 对应 worker pid 和 cwd
3. 主线程 bt
4. 崩点 frame + args/locals
5. 同批 core 是否同签名
6. 对应源码释放路径
7. 怀疑的生命周期窗口
8. 可验证的回归测试
```

## 本次问题的经验

- `tide_worker` 必须在 `6511/7511` 对应 worker 的 `cwd` 下做 gdb
- 近期这批 core 的签名稳定落在：
  `plugin::Source::Flush -> arrow::BufferBuilder::UnsafeAppend -> memmove`
- 如果看到这条栈，优先检查 `consumer_v2` 的 `payloadView` 是否在 revoke/late-ack 后被提前回收

```text
+--------------------------------+
| [Observed Crash]               |
| plugin::Source::Flush          |
| -> Arrow AppendValues          |
| -> memmove                     |
+--------------------------------+
                |
                v
+--------------------------------+
| [Most Likely Cause]            |
| payloadView 生命周期短于 Flush |
+--------------------------------+
                |
                v
+----------------------------------------+
| [Fix Direction]                        |
| 不要在 revoke 路径提前 reclaim        |
| 改为 late ack / freeMessage 后回收    |
+----------------------------------------+
```
