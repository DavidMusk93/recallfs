# Consumer V2 Backpressure Test Report

## 1. 摘要

本报告记录 `consumer_v2` 在本地 Redpanda 环境下的百万级消息反压测试。

测试目标是验证:

- `consumer_v2` 能从真实 Kafka broker 消费大量 backlog 数据。
- 当单 partition backlog 超过高水位时,能通过 Unix socket `/json` 观测到反压指标。
- 慢消费 drain 后,反压状态能恢复。
- runtime 关闭阶段不会因为 KafkaConsumer 生命周期竞争而卡住。

最终结论:

- 百万级数据已构造并保留在本地 Redpanda topic 中。
- Unix socket 观测到 `pausedPartitionCount` 从 `0` 变为 `1`,确认反压触发。
- drain 后 `pausedPartitionCount` 回到 `0`,确认反压释放。
- 修复 KafkaConsumer 双 close 后,测试脚本退出码为 `0`。

## 2. 测试环境

| 项目 | 值 |
|---|---|
| 日期 | 2026-05-09 |
| 操作系统 | Linux |
| Kafka 环境 | `dev/kafka_e2e` Redpanda |
| Broker | `127.0.0.1:9092` |
| 观测接口 | Unix socket HTTP `/json` |
| 测试脚本 | `dev/kafka_e2e/run_backpressure_test.sh` |
| gtest 用例 | `UnifiedConsumerBackpressureE2eTest.MillionScaleBacklogTriggersAndReleasesBackpressure` |
| 追溯会话 | `million-backpressure` |
| 追溯日志 | `.dbg/trae-debug-log-million-backpressure.ndjson` |

## 3. 测试对象

本次覆盖的核心路径:

```text
Redpanda topic with 1,000,000 records
    |
    v
consumer_v2 runtime poll thread
    |
    v
DispatchState per-partition buffer
    |
    v
highWatermark=64 triggers local pause state
    |
    v
Unix socket /json exposes pausedPartitionCount
    |
    v
slow worker drain + ack
    |
    v
lowWatermark=16 releases local pause state
```

关键配置:

| 配置项 | 值 |
|---|---|
| `dispatchBatchSize` | `1` |
| `highWatermark` | `64` |
| `lowWatermark` | `16` |
| `ringCapacity` | `8192` |
| `configcenter.enable` | `false` |
| `auto.offset.reset` | `earliest` |
| `enableAutoCommit` | `false` |

## 4. 执行命令

本次最终复验复用了已经写入百万级数据的 topic,避免重复生产:

```bash
truncate -s 0 .dbg/trae-debug-log-million-backpressure.ndjson 2>/dev/null || true

TIDE_KAFKA_V2_BACKPRESSURE_DEBUG_RUN_ID=post-fix-check \
KAFKA_E2E_RUN_ID=1778336113-1341917 \
KAFKA_E2E_BACKPRESSURE_TOPIC=tide-kafka-v2-backpressure-1778336113-1341917 \
KAFKA_E2E_BACKPRESSURE_SKIP_PRODUCE=1 \
KAFKA_E2E_BACKPRESSURE_TEST_TIMEOUT_SEC=180 \
bash dev/kafka_e2e/run_backpressure_test.sh
```

完整灌数入口仍保留在脚本中:

```bash
KAFKA_E2E_BACKPRESSURE_COUNT=1000000 \
bash dev/kafka_e2e/run_backpressure_test.sh
```

## 5. 执行结果

脚本阶段输出确认:

```text
[backpressure] start local Kafka test environment
[backpressure] create single-partition topic tide-kafka-v2-backpressure-1778336113-1341917
[backpressure] skip bulk produce, reuse topic tide-kafka-v2-backpressure-1778336113-1341917
[backpressure] run consumer_v2 backpressure e2e, timeout=180s
[backpressure] gtest filter=UnifiedConsumerBackpressureE2eTest.MillionScaleBacklogTriggersAndReleasesBackpressure
```

gtest 结果:

```text
[ RUN      ] UnifiedConsumerBackpressureE2eTest.MillionScaleBacklogTriggersAndReleasesBackpressure
[       OK ] UnifiedConsumerBackpressureE2eTest.MillionScaleBacklogTriggersAndReleasesBackpressure (3194 ms)
[  PASSED  ] 1 test.
```

Blade 结果:

```text
Run 1 tests
All tests passed!
success
```

脚本退出码:

```text
0
```

## 6. 反压证据

最终 `post-fix-check` 追溯日志共 `10` 行。

关键事件:

