# Consumer V2 UDS Metrics And Commit Coupling Lesson

## 1. 结论

这次问题已经确认不是 `commitAsync` 可见性问题，而是一个更隐蔽的耦合 bug：

- `consumer_v2` 的 UDS metrics 查询不是纯观测。
- 查询 `/json` 会走到 `buildRuntimeInfo()`。
- `buildRuntimeInfo()` 内部会调用 `syncDirectCommittedOffsetsLocked()`。
- `syncDirectCommittedOffsetsLocked()` 会把 direct ack slot 中的 offset 写回
  `committedOffsetByTopicPartition_`，并把对应 partition 标进
  `dirtyCommittedTopicPartitions_`。
- 周期 broker commit 又依赖 `dirtyCommittedTopicPartitions_` 非空才会被调度。
- 于是形成了错误耦合。

总览图：

```text
+------------------+
| UDS GET /json    |
+--------+---------+
         |
         v
+-------------------------------+
| SharedConsumerState::         |
| buildRuntimeInfo()            |
+---------------+---------------+
                |
                v
+-------------------------------+
| SharedConsumerState::         |
| syncDirectCommittedOffsets    |
| Locked()                      |
+---------------+---------------+
                |
                v
+-------------------------------+
| dirtyCommittedTopicPartitions_|
| becomes non-empty             |
+---------------+---------------+
                |
                v
+-------------------------------+
| SharedConsumerState::         |
| buildCommitOffsetRequest      |
| Locked()                      |
+---------------+---------------+
                |
                v
+-------------------------------+
| SharedConsumerState::         |
| executeCommitOffsetRequest()  |
+---------------+---------------+
                |
                v
+-------------------------------+
| broker committed offset       |
| moves forward                 |
+-------------------------------+
```

如果没有 UDS metrics 查询，direct ack 仍然在推进，但 dirty 集合不推进，周期 commit
可能长期不触发，Kafka lag 就会持续增长。

## 2. 复盘背景

现场有两个 tide_worker 进程：

- `6511`：持续被观测，lag 正常。
- `7511`：未被观测，lag 持续增长。

进程映射如下：

```text
6511 -> pid 1592565
7511 -> pid 1621549
```

同时确认：

- `commitAsync` callback 与 broker 可见性没有问题。
- 问题只在于“commit 调度是否被触发”。

### 2.1 UDS 设计目标

这次问题还暴露出一个被破坏的设计前提。最初对 consumer_v2 UDS 观测面的目标是：

- UDS 只读 runtime state，不推进生产状态。
- UDS 优先读取已有原子变量和只读快照，不引入额外的复杂锁竞争。
- UDS 的存在与否，不应该改变 commit、lag、dispatch、ack 的真实行为。
- UDS 只是观测面，不是修复生产路径缺口的“隐式驱动器”。

按这个目标，`GET /json` 理应满足下面的约束：

```text
+-------------------------------+
| UDS /json                     |
+---------------+---------------+
                |
                v
+-------------------------------+
| read-only state view          |
| atomic variables / snapshots  |
+---------------+---------------+
                |
                v
+-------------------------------+
| no state mutation             |
| no commit side effect         |
+-------------------------------+
```

本次 bug 的本质，就是 UDS 违反了这个原始目标。

## 3. 关键代码路径

### 3.1 UDS metrics 路径

`debug_socket_service.cpp` 的请求链路：

```text
+------------+
| GET /json  |
+-----+------+
      |
      v
+-------------------------------+
| ConsumerV2DebugSocketService::|
| buildJsonResponse()           |
+---------------+---------------+
                |
                v
+-------------------------------+
| buildUnifiedConsumerDebug     |
| Snapshot(socketPath)          |
+---------------+---------------+
                |
                v
+-------------------------------+
| SharedConsumerState::         |
| buildRuntimeInfo()            |
+-------------------------------+
```

其中：

- `buildJsonResponse()` 位于 `src/source/kafka/consumer_v2/debug_socket_service.cpp`
- `buildRuntimeInfo()` 位于 `src/source/kafka/consumer_v2/shared_consumer.cpp`

关键代码：

