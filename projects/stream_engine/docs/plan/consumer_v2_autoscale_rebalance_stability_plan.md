# Consumer V2 Auto-scale Rebalance Stability Plan

## 目标

- 解释 `tide_worker` 当前吞吐低、CPU 不稳定的第一性根因
- 修复 `consumer_v2` auto-scale 直接增删 KafkaConsumer handle 导致的 rebalance 风暴
- 扩展 metrics，让后续可以从 `/var/run/tide/worker_$PORT.sock` 直接判断是否发生 handle churn
- 增加回归测试，避免后续再把 KafkaConsumer handle 当作普通线程池 worker 高频伸缩

## 运行时证据

### 观测入口

```text
+--------------------------------------+
| [port] 6511                          |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [pid] tide_worker                    |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [uds] /var/run/tide/worker_6510.sock |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [metrics] /json /prometheus          |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [logs] /proc/$pid/cwd/logs/          |
|        tide_worker.log               |
+--------------------------------------+
```

### `/json` 关键状态

```text
+---------------------------------------------+
| [consumer] bmq_data_sys / tlb_mirror_large  |
+---------------------------------------------+
| assignedPartitionCount = 0                  |
| pausedPartitionCount   = 0                  |
| activeLeaseCount       = 0                  |
| readyPartitionCount    = 0                  |
| workerQueueDepth       = 0                  |
| subscribed             = false              |
+---------------------------------------------+
                  |
                  v
+---------------------------------------------+
| [not dispatch/backpressure/ack bottleneck]  |
+---------------------------------------------+
```

### 事件计数

```text
+----------------------------------+
| [auto-scale]                     |
+----------------------------------+
| totalScaleOutEvents = 23         |
| totalScaleInEvents  = 23         |
+----------------------------------+
                  |
                  v
+----------------------------------+
| [rebalance]                      |
+----------------------------------+
| totalRebalanceCallbacks = 58     |
| totalAssignedPartitions = 2822   |
| totalRevokedPartitions = 2822    |
+----------------------------------+
```

### 日志周期

```text
+--------------------------------------+
| [1] partitions assigned              |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [2] poll loop start                  |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [3] consume failed                   |
| Unknown topic or partition           |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [4] partitions revoked               |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [5] poll loop consumer close         |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [6] about 60s later repeat           |
+--------------------------------------+
```

## 第一性根因

KafkaConsumer handle 不是普通线程池 worker。

```text
+--------------------------------------+
| [ordinary thread pool worker]        |
+--------------------------------------+
| add/remove worker                    |
| -> local scheduling cost only        |
+--------------------------------------+

+--------------------------------------+
| [KafkaConsumer handle]               |
+--------------------------------------+
| add/remove handle                    |
| -> consumer group membership change  |
| -> rebalance                         |
| -> assign/revoke all affected        |
| -> buffered/in-flight state churn    |
| -> librdkafka broker threads churn   |
+--------------------------------------+
```

当前 auto-scale 使用本地 backlog 作为伸缩信号：

```text
+--------------------------------------+
| [local backlog >= threshold]         |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| scale out: desiredHandleCount++      |
| create another KafkaConsumer         |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| consumer group rebalance             |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [local backlog drains to 0]          |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| scale in: desiredHandleCount--       |
| close one KafkaConsumer              |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| consumer group rebalance again       |
+--------------------------------------+
```

本地 backlog 是短周期缓存水位，不等价于 Kafka group 的稳定容量需求。用它直接驱动 group member 增删，会把正常的 batch 波动放大成 rebalance 风暴。

## 修复原则

1. KafkaConsumer handle 数量必须偏稳定，不能按本地瞬时 backlog 高频 scale-in。
2. scale-out 可以作为应急扩容，但 active stream 下 scale-in 必须被强阻尼，避免 `out -> in -> out` 震荡。
3. scale-in 的依据必须包含“上游 poll 已经长期 idle”，不能只看 worker read idle 或本地 backlog 为 0。
4. metrics 必须暴露 auto-scale 当前 backlog、被 active stream 阻止的 scale-in 次数、poll thread 启停次数。

## 设计

### Scale-in Gate

```text
+--------------------------------------+
| [candidate scale-in]                 |
| backlog <= scaleInIdleThreshold      |
| desiredHandleCount > minHandleCount  |
| worker read idle enough              |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| [new gate] upstream poll idle enough |
| lastPolledMs older than hold window  |
+--------------------------------------+
          | yes                         | no
          v                             v
+-----------------------------+   +----------------------------------+
| perform scale-in            |   | keep handle count stable         |
| close KafkaConsumer handle  |   | count scaleInBlockedByActive     |
+-----------------------------+   +----------------------------------+
```

### Metrics

新增 `/json` 字段：

```text
+----------------------------------------------+
| autoScaleBacklog                              |
| totalScaleInBlockedByActiveStreamEvents       |
| totalPollThreadStarts                         |
| totalPollThreadStops                          |
+----------------------------------------------+
```

新增 `/prometheus` 指标：

```text
+----------------------------------------------------------+
| tide_kafka_consumer_v2_auto_scale_backlog gauge          |
| tide_kafka_consumer_v2_scale_in_blocked_active_stream_events counter |
| tide_kafka_consumer_v2_poll_thread_starts counter        |
| tide_kafka_consumer_v2_poll_thread_stops counter         |
+----------------------------------------------------------+
```

## 验证计划

```text
+--------------------------------------+
| [unit] auto-scale active stream gate |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| backlog drains but lastPolled recent |
| -> desiredHandleCount stays high     |
| -> blocked counter increments        |
+--------------------------------------+

+--------------------------------------+
| [unit] metrics exposure              |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| /json and /prometheus include fields |
+--------------------------------------+

+--------------------------------------+
| [build/test] kafka_v2_test           |
+--------------------------------------+
                  |
                  v
+--------------------------------------+
| focused tests pass                   |
+--------------------------------------+
```

## 风险

- scale-out 后 active stream 下不会快速 scale-in，可能保留更多 KafkaConsumer handles。
- 这是有意取舍：稳定 consumer group membership 优先于用本地 backlog 高频回收 handle。
- 线程预算仍由 `autoScaleMaxPollThreadCount` 和 `maxRdkThreadCount` 限制。

## 回滚

- 如果线上需要临时回滚行为，可将 `tide.kafka.consumer_v2.auto_scale_max_poll_thread_count` 设为和 min 相同，固定 handle 数。
- 或关闭 `tide.kafka.consumer_v2.auto_scale_enable`。
