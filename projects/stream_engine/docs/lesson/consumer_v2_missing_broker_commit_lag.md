# Consumer V2 Missing Broker Commit Lag Lesson

## 背景

这次问题的表象是 Kafka 监控里 topic 有生产流量，但 consumer group 的 consumption
长时间为 0 或 lag 持续增长；同时 consumer_v2 自身 UDS metrics 显示 poll、dispatch、
ack 都在前进。

核心结论：

- Kafka 监控看的是 broker 上的 group committed offset。
- consumer_v2 内部 ack offset 前进，不等于 Kafka broker committed offset 前进。
- partition commit 是按 `group.id + topic + partition` 维度提交“下一条要读的 offset”。
- 漏提交或异步提交结果不可见，会让 Kafka 监控误判消费没有推进。

## Kafka Offset 口径

Kafka consumer 侧至少有三类 offset：

```text
+----------------------+---------------------------------------------+
| Offset               | Meaning                                     |
+----------------------+---------------------------------------------+
| highWatermarkOffset  | broker log end offset, represents produce   |
| ackedOffset          | consumer_v2 has processed up to next offset |
| brokerCommitted      | broker-stored group offset                  |
+----------------------+---------------------------------------------+
```

Lag 口径：

```text
brokerLag = highWatermarkOffset - brokerCommittedOffset
ackedLag  = highWatermarkOffset - ackedOffset
commitGap = ackedOffset - brokerCommittedOffset
```

Kafka UI 和 Kafka group 监控只知道 `brokerCommittedOffset`，不知道
consumer_v2 内部的 `ackedOffset`。

## Partition Commit 机制

Kafka commit 不是提交一个 consumer 的全局位置，而是提交一组 partition offset：

```text
+-------------------+
| group.id          |
+---------+---------+
          |
          v
+-------------------+       +-------------------+
| topic partition 0 | ----> | committed offset  |
+-------------------+       +-------------------+
| topic partition 1 | ----> | committed offset  |
+-------------------+       +-------------------+
| topic partition N | ----> | committed offset  |
+-------------------+       +-------------------+
```

提交对象是 `RdKafka::TopicPartition(topic, partition, nextOffset)`。

重要规则：

- commit offset 是“下一条要消费的 offset”，不是最后一条已处理消息的 offset。
- 只在 commit list 里的 partition 会更新 broker committed offset。
- 没进入 commit list 的 partition 保持旧 committed offset。
- `OFFSET_STORED`、`OFFSET_BEGINNING`、`OFFSET_END` 是起始位置策略，不是可直接作为
  broker committed offset 的具体数字。
- 项目代码只会提交 `offset >= 0` 的具体 offset。

## 项目旧版 Consumer 流程

### ClusterConsumerObj

源码路径：

- `src/source/kafka/consumer_obj.cpp`

读消息时推进内存 offset：

```text
+---------------------------+
| ReadMsg                   |
+-------------+-------------+
              |
              v
+---------------------------+
| msg->offset() + 1         |
| saved in m_partitionState |
+---------------------------+
```

关键语义：

- `ReadMsg()` 在 `!enable.auto.commit` 时把当前 partition 的 `m_offset` 更新为
  `msg->offset() + 1`。
- `SnapshotState()` 遍历 `m_partitionState`，跳过 `m_offset < 0` 的 partition。
- `SnapshotState()` 对剩余 partition 构造 `TopicPartition(topic, partition, offset)`。
- `SnapshotState()` 调用 `m_consumer->commitSync(commitPartitions)`。
- 提交成功后，把同一份 `m_partitionState` 写入 checkpoint state。

ASCII flow：

```text
+---------+      +-------------------+      +--------------------+
| consume | ---> | m_partitionState  | ---> | SnapshotState      |
+---------+      | offset = msg+1    |      | build commit list  |
                 +-------------------+      +---------+----------+
                                                     |
                                                     v
                                           +--------------------+
                                           | commitSync         |
                                           | broker group offset|
                                           +--------------------+
```

### AsyncClusterConsumerObj

源码路径：

- `src/source/kafka/async_consumer_obj.cpp`

异步版只是在消费线程和队列上不同，commit 语义与 `ClusterConsumerObj` 一致：

- `ReadMsg()` 推进 `m_partitionState[partition].m_offset = msg->offset() + 1`。
- `SnapshotState()` 构造 partition commit list。
- `SnapshotState()` 调用 `commitSync`。

### CheckpointKafkaSource

源码路径：

- `src/source/checkpoint_kafka_source.cpp`