```cpp
std::string buildJsonResponse() const {
    auto snapshot = buildUnifiedConsumerDebugSnapshot(socketPath_);
    return buildConsumerV2JsonResponseBody(snapshot);
}
```

`buildUnifiedConsumerDebugSnapshot()` 最终会调用：

```cpp
ConsumerDebugSnapshot consumerSnapshot{
    .key = state->key(),
    .runtimeInfo = state->buildRuntimeInfo(),
};
```

### 3.2 buildRuntimeInfo 的副作用

`buildRuntimeInfo()` 并不是纯只读：

```cpp
RuntimeInfo buildRuntimeInfo() {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanupExpiredWorkersLocked();
    syncDirectCommittedOffsetsLocked();
    refreshLagSnapshotLocked(nowMs());
    ...
}
```

其中 `syncDirectCommittedOffsetsLocked()` 会把 direct slot 里的 next offset 写回
`committedOffsetByTopicPartition_`。

副作用图：

```text
+-------------------------+
| buildRuntimeInfo()      |
+------------+------------+
             |
             v
+-------------------------+
| cleanupExpiredWorkers   |
+------------+------------+
             |
             v
+-------------------------------+
| syncDirectCommittedOffsets    |
| Locked()                      |
+---------------+---------------+
                |
                v
+-------------------------------+
| recordCommittedTopicOffset    |
| Locked()                      |
+---------------+---------------+
                |
                v
+-------------------------------+
| dirtyCommittedTopicPartitions_|
| gets updated                  |
+-------------------------------+
```

关键实现：

```cpp
void syncDirectCommittedOffsetsLocked() {
    for (size_t index = 0; index < directCommittedSlotCount_; ++index) {
        const auto& slot = directCommittedSlots_[index];
        if (!slot.assigned.load(std::memory_order_acquire)) {
            continue;
        }
        auto nextOffset = slot.nextOffset.load(std::memory_order_acquire);
        if (nextOffset >= 0) {
            recordCommittedTopicOffsetLocked(slot.topicPartition, nextOffset);
        }
    }
}
```

而 `recordCommittedTopicOffsetLocked()` 会把 partition 标脏：

```cpp
void recordCommittedTopicOffsetLocked(const TopicPartition& topicPartition,
                                      int64_t nextOffset) {
    ...
    if (iter == committedOffsetByTopicPartition_.end()) {
        committedOffsetByTopicPartition_.emplace(topicPartition, nextOffset);
        dirtyCommittedTopicPartitions_.insert(topicPartition);
        return;
    }
    if (iter->second < nextOffset) {
        iter->second = nextOffset;
        dirtyCommittedTopicPartitions_.insert(topicPartition);
    }
}
```

### 3.3 direct ack 与周期 commit 的错位

direct ack 路径本身只更新 slot 原子变量：

```cpp
void applyDirectAck(const DirectAck& ack) {
    if (ack.slotIndex < directCommittedSlotCount_) {
        auto& slot = directCommittedSlots_[ack.slotIndex];
        auto nextOffset = ack.offset + 1;
        ...
        slot.nextOffset.compare_exchange_weak(...);
    }
    totalAckedRecords_.fetch_add(ack.recordCount, ...);
}
```

但周期 commit 调度前先看 dirty 集合：

```cpp
void maybeEnqueueDirectPeriodicCommit() {
    CommitOffsetRequest periodicCommitRequest;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto currentMs = nowMs();
        if (!config_.enableAutoCommit &&
            config_.commitIntervalMs > 0 &&
            !periodicCommitInFlight_.load(...) &&
            !dirtyCommittedTopicPartitions_.empty() &&
            currentMs - lastPeriodicCommitMs_.load(...) >= config_.commitIntervalMs) {
            periodicCommitRequest = buildCommitOffsetRequestLocked(std::nullopt, true);
            periodicCommitInFlight_.store(!periodicCommitRequest.empty(), ...);
        }
    }
    if (!periodicCommitRequest.empty()) {
        enqueueOwnerCommit(std::move(periodicCommitRequest));
    }
}
```

因此出现了逻辑断裂。

无观测时的异常路径：