| 阶段 | 指标 | 证据 |
|---|---|---|
| runtime 启动 | `running=true` | worker 已注册,runtime 开始运行 |
| 等待 assignment | `assignedPartitionCount=0` | 初始阶段还未完成 assignment |
| 反压触发 | `pausedPartitionCount=1` | attempt `43` 观测到 pause |
| 高水位 | `totalPolledRecords=65` | 超过 `highWatermark=64` |
| 慢消费中 | `totalAckedRecords=1/21/41` | worker 逐步 ack |
| 反压释放 | `pausedPartitionCount=0` | drain `48` 后恢复 |
| 最终状态 | `runtimeLastError=""` | 无 runtime 错误 |

原始关键日志:

```json
{"runId":"post-fix-check","hypothesisId":"B","msg":"[DEBUG] backpressure observed from socket","data":{"attempt":43,"pausedPartitionCount":1,"totalPolledRecords":65,"runtimeAssignedPartitionCount":1,"runtimeSubscribed":true,"runtimeRunning":true,"runtimeLastError":""}}
```

```json
{"runId":"post-fix-check","hypothesisId":"D","msg":"[DEBUG] backpressure released during drain","data":{"drain":48,"pausedPartitionCount":0,"batchSize":1,"totalAckedRecords":49,"totalPolledRecords":65,"runtimeLastError":""}}
```

```json
{"runId":"post-fix-check","hypothesisId":"D","msg":"[DEBUG] final backpressure state sampled","data":{"pausedPartitionCount":0,"totalAckedRecords":49,"totalPolledRecords":65,"runtimeLastError":""}}
```

## 7. 问题与修复

测试过程中暴露了一个 runtime 关闭问题。

现象:

- 反压已经触发并释放。
- 测试体已经到达 `final-state`。
- 进程仍然无法及时退出,最终被 `timeout 180` 杀掉。

追溯日志定位:

```text
consumer_v2 registry resetForTest begin
consumer_v2 close begin
consumer_v2 close external consumer close begin
consumer_v2 poll loop closing begin
consumer_v2 poll loop consumer close begin
consumer_v2 close external consumer close end
consumer_v2 close poll thread join begin
```

根因:

- 外部 `SharedConsumerState::close()` 调用了 `KafkaConsumer::close()`。
- poll 线程退出路径也调用了 `KafkaConsumer::close()`。
- 两个线程竞争关闭同一个 KafkaConsumer,导致主线程卡在 `pollThread.join()`。

修复:

- 外部 `close()` 只设置 `runtimeStopRequested_` 并等待 poll 线程退出。
- KafkaConsumer 的 `unsubscribe/unassign/close` 只由 poll 线程执行。
- registry reset 先取出 `states_` 并释放 registry 锁,再逐个关闭 runtime。

修复后 close 日志闭环:

```text
consumer_v2 close poll thread join begin
consumer_v2 poll loop closing begin
consumer_v2 poll loop consumer close begin
consumer_v2 poll loop consumer close end
consumer_v2 close poll thread join end
consumer_v2 close end
consumer_v2 registry resetForTest end
```

## 8. 可复现性

首次完整构造百万数据:

```bash
bash dev/kafka_e2e/run_backpressure_test.sh
```

复用已存在 topic 快速复验:

```bash
KAFKA_E2E_RUN_ID=1778336113-1341917 \
KAFKA_E2E_BACKPRESSURE_TOPIC=tide-kafka-v2-backpressure-1778336113-1341917 \
KAFKA_E2E_BACKPRESSURE_SKIP_PRODUCE=1 \
bash dev/kafka_e2e/run_backpressure_test.sh
```

检查追溯日志:

```bash
wc -l .dbg/trae-debug-log-million-backpressure.ndjson
tail -n 20 .dbg/trae-debug-log-million-backpressure.ndjson
```

预期结果:

- 脚本退出码为 `0`。
- gtest 输出 `[  PASSED  ] 1 test.`。
- `.dbg/trae-debug-log-million-backpressure.ndjson` 包含 `backpressure observed from socket`。
- 日志中存在 `pausedPartitionCount=1` 和后续 `pausedPartitionCount=0`。

## 9. 结论

本次测试证明:

- 本地 Redpanda 可承载 `consumer_v2` 百万级 backlog 测试。
- `consumer_v2` 能基于 per-partition buffer 高低水位触发和释放反压。
- Unix socket `/json` 可以作为反压观测面使用。
- 修复后的 KafkaConsumer 生命周期模型能稳定退出,不会在测试清理阶段卡住。

后续建议:

- 将 `pausedPartitionCount`、`totalPolledRecords`、`totalAckedRecords` 纳入长期 e2e 指标采样。
- 增加多 partition、多 worker、broker restart 期间反压保持性的扩展场景。
- 在非调试模式下保留必要的阶段日志,避免长时间运行时缺少可见进度。