该 source 是独立 checkpoint 存储语义，初始化时会对 Kafka group 做一次对齐：

```text
+----------------------+      +-------------------+
| checkpoint storage   | ---> | latest_offset     |
+----------------------+      +---------+---------+
                                        |
                                        v
+----------------------+      +-------------------+
| assign partition     | ---> | commitSync        |
+----------------------+      +---------+---------+
                                        |
                                        v
                              +-------------------+
                              | seek start offset |
                              +-------------------+
```

这里 `commitSync + seek` 的目的，是让 Kafka group 位点和外部 checkpoint 起点一致。

## Consumer V2 Commit 流程

源码路径：

- `src/source/kafka/consumer_v2/unified_consumer.cpp`

consumer_v2 的路径比旧版多了共享 consumer、dispatcher、worker ack：

```text
+------------------+
| poll thread      |
| KafkaConsumer    |
+--------+---------+
         |
         v
+------------------+
| MsgSlotRing      |
| DispatchState    |
+--------+---------+
         |
         v
+------------------+
| WorkerQueue      |
| downstream read  |
+--------+---------+
         |
         v
+------------------+
| ackBatch         |
| record offset+1  |
+--------+---------+
         |
         v
+------------------+
| commitOffsets    |
| broker commit    |
+------------------+
```

关键函数：

- `ackBatch()`：worker 释放 batch 后才调用。
- `recordCommittedOffsetLocked(partitionIndex, offset + 1)`：记录该 partition 已 ack 的下一条
  offset。
- `commitOffsetsLocked()`：构造按 KafkaConsumer handle 分组的 partition commit list。
- `partitionConsumers_[partitionIndex]`：记录每个 partition 属于哪个 KafkaConsumer handle。

consumer_v2 与旧版最大差异：

- 旧版在 `ReadMsg()` 阶段推进 offset，snapshot 时提交。
- consumer_v2 在 `ackBatch()` 阶段推进 offset，更接近“下游已经释放 payload 后再可提交”。
- consumer_v2 有多个 KafkaConsumer handle，所以 commit list 必须按 owning consumer 分组。

commit list 生成逻辑：

```text
+--------------------+
| committedOffsets_  |
| per partition      |
+---------+----------+
          |
          | only offset >= 0
          v
+--------------------+
| find partition     |
| owning consumer    |
+---------+----------+
          |
          v
+--------------------+
| consumer ->        |
| [TopicPartition]   |
+---------+----------+
          |
          v
+--------------------+
| commitSync         |
+--------------------+
```

注意：`committedOffsets_` 这个名字容易误导。它在 consumer_v2 中不是“broker 已提交
offset”，而是“内部已 ack、准备提交到 broker 的 next offset”。文档和 metrics 中要避免
把它和 broker committed offset 混淆。

## commitSync 与 commitAsync

### commitSync

`commitSync(partitions)` 的语义：

- 阻塞等待 broker offset commit 请求完成或失败。
- 返回值表示整体请求是否成功。
- 返回后可以检查每个 `TopicPartition::err()`，判断 partition 级别是否失败。
- 调用返回后再销毁 `TopicPartition` list 是安全、直观、可验证的。
- 适合 checkpoint、revoke、以及需要 Kafka monitoring 立即看到 offset 前进的路径。

```text
+-------------+       +-------------------+       +--------------------+
| build list  | ----> | commitSync        | ----> | broker committed   |
+-------------+       | wait response     |       | offset updated     |
                      +---------+---------+       +--------------------+
                                |
                                v
                      +-------------------+
                      | inspect err       |
                      | global + per part |
                      +-------------------+
```

### commitAsync

`commitAsync(partitions)` 的语义：

- 只表示提交请求被本地接受/入队，不表示 broker 已经提交成功。
- 真正结果通过 `offset_commit_cb` 异步返回。
- 如果没有配置 `offset_commit_cb`，就无法知道 partition 级 commit 成功、失败、延迟或乱序。
- librdkafka 测试 `0060-op_prio.cpp` 中使用 `commitAsync(msg)` 时，显式设置了
  `offset_commit_cb` 并等待 callback。
- `commitAsync` 适合“允许最终一致，并且有 callback/metrics 观测结果”的路径。

```text
+-------------+       +-------------------+
| build list  | ----> | commitAsync       |
+-------------+       | enqueue request   |
                      +---------+---------+
                                |
                                | later
                                v
                      +-------------------+
                      | offset_commit_cb  |
                      | real result       |
                      +-------------------+
```

### 二者关键区别