```text
+------------------+
| applyDirectAck() |
+--------+---------+
         |
         v
+-------------------------------+
| directCommittedSlots_[slot]   |
| .nextOffset updates           |
+---------------+---------------+
                |
                v
+-------------------------------+
| dirtyCommittedTopicPartitions_|
| unchanged                     |
+---------------+---------------+
                |
                v
+-------------------------------+
| maybeEnqueueDirectPeriodic    |
| Commit() sees dirty set empty |
+---------------+---------------+
                |
                v
+-------------------------------+
| no CommitOffsetRequest        |
+-------------------------------+
```

但一旦有人查询 metrics，路径就被“意外补齐”。

有观测时的耦合路径：

```text
+-------------------------------+
| SharedConsumerState::         |
| buildRuntimeInfo()            |
+---------------+---------------+
                |
                v
+-------------------------------+
| syncDirectCommittedOffsets    |
| Locked()                      |
+---------------+---------------+
                |
                v
+-------------------------------+
| recordCommittedTopicOffset    |
| Locked()                      |
+---------------+---------------+
                |
                v
+-------------------------------+
| dirtyCommittedTopicPartitions_|
| becomes non-empty             |
+---------------+---------------+
                |
                v
+-------------------------------+
| CommitOffsetRequest later     |
| executes                      |
+-------------------------------+
```

## 4. 观测手段

### 4.1 端口 / 进程映射

先通过 `ss -ltnp` 和 `/proc/$pid` 建立映射：

```bash
ss -ltnp | egrep '6511|7511'
python - <<'PY'
import os
for pid in [1592565,1621549]:
    print(pid, os.readlink(f'/proc/{pid}/exe'))
    print(pid, os.readlink(f'/proc/{pid}/cwd'))
PY
```

确认：

```text
6511 -> pid 1592565
7511 -> pid 1621549
exe  -> /data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker
```

### 4.2 确认 UDS socket 路径

`debug_socket_service.cpp` 默认 socket 命名规则是：

```text
/var/run/tide/worker_${LISTEN_PORT0}.sock
```

查看进程环境：

```bash
python - <<'PY'
import os
keys=[b'LISTEN_PORT0',b'PORT0',b'TIDE_DEBUG_SERVICE_PORT']
for pid in [1592565,1621549]:
    data=open(f'/proc/{pid}/environ','rb').read().split(b'\\0')
    env=dict(item.split(b'=',1) for item in data if b'=' in item)
    print('PID',pid)
    for k in keys:
        print(k.decode(), env.get(k,b'').decode())
PY
```

结果：

```text
pid 1592565 -> LISTEN_PORT0=6510
pid 1621549 -> LISTEN_PORT0=7510
```

所以对应 UDS socket 为：

- `/var/run/tide/worker_6510.sock`
- `/var/run/tide/worker_7510.sock`

用 `curl --unix-socket` 可直接访问：

```bash
curl --unix-socket /var/run/tide/worker_7510.sock -sS http://localhost/json
```

### 4.3 关键函数符号定位

通过 `nm -C` 定位 uprobe 地址：

```bash
nm -C /data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker | \
egrep 'buildRuntimeInfo|syncDirectCommittedOffsetsLocked|buildCommitOffsetRequestLocked|executeCommitOffsetRequest|ackBatchDirect|buildJsonResponse'
```

关键地址：

```text
0x147a8e0  SharedConsumerState::buildRuntimeInfo()
0x147a870  SharedConsumerState::syncDirectCommittedOffsetsLocked()
0x147dd00  SharedConsumerState::buildCommitOffsetRequestLocked(...)
0x1472210  SharedConsumerState::executeCommitOffsetRequest(...)
0x147d1b0  SharedConsumerState::ackBatchDirect(...)
0x148e310  ConsumerV2DebugSocketService::buildJsonResponse() const
```

### 4.4 eBPF 基线观测

#### 4.4.1 双进程 15 秒基线

命令：

