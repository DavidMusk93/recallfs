# Consumer V2 6511 Perf Bottleneck Analysis

## 目标

- 调试 `6511` 所在 `tide_worker` 进程。
- 用 `perf`、`pidstat`、`/proc`、consumer_v2 UDS metrics 判断当前卡点。
- 明确当前是 Kafka 生产/poll 慢，还是 worker 消费/下游处理慢。
- 从 CPU、内存、IO、线程状态角度给出证据和变更建议。

## 对象

```text
+--------------------------------------+
| [listen port] 6511                   |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [pid] 2587406 tide_worker            |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [cwd] /proc/2587406/cwd              |
| tide_engine_1.1.0.6214               |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [uds] /var/run/tide/worker_6510.sock |
+--------------------------------------+
```

## 工具

```text
+------------------+---------------------------------------------+
| perf             | CPU event, stack, symbol attribution         |
| pidstat          | process CPU, memory fault, write throughput  |
| /proc/task       | thread name, state, CPU tick delta           |
| /proc/io         | process read/write bytes                     |
| /sys/class/net   | eth0 rx/tx bytes                             |
| consumer_v2 UDS  | /json runtime metrics                        |
+------------------+---------------------------------------------+
```

`bpftrace` 后续已就绪；新增 eBPF stack/comm/syscall 采样用于交叉验证 `perf` 结论，并作为线上低侵入学习路径。

## 采样命令

```bash
ss -ltnp 'sport = :6511'
pidstat -t -p 2587406 -u -r -d -w 1 10
perf stat -p 2587406 -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions,cache-references,cache-misses -- sleep 10
cd /proc/2587406/cwd
perf record -F 99 -g -p 2587406 -o /tmp/tide-worker-2587406.perf.data -- sleep 20
perf report -i /tmp/tide-worker-2587406.perf.data --stdio --no-children --sort dso,symbol
```

## Consumer V2 证据

10s 采样窗口：

```text
+--------------------------------------+----------------+
| [metric]                             | [value]        |
+--------------------------------------+----------------+
| eth0 rx                              | 125.94 MiB/s   |
| process write                        | 88.71 MiB/s    |
| consumer_v2 totalPolledBytes delta   | 493.43 MiB/s   |
| consumer_v2 currentThroughputBytes/s | ~497 MiB/s     |
| acked records                        | ~67.5k rec/s   |
| read records                         | ~67.5k rec/s   |
+--------------------------------------+----------------+
```

当前 consumer 状态：

```text
+-----------------------------+------------------+
| [runtime]                   | [value]          |
+-----------------------------+------------------+
| sharedConsumerCount         | 1                |
| workerCount                 | 12               |
| handleCount / desired       | 2 / 2            |
| minHandleCount / max        | 1 / 2            |
| assignedPartitionCount      | 124              |
| pausedPartitionCount        | 66               |
| readyPartitionCount         | 107              |
| workerQueueDepth            | 1                |
| ringCapacity                | 2097152          |
| ringLiveCount               | 2097152          |
| ringFreeSlotCount           | 0                |
| totalBufferedRecordCount    | 2089435          |
| autoScaleBacklog            | 2089447          |
| totalScaleOut/In            | 1 / 0            |
| totalRebalanceCallbacks     | 8                |
| totalConsumeErrors          | 1                |
+-----------------------------+------------------+
```

结论：

```text
+--------------------------+
| [Kafka poll/source side] |
+--------------------------+
| 493 MiB/s payload poll   |
| 67.5k rec/s ack/read     |
+--------------------------+
             |
             v
+--------------------------+
| [ring/backpressure]      |
+--------------------------+
| ring free slot = 0       |
| 66 / 124 partitions paused |
+--------------------------+
             |
             v
+--------------------------+
| [downstream side]        |
+--------------------------+
| drain cannot clear ring  |
| process write ~89 MiB/s  |
+--------------------------+
```

生产/poll 不慢；当前瓶颈在 consumer_v2 之后的处理/写出链路，consumer_v2 已经进入持续满环和 partition pause。

## CPU 证据

`perf stat` 10s：