```text
+---------------------+----------------------+----------------------+
| Aspect              | commitSync           | commitAsync          |
+---------------------+----------------------+----------------------+
| Function return     | request completed    | request enqueued     |
| Broker visibility   | known after return   | unknown until cb     |
| Partition errors    | inspect after return | inspect in cb        |
| Ordering risk       | lower                | must handle ordering |
| Good for checkpoint | yes                  | only with cb/guard   |
| Good for monitoring | yes                  | only with cb/metrics |
+---------------------+----------------------+----------------------+
```

## 本次问题为什么会出现

问题不是“完全没有 commit 调用”。线上 UDS 已经看到：

- `commitIntervalMs=5000`
- `totalPeriodicCommitCalls` 持续增长
- `totalCommitFailures=0`

但对 `7510` 做 partition delta 采样时发现：

- 124 个 assigned partition 都有 lag row。
- 124 个 partition 都有 broker committed offset 和 acked offset。
- 12 秒窗口内，115 个 partition 的 `ackedOffset` 增长，但 `brokerCommittedOffset` 没有增长。
- `commitGap` 按 ack 速度继续扩大。

这说明调用层统计的是“commit 请求发出”，而 Kafka broker 监控看的是“group committed
offset 实际推进”。两者之间缺少确认。

错误模型：

```text
+------------------------+
| ackBatch advances      |
| committedOffsets_      |
+-----------+------------+
            |
            v
+------------------------+
| periodic commitAsync   |
| return == NO_ERROR     |
+-----------+------------+
            |
            | no offset_commit_cb
            | no per-partition result
            v
+------------------------+
| brokerCommittedOffset  |
| most partitions stale  |
+------------------------+
```

因此，把 `commitAsync` 的返回值当成“broker 已经提交成功”是错误理解。它只能说明
本地 API 接受了请求。

## 修复策略

consumer_v2 当前策略：

- `enable.auto.commit=false` 保持不变。
- `commit_interval_ms=5000` 默认开启周期 broker commit。
- 周期 broker commit 使用 `commitSync`。
- `commitSync` 返回后检查每个 `TopicPartition::err()`。
- 如果出现 partition 级失败，增加 `totalCommitFailures` 并写 `lastCommitError`。
- `commit_interval_ms=0` 可以显式关闭周期 broker commit，回到 checkpoint-only 语义。

修复后的期望：

```text
+------------------------+
| ackBatch advances      |
| committedOffsets_      |
+-----------+------------+
            |
            v
+------------------------+
| periodic commitSync    |
| wait broker response   |
+-----------+------------+
            |
            v
+------------------------+
| brokerCommittedOffset  |
| advances per partition |
+------------------------+
```

## 调试方法

使用 UDS `/json` 或 Prometheus 对齐三类 offset：

```text
+-------------------------------+--------------------------------+
| Metric                        | Expected after fix             |
+-------------------------------+--------------------------------+
| totalPeriodicCommitCalls      | increases every interval       |
| totalCommitFailures           | stays 0                        |
| lastCommitError               | empty                          |
| brokerCommittedRecordsPerSec  | close to ackedOffset rate      |
| commitGap                     | bounded, not linear growing    |
| partition brokerCommitted     | moves when ackedOffset moves   |
+-------------------------------+--------------------------------+
```

如果 Kafka UI 仍然有 lag，需要区分两类情况：

- `ackedLag` 也增长：真实处理能力追不上生产。
- `ackedLag` 稳定但 `brokerLag` 增长：commit 路径仍然有问题。

Partition 级排查：

```text
+----------------------------+-------------------------------+
| Observation                | Meaning                       |
+----------------------------+-------------------------------+
| acked delta > 0            | consumer_v2 processed records |
| broker committed delta = 0 | broker commit did not advance |
| commitGap grows            | commit visibility broken      |
| partition err non-empty    | per-partition commit failed   |
+----------------------------+-------------------------------+
```

## 预防

- 任何引入内部 ack state 的 source，都必须同时暴露 internal acked offset 与 broker
  committed offset。
- 不要用 `totalCommitCalls` 单独判断 commit 成功。
- 使用 `commitAsync` 必须配置并消费 `offset_commit_cb`，并暴露 callback 成功/失败指标。
- checkpoint/revoke/监控可见性强依赖路径优先用 `commitSync`。
- Dashboard 必须区分：
  - `brokerLag`
  - `ackedLag`
  - `commitGap`
  - `brokerCommittedRecordsPerSec`
- Kafka 有生产但 consumer consumption 为 0 时，优先验证 broker committed offset 是否前进。