```bash
bpftrace -e '
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147d1b0 /pid==1592565 || pid==1621549/ { @counts[pid, "ackBatchDirect"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147a8e0 /pid==1592565 || pid==1621549/ { @counts[pid, "buildRuntimeInfo"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147e0c0 /pid==1592565 || pid==1621549/ { @counts[pid, "maybeEnqueueDirectPeriodicCommit"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147dd00 /pid==1592565 || pid==1621549/ { @counts[pid, "buildCommitOffsetRequestLocked"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x1472210 /pid==1592565 || pid==1621549/ { @counts[pid, "executeCommitOffsetRequest"] = count(); }
interval:s:15 { exit(); }
'
```

结果：

```text
@counts[1592565, buildRuntimeInfo]: 15
@counts[1592565, buildCommitOffsetRequestLocked]: 15
@counts[1592565, executeCommitOffsetRequest]: 15
@counts[1621549, ackBatchDirect]: 5045
@counts[1592565, ackBatchDirect]: 9809
```

解释：

- `6511` 在 15 秒内稳定发生 `buildRuntimeInfo/buildCommit/executeCommit`。
- `7511` 在同一窗口里 `ackBatchDirect` 已经发生 `5045` 次，说明它在真实消费并 ack。
- 但 `7511` 没有看到 `buildRuntimeInfo/buildCommit/executeCommit`，说明“消费推进”与“commit 调度”
  已经分叉。

基线对照图：

```text
6511:
+-------------------------------+
| ackBatchDirect                |
| 9809                          |
+---------------+---------------+
                |
                v
+-------------------------------+
| SharedConsumerState::         |
| buildRuntimeInfo()            |
| 15                            |
+---------------+---------------+
                |
                v
+-------------------------------+
| executeCommitOffsetRequest()  |
| 15                            |
+-------------------------------+

7511:
+-------------------------------+
| ackBatchDirect                |
| 5045                          |
+---------------+---------------+
                |
                v
+-------------------------------+
| SharedConsumerState::         |
| buildRuntimeInfo()            |
| 0                             |
+---------------+---------------+
                |
                v
+-------------------------------+
| executeCommitOffsetRequest()  |
| 0                             |
+-------------------------------+
```

#### 4.4.2 7511 无观测时的 6 秒基线

命令：

```bash
bpftrace -e '
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147d1b0 /pid==1621549/ { @counts["ackBatchDirect"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147a8e0 /pid==1621549/ { @counts["buildRuntimeInfo"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147a870 /pid==1621549/ { @counts["syncDirectCommittedOffsetsLocked"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147dd00 /pid==1621549/ { @counts["buildCommitOffsetRequestLocked"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x1472210 /pid==1621549/ { @counts["executeCommitOffsetRequest"] = count(); }
interval:s:6 { exit(); }
'
```

结果：

```text
@counts[ackBatchDirect]: 2561
```

解释：

- 没有观测时，`7511` 只有 direct ack 在前进。
- `buildRuntimeInfo/syncDirectCommittedOffsetsLocked/buildCommitOffsetRequestLocked/executeCommitOffsetRequest`
  全部为 `0`。

无观测时状态图：

```text
+-------------------------------+
| directCommittedSlots_[slot]   |
| .nextOffset grows             |
+---------------+---------------+
                |
                v
+-------------------------------+
| dirtyCommittedTopicPartitions_|
| still empty                   |
+---------------+---------------+
                |
                v
+-------------------------------+
| broker committed offset       |
| stalls                        |
+-------------------------------+
```

#### 4.4.3 对 7511 发起受控 UDS metrics 查询

通过 Unix socket 连续请求 `/json` 5 秒，同时对关键函数做 uprobe 计数。

命令：

```python
import subprocess, threading, time
sock='/var/run/tide/worker_7510.sock'
cmd = r'''bpftrace -e '
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147d1b0 /pid==1621549/ { @counts["ackBatchDirect"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147a8e0 /pid==1621549/ { @counts["buildRuntimeInfo"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147a870 /pid==1621549/ { @counts["syncDirectCommittedOffsetsLocked"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147dd00 /pid==1621549/ { @counts["buildCommitOffsetRequestLocked"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x1472210 /pid==1621549/ { @counts["executeCommitOffsetRequest"] = count(); }
interval:s:6 { exit(); }
' '''
```

结果：