```text
+--------------------+------------------+
| [counter]          | [value]          |
+--------------------+------------------+
| task-clock         | 40307.80 ms      |
| CPU utilized       | 4.015 CPUs       |
| context switches   | 140826           |
| page faults        | 2323159          |
| cycles             | 118.75B          |
| instructions       | 230.45B          |
| IPC                | 1.94             |
| cache misses       | 475.44M          |
| cache miss ratio   | 30.87%           |
+--------------------+------------------+
```

`pidstat` 5s：

```text
+--------------------+------------------+
| [counter]          | [value]          |
+--------------------+------------------+
| average CPU        | 654%             |
| average user CPU   | 564.8%           |
| average system CPU | 89.2%            |
| minor faults       | 314246 /s        |
| major faults       | 3.8 /s           |
| process write      | 332524.6 kB/s    |
| iodelay            | 0                |
+--------------------+------------------+
```

`perf report` top symbols:

```text
+-----------------------------+----------+--------------------------------------+
| [symbol]                    | overhead | [meaning]                            |
+-----------------------------+----------+--------------------------------------+
| ZSTD_decompressSequences    | 8.97%    | Kafka compressed payload decompress   |
| __memmove_avx_unaligned     | 6.64%    | payload/source/Arrow copy             |
| JSONStructuredDecoder       | 6.39%    | downstream JSON decode                |
| simdjson stage2_next        | 4.15%    | JSON parser stage2                    |
| rd_kafka_broker_ops_io_serve| 1.20%    | librdkafka broker IO                  |
+-----------------------------+----------+--------------------------------------+
```

关键栈：

```text
+--------------------------+
| [consumer poll thread]   |
+--------------------------+
| SharedConsumerState::pollLoop |
| -> handleConsumedMessages     |
| -> __memmove_avx_unaligned    |
+--------------------------+

+--------------------------+
| [slot worker]            |
+--------------------------+
| CSlot::Run               |
| -> DecoderMap::Run       |
| -> JSONStructuredDecoder |
| -> simdjson              |
+--------------------------+

+--------------------------+
| [rdk broker thread]      |
+--------------------------+
| rd_kafka_broker_serve    |
| -> ZSTD_decompress       |
+--------------------------+
```

CPU 结论：

- 不是大量线程睡死或等锁导致吞吐低。
- 热点主要是 payload 解压、payload copy、JSON 解析和 writer/下游执行。
- cache miss ratio 高，且 minor fault 很高，说明大 ring + 大 payload copy + Arrow/JSON 构建有明显内存带宽/缓存压力。

## 线程状态证据

`/proc/2587406/task` 10s tick delta top：

```text
+------------------+----------------------+-------------------------------+
| [thread]         | [evidence]           | [meaning]                     |
+------------------+----------------------+-------------------------------+
| kcv2p...-0/1     | ~70 ticks each       | consumer_v2 poll still active |
| rdk:broker*      | high ticks           | Kafka broker/decompress active|
| slot-*           | high ticks           | downstream task CPU active    |
| writer/ex-*      | high ticks           | writer executor active        |
+------------------+----------------------+-------------------------------+
```

线程状态聚合：

```text
+---------+-------+
| state   | count |
+---------+-------+
| S       | 1476  |
| R       | 7     |
| D       | 1     |
+---------+-------+
```

线程结论：

- 大部分线程处于 sleeping 是正常的 worker/rdk 线程池状态。
- 少量 runnable 线程承担实际 CPU 热点。
- 只有 1 个 D 状态线程，且 `pidstat iodelay=0`，不支持“磁盘 IO wait 卡死”为主因。

## IO 与内存证据

`/proc/io` 10s：

```text
+----------------+--------------+
| process read   | 0 MiB/s      |
| process write  | 88.71 MiB/s  |
+----------------+--------------+
```

`pidstat` 窗口里 process write 有明显波动，平均可到 `~324 MiB/s`。

`iostat`：

- 系统 iowait 约 `0.09% - 0.17%`。
- NVMe 写 util 在采样窗口内最高约 `34.8%`。
- 没有看到磁盘打满或高 iowait。

IO 结论：

- 当前有写出压力，但磁盘不是全局硬瓶颈。
- 更可能是 writer/executor 处理、编码、flush 策略或下游 sink 协议侧限制，导致 drain 速度低于 source poll。

## 根因判断

```text
+--------------------------+
| [not bottleneck]         |
+--------------------------+
| Kafka source production  |
| consumer_v2 poll handle  |
| host disk iowait         |
+--------------------------+
             |
             v
+--------------------------+
| [active bottleneck]      |
+--------------------------+
| downstream decode/write  |
| memory copy/cache miss   |
| ring full backpressure   |
+--------------------------+
```

