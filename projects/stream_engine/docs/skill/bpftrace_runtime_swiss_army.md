# bpftrace 运行时瑞士军刀

## 1. 定位

把 bpftrace 视为线上 tide_worker 的"运行时瑞士军刀"：在不重启进程、不改代码的前提下，
观察 consumer_v2 真实路径上每个函数的调用频率、调用栈、参数与延迟，配合源码做"静态 review +
动态 trace"双向验证，找出 metrics 看不出的隐藏热点。

适用场景：

- 消费追不上生产，但 metrics 看不出哪个环节最慢
- 生命周期平均吞吐与瞬时吞吐口径打架，怀疑 hot loop 自旋
- pause/resume、commit、auto-scale 等控制路径的调用频率与时序
- mutex 大锁是否被某个 hot caller 占据
- librdkafka 回调、回收路径是否在主循环里阻塞

## 2. 准备工作

### 2.1 安装

```bash
apt-get install -y bpftrace
bpftrace --version    # >= 0.17 即可
```

### 2.2 进程定位

线上 tide_worker 是 bundle 部署，符号在主 binary 内，但其它依赖通过相对 `./lib` 加载。
所有 bpftrace 命令都应该在 `/proc/$pid/cwd` 下执行：

```bash
PID=$(lsof -i:6511 -sTCP:LISTEN -t)        # 也可用 ss -tlnp | grep :6511
ls /proc/$PID/cwd                            # 确认 lib/ 等相对依赖
cd /proc/$PID/cwd
BIN=$(readlink /proc/$PID/exe)
```

### 2.3 找符号

consumer_v2 的关键符号都是 `tide::kafka::consumer_v2::SharedConsumerState::*`，
直接在主 binary 上查 mangled name：

```bash
nm "$BIN" | grep -E 'syncPartitionPauseLocked|commitOffsetsLocked|ackBatch|tryDispatchForWorkerLocked'
```

把 mangled name 喂给 `uprobe:$BIN:<mangled>` 即可。

## 3. 一次性问诊脚本

### 3.1 总体调用频率

在 hot path 上一次性看 5 秒内每个关键函数被调用几次：

```bash
PID=774304; cd /proc/$PID/cwd
BIN=$(readlink /proc/$PID/exe)
SYNC=_ZN4tide5kafka11consumer_v219SharedConsumerState24syncPartitionPauseLockedEj
COMMIT=_ZN4tide5kafka11consumer_v219SharedConsumerState19commitOffsetsLockedERKSt8optionalISt6vectorINS1_14TopicPartitionESaIS5_EEEb

bpftrace -p $PID -e '
uprobe:'"$BIN"':'"$SYNC"'   { @sync   = count(); @sync_by_tid[tid]   = count(); }
uprobe:'"$BIN"':'"$COMMIT"' { @commit = count(); @commit_by_tid[tid] = count(); }
interval:s:5 { print(@sync); print(@commit); print(@sync_by_tid); exit(); }
'
```

输出形如：

```
@sync: 1579628                # 5s 调用 158w 次 ≈ 316k qps
@commit: 4                    # 5s 仅 4 次（与 commitIntervalMs=5000 + ackBatch 触发时机有关）
@sync_by_tid[779533]: 381824  # 单线程 76k qps，几乎全是 poll 线程
```

### 3.2 调用方反查

挑一个高频 tid，再看是谁在 5s 内反复调它：

```bash
bpftrace -p $PID -e '
uprobe:'"$BIN"':'"$SYNC"' /tid == 779533/ {
  @callers[ustack(perf,3)] = count();
}
interval:s:5 { print(@callers); exit(); }
'
```

```bash
for t in 779533 778789 778426; do
  printf 'tid=%s comm=%s\n' $t "$(cat /proc/$PID/task/$t/comm)"
done
```

`comm=kcv2p538cc4-*` 即 consumer_v2 poll 线程。

### 3.3 函数延迟直方图

锁竞争和 librdkafka 同步调用要看 P50/P99：

```bash
bpftrace -p $PID -e '
uprobe:'"$BIN"':'"$COMMIT"' { @t[tid] = nsecs; }
uretprobe:'"$BIN"':'"$COMMIT"' /@t[tid]/ {
  @lat = hist((nsecs - @t[tid]) / 1000);   # 单位 us
  delete(@t[tid]);
}
interval:s:10 { print(@lat); exit(); }
'
```

### 3.4 mutex 等待

`std::mutex::lock` 在 glibc 里走 `pthread_mutex_lock`：