```text
curl_count=23
@counts[executeCommitOffsetRequest]: 1
@counts[buildCommitOffsetRequestLocked]: 1
@counts[buildRuntimeInfo]: 20
@counts[syncDirectCommittedOffsetsLocked]: 20
@counts[ackBatchDirect]: 4979
```

解释：

- 仅仅因为对 `7511` 的 UDS socket 连续访问 `/json`，`buildRuntimeInfo` 和
  `syncDirectCommittedOffsetsLocked` 就立刻高频出现。
- 与此同时，之前为 `0` 的 `buildCommitOffsetRequestLocked` 和
  `executeCommitOffsetRequest` 也开始出现。
- 这证明 metrics 观测确实在驱动 commit 调度。

受控触发图：

```text
+-------------------------------+
| curl --unix-socket            |
| /var/run/tide/worker_7510.sock|
| http://localhost/json         |
+---------------+---------------+
                |
                v
+-------------------------------+
| buildRuntimeInfo()            |
| appears from 0                |
+---------------+---------------+
                |
                v
+-------------------------------+
| syncDirectCommittedOffsets    |
| Locked() appears from 0       |
+---------------+---------------+
                |
                v
+-------------------------------+
| buildCommitOffsetRequest      |
| Locked() appears from 0       |
+---------------+---------------+
                |
                v
+-------------------------------+
| executeCommitOffsetRequest()  |
| appears from 0                |
+-------------------------------+
```

#### 4.4.4 证明是 `/json` 路径直接触发

为了进一步排除其他路径，直接观察 `buildJsonResponse` 与 `buildRuntimeInfo` 的同步关系。

命令：

```python
import subprocess, threading, time
sock='/var/run/tide/worker_7510.sock'
cmd = r'''bpftrace -e '
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x148e310 /pid==1621549/ { @counts["buildJsonResponse"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147a8e0 /pid==1621549/ { @counts["buildRuntimeInfo"] = count(); }
uprobe:/data00/tmp/container-data-v3/public/root/tide_engine_1.1.0.6318/src/runtime/taskmanager/tide_worker:0x147a870 /pid==1621549/ { @counts["syncDirectCommittedOffsetsLocked"] = count(); }
interval:s:4 { exit(); }
' '''
```

结果：

```text
curl_count=14
@counts[buildRuntimeInfo]: 11
@counts[buildJsonResponse]: 11
@counts[syncDirectCommittedOffsetsLocked]: 11
```

解释：

- `buildJsonResponse`、`buildRuntimeInfo`、`syncDirectCommittedOffsetsLocked`
  的计数严格同步。
- 这说明 `/json` 路径正是触发 commit side effect 的源头。

## 5. 证据归因

基于上面的 eBPF 证据，可以对最初假设做结论：

- 假设 1：成立。
  - UDS metrics 查询确实会调用 `buildRuntimeInfo()`。
  - `buildRuntimeInfo()` 中的 `syncDirectCommittedOffsetsLocked()` 会把 acked direct slot
    同步进 dirty commit state。

- 假设 2：成立。
  - 未观测时，`7511` 的 `ackBatchDirect` 很活跃，但 commit request 构造和执行都为 `0`。
  - 说明 direct ack 本身没有把 partition 推进到周期 commit 可见状态。

- 假设 3：部分成立，但不是主根因。
  - `buildRuntimeInfo()` 还会调用 `refreshLagSnapshotLocked()`，确实也不是纯读。
  - 但本次 commit 能否被触发的关键证据已经由 `syncDirectCommittedOffsetsLocked()` 闭环解释。

- 假设 4：成立。
  - 6511 和 7511 的本质差异是“是否存在稳定的 metrics 入口调用”，而不是 Kafka broker 或 callback。

- 假设 5：成立。
  - 修复点必须放在 direct ack -> commit state -> periodic commit 的主路径上。
  - metrics 查询必须变成纯读。

## 6. 根因总结

根因可以归纳成一句话：

> `consumer_v2` 把“调试观测路径”误用了“生产状态推进路径”，导致 commit 调度依赖 metrics 查询副作用。

更具体地说：