当前是消费/下游慢，不是生产/poll 慢。

更准确地说：

1. Kafka/librdkafka 能把压缩网络流量解压成约 `493 MiB/s payload`。
2. consumer_v2 poll 线程仍在工作，并把 ring 填满。
3. 下游 `slot-*` 在 JSON decode，`writer/ex-*` 在写出，进程写 IO 波动明显。
4. ring 无空位导致 partition pause，source 只能在背压下维持满水位。

## 当前配置评估

```text
+--------------------------------------+----------------------------+
| [config]                             | [assessment]               |
+--------------------------------------+----------------------------+
| ring_capacity=2097152                | 过大且已满，隐藏下游慢     |
| high_watermark=32768                 | 单 partition 积压上限较高  |
| low_watermark=8192                   | 滞回很宽，pause 时间会较长 |
| auto_scale_max_poll_thread_count=2   | 已足够，不应继续增加       |
+--------------------------------------+----------------------------+
```

建议：

- 不要继续提高 `auto_scale_max_poll_thread_count`，否则只会更快填满 ring，并增加 rdk broker 线程和解压 CPU。
- 若要稳定延迟和内存，先把 `ring_capacity` 降到 `524288` 或 `1048576` 做 A/B。
- `high_watermark/low_watermark` 可以先保守下调到 `16384/4096` 或 `8192/2048`，观察吞吐是否下降；如果吞吐不降，说明原配置只是堆积更多内存。
- 如果吞吐目标更高，优先增加/优化下游并行度、JSON decode、writer batch/flush，而不是加 Kafka handle。

## 变更建议

### 观测增强

1. HTML/JSON 增加明确命名：
   - `payloadThroughputBytesPerSec`
   - `payloadThroughputMiBPerSec`
   - `network traffic is not payload traffic` 说明
2. 增加 rolling window 指标，而不是只展示生命周期平均：
   - `pollPayloadBytesPerSec_10s`
   - `readRecordsPerSec_10s`
   - `ackRecordsPerSec_10s`
3. 增加 backpressure 状态：
   - `ringUsagePercent`
   - `isRingFull`
   - `pausedPartitionRatio`
   - `sourceBlockedByRingEvents`
4. 增加阶段化吞吐：
   - poll -> dispatch -> read -> ack
   - 用来一眼判断生产慢还是消费慢

### consumer_v2 侧

1. `ring_capacity` 不应作为吞吐调优第一旋钮，应作为内存/延迟保护阈值。
2. auto-scale handle 不应在 ring 满时继续 scale-out。
3. 当 `ringFreeSlotCount=0` 持续时，应在 HTML 上标红并提示“下游慢”。

### 下游侧

1. 优先 profile `JSONStructuredDecoder`：
   - 减少字段解析数。
   - 避免重复 string copy。
   - 尽量批量解析/列式写入。
2. profile writer：
   - writer batch size。
   - flush 周期。
   - 文件/块大小。
   - 是否存在小写放大。
3. 如果目标是 10x：
   - source 已能提供约 `493 MiB/s payload`。
   - 需要扩下游 slot/operator/writer 能力，否则 consumer_v2 调参不会带来 10x。

## 下一步计划

```text
+-----------------------------+
| [phase 1] metrics fix       |
+-----------------------------+
| rolling throughput          |
| ring usage/backpressure     |
| stage-by-stage rate         |
+-----------------------------+
              |
              v
+-----------------------------+
| [phase 2] downstream perf   |
+-----------------------------+
| perf slot JSON decoder      |
| perf writer/ex              |
| inspect batch/flush config  |
+-----------------------------+
              |
              v
+-----------------------------+
| [phase 3] config A/B        |
+-----------------------------+
| ring 2097152 -> 1048576     |
| high/low 32768/8192 -> lower|
| keep handle max=2           |
+-----------------------------+
```

## Workers 60 后复测

用户将 consumer_v2 配置改为：

```text
+------------------------------------+---------+
| [config]                           | [value] |
+------------------------------------+---------+
| workerCount                        | 60      |
| ringCapacity                       | 524288  |
| highWatermark / lowWatermark       | 8192/2048 |
| autoScaleMaxPollThreadCount        | 2       |
+------------------------------------+---------+
```