```bash
bpftrace -e '
uprobe:/lib/x86_64-linux-gnu/libpthread.so.0:pthread_mutex_lock { @t[tid] = nsecs; }
uretprobe:/lib/x86_64-linux-gnu/libpthread.so.0:pthread_mutex_lock /@t[tid]/ {
  @wait = hist((nsecs - @t[tid]) / 1000);
  delete(@t[tid]);
}
interval:s:5 { print(@wait); exit(); }
' -p $PID
```

bundle 不一定走 `libpthread.so.0`，可以先 `cat /proc/$PID/maps | grep -E 'libpthread|libc\.so'`
找到实际 .so，再换路径。

### 3.5 调用栈采样定位 hot path

```bash
bpftrace -p $PID -e '
profile:hz:99 { @samples[ustack(perf, 8)] = count(); }
interval:s:10 { print(@samples); exit(); }
' | head -200
```

## 4. consumer_v2 消费慢根因

### 4.1 现象

- Kafka 监控：消费 records/s 远低于生产 records/s，lag 持续增长
- UDS metrics：`polledToProductionRatio ≈ 0.38`，`ringLiveCount` 远未满（5w / 26w）
- `totalPauseCalls`、`totalResumeCalls` 每 10s 增加 ~520 次，几乎对称
- `pausedPartitionCount` 在 7~18 之间反复抖动
- 52/125 partition 出现"acked offset 已推进但 broker committed offset 不动"

### 4.2 静态 review

- `unified_consumer.cpp:2354` 在 poll 路径中**对每条 record** 调一次 `syncPartitionPauseLocked`
- `unified_consumer.cpp:1296` 在 ackBatch 路径中**每次 ack** 也调用 `syncPartitionPauseLocked`
- `dispatch_state.cpp:336` `updatePauseState` 用 `>=highWatermark / <=lowWatermark`
  作为切换条件，缺乏滞回；watermark=4096/1024 时 dispatch 一个 batch 就跨过 lowWatermark
- `unified_consumer.cpp:1292` 周期 commit 直接挂在 ackBatch 内部，与 poll/ack/dispatch
  共用 `mutex_`

### 4.3 动态 trace

5s `syncPartitionPauseLocked` 158w 次，76k qps 集中在单条 poll 线程。
每次都拿大锁、构建 `vector<TopicPartition*>`、再 `destroy`，即便 `shouldPause==isPaused`
也只是函数体内 early return，外层 `std::lock_guard<std::mutex>` 已经持有 hot mutex。

由此推出：

- mutex 被 poll 线程长期占用 → ack/commit/dispatch 都在排队
- 周期 commit 失去机会窗口 → broker offset 不前进，Kafka 监控显示 lag 涨
- 真正的 pause/resume 也在 high/low watermark 边界振荡 → librdkafka 不停翻转
- 即便 worker 数量 48→60 也没收益，因为瓶颈在大锁本身

### 4.4 根因结论

> **consumer_v2 的 hot path 把"状态查询"和"状态切换"混在一个函数里，并在 mutex 内对每条
> record 都调用一次。**

只要 partition 没翻转，函数体本应是 no-op，但仍然消耗大锁，等于把 poll 路径钉在大锁上。

## 5. 优化方向（按 commit 推进）

依次落地，每一步独立提交、独立验证：

1. **Pause/resume dirty bit**：拆分查询与切换，poll/ack 路径只在状态翻转时调用 broker
   pause/resume；其余情况 zero-cost。
2. **周期 commit 出 ackBatch**：让 commit 走专门触发点，避免 hot mutex 拖慢；
   `ackBatch` 仅记录 next offset，commit 由 dispatch 边界或独立调度器驱动。
3. **Watermark 滞回**：保留 high/low 二阈值的同时，加入"距离阈值 N 条记录"的滞回，
   抑制 dispatch batch 跨阈值后立即翻转。
4. **回归测试**：补 dirty bit 行为、commit 时序、watermark 滞回的单元测试，
   防止后续回退。

每一步落地后，再用 §3 的脚本重新采样：

- `@sync` 调用次数应骤降（从 158w → 数百次/5s）
- `@commit` 调用次数应稳定按 `commitIntervalMs` 节奏推进
- `pausedPartitionCount` 不再振荡
- `polledToProductionRatio` 上升，broker committed records/s 接近 ack records/s

## 6. 经验总结

- bpftrace 不取代 metrics，但能补足 metrics 看不到的"per-call 频率"维度
- bundle 部署一律 `cd /proc/$pid/cwd` 再 attach，符号与 lib 才完整
- 任何 hot path 调用都要看"每条 record 多少次"，而不是只看单次延迟
- 控制语句和数据语句分离：状态查询尽量 lock-free，状态切换才走系统调用
- 复盘要"静态 review + 动态 trace"双向印证，不能只信一种证据