1. direct ack 只更新 `directCommittedSlots_[slot].nextOffset`
2. 周期 commit 调度却依赖 `dirtyCommittedTopicPartitions_`
3. 这两者之间的同步，错误地放在了 `buildRuntimeInfo()` 里
4. 所以只有 metrics 查询时，dirty 集合才被推进
5. 最终表现为“看 metrics 的节点 commit 正常，不看的节点 lag 持续增长”

根因图：

```text
+-------------------------------+
| directCommittedSlots_         |
| holds latest acked state      |
+---------------+---------------+
                |
                | wrong bridge
                v
+-------------------------------+
| SharedConsumerState::         |
| buildRuntimeInfo()            |
| debug / metrics path          |
+---------------+---------------+
                |
                v
+-------------------------------+
| dirtyCommittedTopicPartitions_|
| gates periodic commit         |
+-------------------------------+
```

## 7. 彻底修复方案

我们不追求“补一刀让它先工作”，而是从第一性原理出发，把观测面和生产状态推进彻底解耦。

修复目标只有一个：

> metrics/debug 路径必须纯读；任何会改变 commit 状态的动作都只能发生在 owner/ack/commit 主路径。

同时再加上 UDS 的原始设计约束：

- UDS 只读。
- UDS 优先读取原子变量。
- UDS 不为观测额外引入复杂锁。
- UDS 不参与 commit 状态推进。

建议按下面方式重构。

目标架构图：

```text
read-only path:

+-------------------+
| UDS /json         |
+---------+---------+
          |
          v
+-------------------------------+
| SharedConsumerState::         |
| buildRuntimeInfo()            |
+---------------+---------------+
                |
                v
+-------------------------------+
| read directCommittedSlots_    |
| read commit state view        |
| read lag snapshot             |
+-------------------------------+

state-changing path:

+-------------------+
| direct ack        |
+---------+---------+
          |
          v
+-------------------------------+
| reconcileAckedOffsetsForCommit|
+---------------+---------------+
                |
                v
+-------------------------------+
| periodic commit               |
+-------------------------------+
```

### 7.1 第一步：拆分 sync 的职责

把当前的 `syncDirectCommittedOffsetsLocked()` 拆成两个层次：

1. `collectDirectCommittedOffsetsLocked(...)`
   - 纯读取 `directCommittedSlots_[].nextOffset`
   - 返回临时视图或临时列表
   - 不修改 `committedOffsetByTopicPartition_`
   - 不修改 `dirtyCommittedTopicPartitions_`
   - 如果实现允许，进一步演进为基于原子变量的只读采样函数，减少对 `mutex_` 的依赖

2. `flushDirectCommittedOffsetsIntoCommitStateLocked(...)`
   - 把临时视图写回 `committedOffsetByTopicPartition_`
   - 更新 `dirtyCommittedTopicPartitions_`
   - 这是状态推进动作，只允许在 owner/commit 主路径调用

第一性原理是：

```text
+-------------------------------+
| read path                     |
| must not mutate state         |
+-------------------------------+

+-------------------------------+
| commit path                   |
| may mutate commit state       |
+-------------------------------+
```

也就是说，当前名为 `syncDirectCommittedOffsetsLocked()` 的函数混合了“采样”和“推进状态”两种职责，
这正是根因之一。

### 7.2 第二步：把 flush 放回正确路径

以下路径可以调用 `flushDirectCommittedOffsetsIntoCommitStateLocked(...)`：

- `maybeEnqueueDirectPeriodicCommit()`
- `snapshotState()`
- revoke / shutdown 前的 final commit
- 显式 commit 触发路径

这样 direct ack 与 commit state 的同步，就回到真正的消费主路径。

推荐主路径图：

```text
+------------------+
| applyDirectAck() |
+--------+---------+
         |
         v
+-------------------------------+
| reconcileAckedOffsetsForCommit|
+---------------+---------------+
                |
                v
+-------------------------------+
| buildCommitOffsetRequest      |
| Locked()                      |
+---------------+---------------+
                |
                v
+-------------------------------+
| executeCommitOffsetRequest()  |
+-------------------------------+
```

### 7.3 第三步：让 `buildRuntimeInfo()` 变成纯读

`buildRuntimeInfo()` 只能：