### Metrics 证据

10.777s 采样窗口：

```text
+------------------------------+----------------+
| [metric]                     | [value]        |
+------------------------------+----------------+
| eth0 rx                      | 805.31 MiB/s   |
| consumer_v2 polled payload   | 2529.52 MiB/s  |
| polled records               | 345713.81 /s   |
| read records                 | 344882.40 /s   |
| acked records                | 344977.42 /s   |
| dispatched records           | 344407.30 /s   |
| process write                | 66.53 MiB/s    |
| RSS delta                    | +1.49 GiB      |
+------------------------------+----------------+
```

采样结束时 runtime 状态：

```text
+--------------------------+------------------+
| [runtime]                | [value]          |
+--------------------------+------------------+
| ringLive / ringCapacity  | 125672 / 524288  |
| ringFreeSlotCount        | 398616           |
| totalBufferedRecordCount | 102656           |
| pausedPartitionCount     | 16               |
| assignedPartitionCount   | 125              |
| workerQueueDepth         | 11               |
| activeLeaseCount         | 8                |
+--------------------------+------------------+
```

结论：

- `ringFreeSlotCount` 从 `0` 变为 `398616`，说明上次的 consumer_v2 ring 背压已缓解。
- `pausedPartitionCount` 从约一半 partition pause 降到 `16/125`，source 侧不再是主卡点。
- `read/ack` 与 `polled` 基本同阶，consumer_v2 到 worker 的队列不再明显积压。
- workers 从 `48 -> 60` 收益不明显，说明瓶颈已经不在 worker 数量本身，而在每条记录的 CPU/内存/写出成本。

### Perf 证据

`perf stat` 10s：

```text
+--------------------+------------------+
| [counter]          | [value]          |
+--------------------+------------------+
| task-clock         | 197641.82 ms     |
| CPU utilized       | 19.667 CPUs      |
| context switches   | 1,635,718        |
| cpu migrations     | 198,256          |
| page faults        | 6,945,887        |
| instructions       | 841.92B          |
| IPC                | 1.48             |
| cache miss ratio   | 38.18%           |
+--------------------+------------------+
```

`pidstat` 10s：

```text
+--------------------+------------------+
| [counter]          | [value]          |
+--------------------+------------------+
| average CPU        | 1937.36%         |
| average user CPU   | 1600.50%         |
| average system CPU | 336.86%          |
| minflt/s           | 935070.73        |
| majflt/s           | 2.50             |
| process write      | 77641.51 kB/s    |
| iodelay            | 0                |
+--------------------+------------------+
```

`perf report` top symbols:

```text
+-----------------------------+----------+--------------------------------------+
| [symbol]                    | overhead | [meaning]                            |
+-----------------------------+----------+--------------------------------------+
| JSONStructuredDecoder       | 5.16%    | JSON object parse                     |
| LZ4_compress_fast_extState  | 3.62%    | sink/output compression               |
| StringWriter::Write         | 2.34%    | JSON field to output row/string write |
| BaseBinaryBuilder::Append   | 1.30%    | Arrow/string append and allocation    |
| handleConsumedMessages      | 1.20%    | consumer_v2 payload copy/dispatch     |
| __handle_mm_fault           | 1.37%    | memory allocation/page fault cost     |
+-----------------------------+----------+--------------------------------------+
```

线程分布：

```text
+------------------+----------------------+--------------------------------+
| [thread group]   | [evidence]           | [meaning]                      |
+------------------+----------------------+--------------------------------+
| slot-*           | top CPU threads      | downstream decode/transform hot|
| writer/ex-*      | active but scattered | sink/write path active         |
| kcv2p*           | active, not dominant | source poll not bottleneck     |
| rdk:broker*      | active               | Kafka decompress/network active|
| jemalloc_bg_thd  | visible in perf      | allocator pressure             |
+------------------+----------------------+--------------------------------+
```

### 日志证据

`tide_worker.log` 未看到 consumer_v2 的 consume error、revoke/assign 风暴或 socket 异常；近期主要是 Kafka register/hot-reboot 信息。

```text
+---------------------------+
| [logs]                    |
+---------------------------+
| no consumer_v2 error loop |
| no rebalance storm        |
| no ring/socket error      |
+---------------------------+
```

