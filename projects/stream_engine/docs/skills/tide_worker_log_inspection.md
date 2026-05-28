# Tide Worker 日志查看技巧

## 目标

- 快速定位运行中 `tide_worker` 的真实日志文件
- 区分 bundle 工作目录下的 `logs/`、重定向 stdout/stderr、以及 `/proc/$pid/fd` 实际打开文件
- 为线上排障提供一套稳定的最小命令集

## 核心流程

```text
+-----------------------------+
| [Step 1] 找目标进程         |
| ss / ps / pid-file          |
+-----------------------------+
              |
              v
+----------------------------------+
| [Step 2] 找进程工作目录          |
| readlink -f /proc/$pid/cwd       |
+----------------------------------+
              |
              v
+----------------------------------+
| [Step 3] 看启动命令              |
| /proc/$pid/cmdline               |
+----------------------------------+
              |
              v
+----------------------------------+
| [Step 4] 先看 cwd/logs           |
| /proc/$pid/cwd/logs/*.log        |
+----------------------------------+
              |
              v
+----------------------------------+
| [Step 5] 再看 fd 实际指向        |
| /proc/$pid/fd -> readlink        |
+----------------------------------+
              |
              v
+----------------------------------+
| [Step 6] 确认 stdout/stderr      |
| fd 1 / fd 2 / std_*.log          |
+----------------------------------+
```

## 先找进程

### 从端口找

```bash
ss -ltnp | grep -E ':(6511|7511)\s'
```

### 从进程名找

```bash
ps -eo pid,ppid,etime,cmd | grep tide_worker | grep -v grep
```

## 看工作目录

```bash
readlink -f /proc/$pid/cwd
```

典型意义：

```text
+----------------------------------------------+
| /proc/$pid/cwd                               |
+----------------------------------------------+
              |
              v
+----------------------------------------------+
| 实际 bundle 运行目录                         |
| 例如 tide_engine_1.1.0.xxxx                  |
+----------------------------------------------+
              |
              v
+----------------------------------------------+
| 该目录下通常有：                             |
| ./logs/                                      |
| ./config/                                    |
| ./lib/                                       |
| ./src/runtime/taskmanager/tide_worker        |
+----------------------------------------------+
```

## 看启动命令

```bash
tr '\0' ' ' < /proc/$pid/cmdline
```

重点确认：

- `--log-file=...`
- `--app-log-dir=...`
- `--conf-file=...`

## 最常用日志路径

默认最先检查：

```bash
tail -n 200 /proc/$pid/cwd/logs/tide_worker.log
```

如果是带角色的 worker，常见路径可能是：

```bash
tail -n 200 /proc/$pid/cwd/logs/tide_worker.${WORKER_ROLE}.log
```

从脚本看，常见启动方式包括：

```text
+----------------------------------------------+
| run_tide_worker.sh                           |
| --log-file=logs/tide_worker.log              |
+----------------------------------------------+

+------------------------------------------------------+
| run_tide_worker_daemon.sh                            |
| --app-log-dir=./${WORKER_ROLE}                       |
| --log-file=logs/tide_worker.${WORKER_ROLE}.log       |
+------------------------------------------------------+
```

## 不要只信 `logs/` 目录

真正稳妥的方法是看进程当前打开了什么文件：

```bash
ls -l /proc/$pid/fd | sed -n '1,120p'
```

或只筛日志：

```bash
for fd in /proc/$pid/fd/*; do
  readlink "$fd"
done | grep -E 'tide_worker|logs|std_' || true
```

对应思路：

```text
+-----------------------------+
| [cwd/logs] 只是默认猜测     |
+-----------------------------+
              |
              v
+-----------------------------+
| [fd] 才是进程真实打开文件   |
+-----------------------------+
              |
              v
+----------------------------------+
| 如果 cwd/logs 没内容或不对       |
| 以 /proc/$pid/fd 为准            |
+----------------------------------+
```

## 看 stdout / stderr

很多启动脚本会把 stdout/stderr 重定向到 `std_*.log`：

```bash
ls -lt /proc/$pid/cwd/logs/std_*.log 2>/dev/null | head
tail -n 200 /proc/$pid/cwd/logs/std_*.log
```

也可以直接看 fd 1 和 fd 2：

```bash
readlink -f /proc/$pid/fd/1
readlink -f /proc/$pid/fd/2
```

## 排障推荐顺序

```text
+-----------------------------+
| [1] 先拿 pid                |
+-----------------------------+
              |
              v
+-----------------------------+
| [2] 看 /proc/$pid/cwd       |
+-----------------------------+
              |
              v
+-----------------------------+
| [3] 看 /proc/$pid/cmdline   |
+-----------------------------+
              |
              v
+--------------------------------------+
| [4] tail cwd/logs/tide_worker.log    |
+--------------------------------------+
              |
              v
+--------------------------------------+
| [5] 查 /proc/$pid/fd 实际日志文件    |
+--------------------------------------+
              |
              v
+--------------------------------------+
| [6] 补看 stdout/stderr 与 std_*.log  |
+--------------------------------------+
```

## 针对 consumer_v2 的日志关键字

如果目的是排 consumer_v2，优先 grep：

```bash
grep -a -nE 'consumer_v2|unix socket|discovery|bind|listen|registerWorker|runtime' \
  /proc/$pid/cwd/logs/tide_worker.log | tail -n 200
```

常见关键字：

- `consumer_v2 unix socket already active, skip bind`
- `consumer_v2 bind unix socket failed`
- `consumer_v2 listen unix socket failed`
- `consumer_v2 discovery write failed`
- `consumer_v2 runtime already start requested`
- `consumer_v2 source adapter created`

## 本次问题的经验

```text
+----------------------------------+
| [Observed] 6511 所在 worker      |
| 日志里没有 bind failed           |
+----------------------------------+
              |
              v
+----------------------------------+
| [Actual] 出现 skip bind          |
| consumer_v2 unix socket already  |
| active                           |
+----------------------------------+
              |
              v
+----------------------------------+
| [Meaning] 不是目录/权限失败      |
| 是启动阶段判断已有 active peer   |
+----------------------------------+
              |
              v
+----------------------------------+
| [Action] 继续查 env + socketPath |
| + discovery + /proc/$pid/fd      |
+----------------------------------+
```