- 读取当前 commit state
- 读取 direct slot 原子变量做展示
- 读取 lag snapshot 或基于只读数据计算 lag 视图

但不能：

- 写 `committedOffsetByTopicPartition_`
- 写 `dirtyCommittedTopicPartitions_`
- 隐式触发 commit 调度

如果 metrics 需要展示“最新 acked offset”，应当通过纯读 slot 视图计算，不应通过
“先写回 commit state 再展示”的方式实现。

这里要特别强调：

- 为了观测“更实时”，不能再把 `buildRuntimeInfo()` 变成一个会改状态的函数。
- 为了观测“更方便”，也不应该把更多 commit 相关锁塞进 UDS 路径。
- 如果现有数据结构不适合只读观测，应当改造状态结构本身，而不是让观测接口承担写路径职责。

### 7.4 第四步：明确 owner 线程边界

建议把“推进 commit 状态”的动作统一限制在 owner 线程语义范围内，避免后续再次出现
“某个 debug 接口顺手推进了生产状态”的问题。

可以引入一个明确命名的辅助函数，比如：

```text
reconcileAckedOffsetsForCommitLocked()
```

语义上明确它是“为 commit 服务的状态归并”，而不是“普通信息采样”。

### 7.5 第五步：补回归测试

需要新增一个针对本 bug 的回归测试 / harness，最少覆盖下面两个场景：

1. 不访问 UDS metrics
   - direct ack 持续推进
   - periodic commit 仍能按 `commitIntervalMs` 触发

2. 高频访问 UDS metrics
   - `buildRuntimeInfo()` 不改变 commit state
   - commit 行为与“不访问 UDS”保持一致

理想断言：

```text
with metrics polling    -> broker commit cadence == baseline
without metrics polling -> broker commit cadence == baseline
```

### 7.6 最终修复判据

如果修复是正确的，那么应当同时满足下面四个条件：

```text
+-------------------------------+
| condition 1                   |
| UDS /json is read-only        |
+-------------------------------+

+-------------------------------+
| condition 2                   |
| UDS reads atomic state first  |
+-------------------------------+

+-------------------------------+
| condition 3                   |
| commit state mutates only in  |
| owner / ack / commit path     |
+-------------------------------+

+-------------------------------+
| condition 4                   |
| metrics polling on/off does   |
| not change broker commit      |
+-------------------------------+
```

## 8. 修复后验证建议

修复完成后，建议继续用 eBPF 重跑下面两组验证：

### 8.1 无 metrics 访问

观察 `7511`：

- `ackBatchDirect` 持续增长
- `buildCommitOffsetRequestLocked` 周期性增长
- `executeCommitOffsetRequest` 周期性增长
- `buildRuntimeInfo` 为 `0` 时，commit 仍然推进

### 8.2 高频 metrics 访问

继续对 `/var/run/tide/worker_7510.sock` 高频 `GET /json`：

- `buildJsonResponse` 和 `buildRuntimeInfo` 仍可增长
- 但 commit cadence 不应因为 metrics 访问而改变
- `buildRuntimeInfo()` 不应再成为 commit 能否推进的必要条件

修复后期望图：

```text
Case A: no metrics polling
+-------------------------------+
| direct ack                     |
+---------------+---------------+
                |
                v
+-------------------------------+
| reconcileAckedOffsetsForCommit|
| + periodic commit             |
+---------------+---------------+
                |
                v
+-------------------------------+
| broker committed offset moves |
+-------------------------------+

Case B: with metrics polling
+-------------------------------+
| UDS /json polling             |
| read-only only                |
+-------------------------------+

+-------------------------------+
| direct ack                     |
+---------------+---------------+
                |
                v
+-------------------------------+
| reconcileAckedOffsetsForCommit|
| + periodic commit             |
+---------------+---------------+
                |
                v
+-------------------------------+
| broker committed offset moves |
+-------------------------------+
```

## 9. 一句话 Lesson

在 runtime / observability 设计中，**观测接口必须是纯读的**。  
一旦 debug/metrics 路径承担了生产状态推进职责，系统就会出现“被观测时正常，不被观测时出错”的
海森堡式故障。