### 当前瓶颈判断

```text
+------------------------------+
| [source side]                |
| poll 2.5 GiB/s payload       |
| ring has free slots          |
+------------------------------+
              |
              v
+------------------------------+
| [worker boundary]            |
| read ~= ack ~= dispatch      |
| queue depth low              |
+------------------------------+
              |
              v
+------------------------------+
| [hot path]                   |
| JSON decode                  |
| string/Arrow append          |
| LZ4 compression              |
| allocator/page fault/cache   |
+------------------------------+
```

本轮瓶颈已经从 `consumer_v2 ring backpressure` 转移到下游 CPU/内存路径：

1. JSON 解码和字段写入是主要业务 CPU。
2. 输出侧 LZ4 压缩成为明显热点。
3. Arrow/String append 和 page fault/cache miss 说明内存分配、对象构建和数据拷贝成本很高。
4. IO 没有 `iodelay`，磁盘 util 不满，所以不是磁盘硬卡死。

### 为什么 48 -> 60 几乎没有收益

- 增加 workers 只增加并发消费者数量，不降低单条记录的 JSON decode、string write、Arrow append、LZ4 compress 成本。
- 当前 CPU 已消耗约 `19.7` cores，cache miss `38.18%`，继续加 worker 会放大调度、cache 和 allocator 压力。
- `activeLeaseCount=8`、`workerQueueDepth=11`，说明不是 60 个 worker 都在被 consumer_v2 持续喂满；并发受上游 partition ready、下游执行图和 writer path 共同限制。
- `slot-*` 与 `writer/ex-*` 热点分散，说明需要优化每阶段处理成本或下游并行布局，而不是只加 consumer workers。

### 变更建议

1. 优先优化 `JSONStructuredDecoder`：
   - 减少不必要字段解析。
   - 避免重复 string materialize。
   - 检查是否可以按所需列投影解析。
   - 增加 decoder 级 metrics：records/s、bytes/s、parse ns/record、output bytes/record。
2. 优化输出压缩：
   - 当前 `LZ4_compress_fast_extState` 明显进入热点。
   - 评估压缩级别、batch size、block size 和 flush 周期。
   - 如果下游允许，A/B 降低压缩成本或增大 block。
3. 优化 Arrow/String builder：
   - 预估 capacity，减少 append 触发的扩容和 page fault。
   - 复用 builder/buffer，减少短生命周期大对象。
4. worker 数建议：
   - 暂时不要继续增加到 72/96。
   - 建议 A/B `48`、`60`、`72`，用 `payload MiB/s / CPU core` 和 `RSS delta` 作为主指标。
   - 如果 60 比 48 吞吐不明显提升但 CPU/RSS 上升，应回退到 48 或更低。
5. consumer_v2 参数建议：
   - `auto_scale_max_poll_thread_count=2` 保持。
   - `ringCapacity=524288` 当前更健康，不建议回到 `2097152`。
   - `high/low=8192/2048` 当前可继续观察；若 pause 仍频繁但 ring 不满，可以评估 `16384/4096`。

## eBPF Stack Analysis

### 目标

- 用 `bpftrace` 在不生成大 perf.data 的情况下观察当前 `6511` 进程。
- 先用 thread `comm`、kernel stack、syscall/write probe 定位方向。
- 再尝试 user stack；如果 user stack 解不出来，不阻塞判断。
- 学习重点：先回答“谁忙”和“忙在哪类系统路径”，再决定是否做符号级栈分析。

### 当前对象

```text
+--------------------------------------+
| listen port: 6511                    |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| pid: 319594 tide_worker              |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| uds: /var/run/tide/worker_6510.sock  |
+--------------------------------------+
```

确认命令：

```bash
ss -tlnp | grep 6511
ss -lxp | grep worker_6510
pgrep -a tide_worker | head -10
```

### 为什么 bpftrace 看起来像 hang

`bpftrace` 不一定是真的卡死，常见原因：

- `ustack()` 需要读取进程 maps、符号、unwind 信息；`tide_worker` 线程多、VIRT/RSS 大时启动会慢。
- probe attach 需要内核侧加载和校验 BPF program；复杂 probe 或多个 probe 会更慢。
- 对 1000+ 线程进程做用户栈聚合时，输出前要等 `timeout` 结束并打印 map。
- 如果二进制缺少 frame pointer 或 unwind 信息不可用，`ustack()` 可能大量显示 `@[]`。

建议：

```bash
# 先用 timeout，避免误以为 hang。
timeout 15 bpftrace -e 'profile:hz:99 /pid == 319594/ { @[comm] = count(); }'

# 需要长时间采样时再放 tmux，避免 SSH/IDE 终端断开。
tmux new -s bpf-6511
timeout 60 bpftrace -e 'profile:hz:99 /pid == 319594/ { @[comm] = count(); }' | tee /tmp/bpf-6511-comm.txt
```

### 推荐采样顺序

#### 1. comm 级 CPU 分布

先看线程类别，成本低，最稳定：

```bash
timeout 20 bpftrace -e 'profile:hz:99 /pid == 319594/ { @[comm] = count(); }'
```

本次结果摘要：

```text
top groups:
- slot-*             high CPU samples
- writer/ex-*        active and widespread
- rdk:broker-*       active Kafka broker/decompress/network threads
- jemalloc_bg_thd    allocator/background memory pressure
- kc-1-323920        high syscall/futex pressure in later probes
```

解释：

- CPU 不集中在 consumer_v2 direct-dispatch 队列线程。
- `slot-*`、`writer/ex-*`、`rdk:broker-*` 同时活跃，说明 source、decode/transform、sink/write 都在工作。
- 结合 UDS queue 全空，更像下游处理/写出容量限制，而不是 consumer_v2 内部排队。

#### 2. kernel stack

看内核时间花在哪类路径：

```bash
timeout 20 bpftrace -e 'profile:hz:99 /pid == 319594/ { @[kstack(12)] = count(); }'
```

本次有代表性的 kernel stacks：

```text
copy_user_enhanced_fast_string
_copy_to_iter
__skb_datagram_iter
tcp_recvmsg_locked
tcp_recvmsg
inet6_recvmsg
sock_recvmsg
____sys_recvmsg
```

含义：

- Kafka/librdkafka 网络读取仍活跃。
- 这是输入侧正常工作信号，不代表 source 阻塞。

```text
copy_user_enhanced_fast_string
copy_page_from_iter_atomic
generic_perform_write
__generic_file_write_iter
generic_file_write_iter
new_sync_write
vfs_write
ksys_write
```

含义：

- 用户态向文件/下游 fd 写数据明显活跃。
- 与 `writer/ex-*`、`fringedb-*` 的 `vfs_write` bytes 结果一致。

```text
__handle_mm_fault
handle_mm_fault
do_user_addr_fault
exc_page_fault
```

含义：

- 大量对象构建、buffer 扩容、mmap/allocator 活动带来 page fault 成本。
- 与 perf 中 `__handle_mm_fault`、cache miss、jemalloc 背景线程热点一致。

```text
madvise_free_pte_range
walk_page_range
do_madvise
__x64_sys_madvise
```

含义：

- allocator 正在回收/释放页，说明内存 churn 明显。
- 下游 JSON/Arrow/string 构建和压缩输出可能产生大量短生命周期内存。

#### 3. user stack

尝试用户栈：

```bash
cd /proc/319594/cwd
timeout 30 bpftrace -e 'profile:hz:49 /pid == 319594/ { @[ustack(20)] = count(); }'
```

本次现象：

```text
@[] dominates or command may timeout/exit without useful stacks
```

解释：

- 当前环境 user stack unwind 不可靠。
- 不能因为 `ustack()` 无法展开就说 eBPF 没用；应退回到 `comm`、`kstack`、syscall/kprobe。
- 若必须拿用户栈，后续可以单独准备带 frame pointer 的 binary，或用 `/proc/$pid/cwd` 下的符号路径重试。

#### 4. futex/syscall 压力

看锁/等待/系统调用热点：

```bash
timeout 12 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /pid == 319594/ { @[comm] = count(); }'
```

本次高位：

```text
kc-1-323920:      61742
rdk:broker18664: 13109
slot-* threads:  several thousands each
```

含义：

- 锁/条件变量等待集中在 Kafka client、broker thread、slot 下游执行线程。
- 没看到 consumer_v2 direct queue 本身成为 futex 热点。

读写 syscall 入口：

```bash
timeout 12 bpftrace -e \
'tracepoint:syscalls:sys_enter_read /pid == 319594/ { @[comm] = count(); }
 tracepoint:syscalls:sys_enter_write /pid == 319594/ { @[comm] = count(); }
 tracepoint:syscalls:sys_enter_pwrite64 /pid == 319594/ { @[comm] = count(); }'
```

本次高位主要是：

```text
writer/ex-* threads
c2a59e8a/w-* threads
build thread
```

含义：

- 下游 writer/executor 在频繁 syscall。
- 如果吞吐继续不足，优先看 writer batch、flush、block size、小写放大。

#### 5. vfs_write bytes

看谁写了最多 bytes：

```bash
timeout 8 bpftrace -e \
'kprobe:vfs_write /pid == 319594/ { @bytes[comm] = sum(arg2); @cnt[comm] = count(); }' \
  | grep '@bytes' | tail -80
```

本次 top bytes：

```text
fringedb-c6:   ~1.10 GB / 8s
fringedb-c4:   ~1.04 GB / 8s
fringedb-c5:   ~0.96 GB / 8s
writer/ex-48:  ~302 MB / 8s
writer/ex-47:  ~294 MB / 8s
writer/ex-17:  ~280 MB / 8s
writer/ex-34:  ~269 MB / 8s
writer/ex-21:  ~265 MB / 8s
writer/ex-23:  ~259 MB / 8s
```

解释：

- 写出压力主要在 `fringedb-*` 和 `writer/ex-*`。
- 这支持“下游写出/编码/压缩链路限制 ack 前进速度”的判断。
- 这不是磁盘全局 iowait 卡死；更像 writer/sink path 的 CPU、batch、flush、内存构建共同限制。

### 与 UDS 的交叉验证

同一窗口 UDS 证据：

```text
productionRecordsPerSec       ~= 554k/s
ackedOffsetRecordsPerSec      ~= 401k/s
brokerCommittedRecordsPerSec  = 0/s
direct pushed/s               ~= 401k/s
ringLiveCount                 = 0
readyPartitionCount           = 0
workerQueueDepth              = 0
```

解释：

- input > ack，因此 lag 必然增长。
- consumer_v2 队列为空，所以不是 direct-dispatch queue 堵住。
- direct pushed/s 与 ackedOffsetRecordsPerSec 接近，说明 source 能把记录推到 worker/downstream，但端到端处理能力不够。
- `brokerCommittedRecordsPerSec=0` 是独立部署问题：当前进程还没有 direct periodic commit fix，必须重启/更新到包含修复的版本。

### eBPF 结论

```text
+-------------------------+     +-------------------------+     +---------------------------+
| Kafka source/input      | --> | consumer_v2 direct path | --> | downstream slot/writer    |
| ~554k records/s         |     | queues empty, ~401k/s   |     | eBPF write/syscall hot    |
+-------------------------+     +-------------------------+     +---------------------------+
           |                               |                                  |
           v                               v                                  v
 input faster than                 not the queue bottleneck           ack advances too slowly
 processing capacity
```

主瓶颈：

```text
downstream slot/writer/fringedb write path capacity
```

独立问题：

```text
current 6511 process still lacks direct periodic commit fix
```

### 下一步命令

部署包含 commit fix 的 binary 后，先确认 commit path：

```bash
python3 - <<'PY'
import json, subprocess, time
sock = "/var/run/tide/worker_6510.sock"
def s():
    return json.loads(subprocess.check_output(
        ["curl","-s","--unix-socket",sock,"http://localhost/json"]
    ))["consumers"][0]
a = s()
time.sleep(10)
b = s()
for f in ["totalPeriodicCommitCalls", "totalCommitCalls", "totalCommitGap"]:
    print(f, a[f], "->", b[f], "delta", b[f] - a[f])
print("brokerCommittedRecordsPerSec", b["brokerCommittedRecordsPerSec"])
PY
```

再做 eBPF 30s 复测：

```bash
timeout 30 bpftrace -e 'profile:hz:99 /pid == 319594/ { @[comm] = count(); }'
timeout 30 bpftrace -e 'profile:hz:99 /pid == 319594/ { @[kstack(12)] = count(); }'
timeout 30 bpftrace -e 'kprobe:vfs_write /pid == 319594/ { @bytes[comm] = sum(arg2); @cnt[comm] = count(); }'
timeout 30 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /pid == 319594/ { @[comm] = count(); }'
```
