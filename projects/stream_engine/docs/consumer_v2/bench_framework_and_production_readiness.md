# Consumer V2 Bench Framework and Production Readiness

## 目标状态

`consumer_v2` 当前性能目标已经达到：

| 目标 | 结果 | 证据 |
|------|------|------|
| 原始基线 | `215,077 msg/s` | 真实 `billion backlog` e2e + perf |
| 5x 目标线 | `~1,075,385 msg/s` | `215,077 * 5` |
| 当前 perf 口径高水位 | `1.08492e+06 msg/s` | `real e2e + perf record` 同窗采样 |

这说明性能优化阶段已经越过 5x 门槛。下一阶段不再以吞吐冲高为主，而是确认 production readiness：

1. 是否存在内存泄漏或长期 RSS 单调增长
2. 是否能 graceful quit，且退出后线程、socket、slot、consumer 都干净释放
3. dispatch / poll thread 扩缩容是否在 real e2e 下符合预期
4. HTML / JSON / Prometheus 观测面是否能真实反映运行态

只有以上主题都有 real e2e 结论或数据支撑，`consumer_v2` 才能判定为 production ready。

## Bench 原则

所有正式结论必须来自 real e2e。synthetic bench、短窗口冲高、单测和代码静态推理只能用于定位问题，不能作为 production readiness 结论。

### 固定口径

| 项目 | 规范 |
|------|------|
| Kafka | 使用真实 Redpanda / Kafka broker |
| Topic | 复用固定 `billion backlog` topic |
| Partition | 固定 `100` partitions |
| Ack target | 固定 `30,000,000` |
| Payload | 使用真实 payload 读取和 ack，不跳过公共 API |
| Backpressure | 必须观察到 `maxPausedPartitions >= 1` |
| Report | 每轮生成 JSON report |
| Perf | 性能结论必须用同一轮 e2e 同窗 `perf record` |
| Sweep | 环境变量 sweep 必须串行，禁止多个 e2e 共享同一 topic/group 并发跑 |

### 禁止口径

| 禁止项 | 原因 |
|--------|------|
| 2-3 秒短窗口吞吐 | 容易把启动冲高误判为稳态 |
| synthetic preload | 无法覆盖真实 poll、payload、ack、commit、pause/resume |
| 单独跑 perf、不生成 report | 无法把热点和吞吐指标对齐 |
| report 缺少 avg batch | 无法判断吞吐变化来自 batch 碎裂还是 CPU 成本 |
| topic/group 漂移 | 会引入 rebalance、backlog 深度、broker 状态差异 |

## Bench 环境

### 构建

```bash
cd /root/Documents/stream_engine
export DISABLE_CAS=1
bash dev/test_build.sh kafka_v2_test
```

### Bundle 运行目录

`kafka_v2_test` 使用 bundle lib，相对 interpreter 是 `./lib/ld-linux-x86-64.so.2`，所以正式运行目录必须是：

```bash
cd /data00/home/sunmingqiang/Documents/stream_engine/build64_release/src/test
```

不要从仓库根目录直接执行绝对路径二进制，否则可能因为相对 interpreter 找不到而报 `No such file or directory`。

### 启动 Kafka

```bash
cd /root/Documents/stream_engine
bash dev/kafka_e2e/start.sh
```

## Real E2E Bench

### 环境变量

```bash
cd /root/Documents/stream_engine

export DISABLE_CAS=1
export TIDE_KAFKA_V2_BACKPRESSURE_E2E=1
export TIDE_KAFKA_V2_LARGE_SCALE_E2E=1
export TIDE_KAFKA_V2_BACKPRESSURE_COUNT=1000000000
export TIDE_KAFKA_V2_BACKPRESSURE_TOPIC=tide-kafka-v2-billion-1778472647-1009376
export TIDE_KAFKA_V2_BACKPRESSURE_BROKERS=127.0.0.1:9092
export TIDE_KAFKA_CONSUMER_V2_SOCKET_PATH=/tmp/tide-kafka-v2-large-scale/worker_real_bench.sock
export TIDE_KAFKA_V2_LARGE_SCALE_ACK_TARGET=30000000
export TIDE_KAFKA_V2_LARGE_SCALE_REPORT_PATH=/root/Documents/stream_engine/.dbg/billion-backpressure-report-manual.json
export TIDE_KAFKA_V2_LARGE_SCALE_PARTITION_COUNT=100
export TIDE_KAFKA_V2_LARGE_SCALE_CONSUME_TIMEOUT_SEC=1800
export TIDE_KAFKA_V2_BACKPRESSURE_READ_SLEEP_MS=0
export TIDE_KAFKA_V2_LARGE_SCALE_PERF_READ_SLEEP_MS=0
export TIDE_KAFKA_V2_E2E_PSM=${TIDE_KAFKA_V2_E2E_PSM:-${TIDE_ENGINE_PSM:-data.systi.tide}}
export TIDE_KAFKA_V2_E2E_OWNER=${TIDE_KAFKA_V2_E2E_OWNER:-huliang}
export TIDE_KAFKA_V2_E2E_TEAM=${TIDE_KAFKA_V2_E2E_TEAM:-data-ti-data}
```

### 执行

```bash
cd /data00/home/sunmingqiang/Documents/stream_engine/build64_release/src/test
timeout 7200 ./kafka_v2_test \
  --gtest_filter=UnifiedConsumerBackpressureE2eTest.BillionScaleHundredPartitionBacklogReportsRuntimeStabilityAndPerf
```

### 查看 Report

```bash
cat /root/Documents/stream_engine/.dbg/billion-backpressure-report-manual.json
```

## Real E2E + Perf

### 执行

```bash
cd /root/Documents/stream_engine

export DISABLE_CAS=1
export TIDE_KAFKA_V2_BACKPRESSURE_E2E=1
export TIDE_KAFKA_V2_LARGE_SCALE_E2E=1
export TIDE_KAFKA_V2_BACKPRESSURE_COUNT=1000000000
export TIDE_KAFKA_V2_BACKPRESSURE_TOPIC=tide-kafka-v2-billion-1778472647-1009376
export TIDE_KAFKA_V2_BACKPRESSURE_BROKERS=127.0.0.1:9092
export TIDE_KAFKA_CONSUMER_V2_SOCKET_PATH=/tmp/tide-kafka-v2-large-scale/worker_real_perf.sock
export TIDE_KAFKA_V2_LARGE_SCALE_ACK_TARGET=30000000
export TIDE_KAFKA_V2_LARGE_SCALE_REPORT_PATH=/root/Documents/stream_engine/.dbg/billion-backpressure-report-perf.json
export TIDE_KAFKA_V2_LARGE_SCALE_PARTITION_COUNT=100
export TIDE_KAFKA_V2_LARGE_SCALE_CONSUME_TIMEOUT_SEC=1800
export TIDE_KAFKA_V2_BACKPRESSURE_READ_SLEEP_MS=0
export TIDE_KAFKA_V2_LARGE_SCALE_PERF_READ_SLEEP_MS=0
export TIDE_KAFKA_V2_E2E_PSM=${TIDE_KAFKA_V2_E2E_PSM:-${TIDE_ENGINE_PSM:-data.systi.tide}}
export TIDE_KAFKA_V2_E2E_OWNER=${TIDE_KAFKA_V2_E2E_OWNER:-huliang}
export TIDE_KAFKA_V2_E2E_TEAM=${TIDE_KAFKA_V2_E2E_TEAM:-data-ti-data}

cd /data00/home/sunmingqiang/Documents/stream_engine/build64_release/src/test
timeout 7200 perf record -F 99 -g --call-graph dwarf \
  -o /root/Documents/stream_engine/.dbg/perf-e2e-real.data \
  ./kafka_v2_test \
  --gtest_filter=UnifiedConsumerBackpressureE2eTest.BillionScaleHundredPartitionBacklogReportsRuntimeStabilityAndPerf
```

### perf report

```bash
cd /root/Documents/stream_engine
perf report --stdio --sort symbol -i .dbg/perf-e2e-real.data | head -n 120
```

### perf script

```bash
cd /root/Documents/stream_engine
perf script -i .dbg/perf-e2e-real.data > .dbg/perf-e2e-real.unfold
```

## Report 字段

正式 report 至少要包含以下字段：

| 字段 | 判定意义 |
|------|----------|
| `ackedMsgsPerSec` | 稳态吞吐主指标 |
| `durationMs` | 防止短窗口假高 |
| `ackedDuringPerf` | 对齐 perf 采样窗口 |
| `maxPausedPartitions` | 证明 backpressure 真实触发 |
| `maxBufferedRecords` | 判断 buffer/ring 压力 |
| `totalPolledRecords` | poll 入口进度 |
| `totalAckedRecords` | ack 出口进度 |
| `totalDispatchedBatches` | dispatch batch 数 |
| `totalDispatchedRecords` | dispatch record 数 |
| `totalReadBatches` | worker read batch 数 |
| `totalReadRecords` | worker read record 数 |
| `avgDispatchBatchSize` | dispatch 是否打碎 batch |
| `avgReadBatchSize` | read path 是否打碎 batch |
| `currentThroughputMsgsPerSec` | runtime 实时吞吐 |
| `lastError` | runtime 错误状态 |

## 当前性能结论

| 阶段 | Acked Msg/s | Avg Read Batch | 说明 |
|------|-------------|----------------|------|
| 初始 real e2e + perf 基线 | `215,077` | `332.203` | 5x 计算基线 |
| poll drain `128 + perf` | `983,133` | `486.481` | cache tile 收敛 |
| live range `+ perf` | `983,906` | `501.040` | offset lifecycle range 化 |
| dispatch cap `640 + perf` | `1.00429e+06` | `490.664` | dispatch/backpressure 参数收敛 |
| message allocation lock boundary `+ perf` | `1.08492e+06` | `482.628` | 首次 perf 口径超过 5x |

当前性能目标已经达到。后续优化只允许服务 production readiness 或明显降低风险，不再为了局部吞吐继续大改。

## Production Readiness Gate

### 总体门禁

| 主题 | 当前状态 | 还缺什么 |
|------|----------|----------|
| 性能 5x | 已达成 | 保留复跑 report |
| 内存泄漏 | 有 stress / debug 指标基础 | 需要 real e2e 长窗口 RSS / ring / buffer 结论 |
| graceful quit | 有 close / resetForTest 路径 | 需要 real e2e 退出后线程、socket、slot、consumer 清理结论 |
| dispatch 扩缩容 | 有 auto-scale 状态与单测 | 需要 real e2e scale out / scale in 数据 |
| HTML 观测 | 代码支持 `/`、`/json`、`/prometheus`，e2e 使用 `/json` | 需要 real e2e 验证 HTML 页面字段与 `/json` 一致 |

### 1. 内存泄漏

#### 现有证据

| 证据 | 状态 |
|------|------|
| `RuntimeInfo.ringLiveCount` | 已暴露 |
| `RuntimeInfo.ringFreeSlotCount` | 已暴露 |
| `RuntimeInfo.totalBufferedRecordCount` | 已暴露 |
| large-scale debug event `vmRSSKb` | 已采样 |
| stress test ring leak threshold | 已有非 real Kafka 压测 |

#### Production-ready 结论要求

real e2e 至少跑一个长窗口，例如 `30,000,000` ack 或更长，采样以下指标：

| 指标 | 通过条件 |
|------|----------|
| `VmRSS` | 进入稳态后不持续单调增长 |
| `ringLiveCount` | 不持续接近 `ringCapacity` |
| `ringFreeSlotCount` | 不持续归零 |
| `totalBufferedRecordCount` | 不持续增长无回落 |
| `lastError` | 为空 |

建议把 `VmRSS`、`Threads`、`ringLiveCount`、`totalBufferedRecordCount` 写入正式 JSON report，而不只写入 debug event。

### 2. Graceful Quit

#### 现有证据

| 证据 | 状态 |
|------|------|
| `SharedConsumerState::close()` | 设置 `closed_` / `runtimeStopRequested_` 并 join poll threads |
| poll loop close | 执行 unsubscribe / unassign / consumer close |
| e2e `resetForTest` | 每轮结束会触发 close |
| `RuntimeInfo.closed` | 已暴露 |
| `RuntimeInfo.pollThreadExited` | 已暴露 |

#### Production-ready 结论要求

需要新增或扩展 real e2e 退出验证，结束后确认：

| 指标 | 通过条件 |
|------|----------|
| `closed` | `true` |
| `pollThreadExited` | `true` |
| `handleCount` | `0` 或 registry 已清理 |
| `workerCount` | `0` 或显式 unregister 后为 `0` |
| `ringLiveCount` | `0`，或 close 路径明确清理全部 slot |
| debug socket | 退出后不可连接，或 discovery 中不再暴露该 worker |
| process threads | 无 `rdkafka` / consumer poll thread 残留 |

建议 real e2e 在 `resetForTest` 后追加一次 `/proc/self/task` 线程名统计和 debug socket connect 检查。

### 3. Dispatch 扩缩容

#### 现有证据

| 证据 | 状态 |
|------|------|
| `autoScaleEnabled` | RuntimeInfo 已暴露 |
| `desiredHandleCount` | RuntimeInfo 已暴露 |
| `minHandleCount` / `maxHandleCount` | RuntimeInfo 已暴露 |
| `totalScaleOutEvents` / `totalScaleInEvents` | RuntimeInfo 已暴露 |
| `scaleOutBacklogThreshold` / `scaleInIdleThreshold` | RuntimeInfo 已暴露 |
| `UnifiedConsumerRuntimeStateTest.AutoScaleAdjustsDesiredHandleCountFromBacklog` | 单测覆盖 |

#### Production-ready 结论要求

需要 real Kafka e2e 覆盖两个阶段：

| 阶段 | 触发条件 | 通过条件 |
|------|----------|----------|
| scale out | backlog / active lease / buffered records 达到阈值 | `desiredHandleCount` 增长，`totalScaleOutEvents > 0`，线程数不超过预算 |
| scale in | backlog 降到 idle 阈值 | `desiredHandleCount` 回落，`totalScaleInEvents > 0`，吞吐无异常中断 |

必须同时记录：

| 指标 | 用途 |
|------|------|
| `desiredHandleCount` | 验证扩缩容目标 |
| `handleCount` | 验证真实 consumer 数 |
| `estimatedRdkThreadCount` | 验证线程预算 |
| `readyPartitionCount` | 验证调度压力 |
| `activeLeaseCount` | 验证 worker 占用 |
| `totalBufferedRecordCount` | 验证 backlog |
| `totalScaleOutEvents` / `totalScaleInEvents` | 验证决策发生 |

### 4. HTML / JSON / Prometheus 观测

#### 现有证据

| Endpoint | 状态 |
|----------|------|
| `/json` | real e2e 已查询并作为 backpressure 判断来源 |
| `/` | 代码生成 HTML dashboard，包含 auto refresh |
| `/prometheus` | 代码生成 text/plain metrics |

#### Production-ready 结论要求

需要 real e2e 明确验证：

| Endpoint | 通过条件 |
|----------|----------|
| `/json` | 返回非空 JSON，关键字段和 `RuntimeInfo` 一致 |
| `/` | 返回 `text/html`，包含 topic/group、partition、throughput、pause/resume、error 字段 |
| `/prometheus` | 返回 `text/plain`，指标可被 scrape，数值和 `/json` 同步 |

HTML 不是只要页面能打开就算通过。它必须能回答生产排障问题：

| 问题 | HTML 必须展示 |
|------|---------------|
| 当前是否在消费 | `running` / `subscribed` / `assignedPartitionCount` |
| 是否反压 | `pausedPartitionCount` / `totalBufferedRecordCount` |
| 是否卡住 | `lastPolledMs` / `lastReadMs` / `lastAckedMs` / throughput |
| 是否扩缩容 | `desiredHandleCount` / `handleCount` / scale events |
| 是否有错误 | `lastError` / consume errors |

## Production-ready 执行顺序

接下来按下面顺序推进：

1. 长窗口 memory e2e：补 RSS / ring / buffer report 字段，确认无持续增长
2. graceful quit e2e：确认 close 后线程、socket、ring、consumer 清理
3. auto-scale real e2e：构造 backlog 上升和下降，确认 scale out / scale in
4. observability e2e：验证 `/json`、`/`、`/prometheus` 与 RuntimeInfo 一致
5. 汇总 production-ready report：把四个主题的 real e2e report 和结论写入本文档

## Production-ready Smoke

本轮把 production-ready gate 接入 `BillionScaleHundredPartitionBacklogReportsRuntimeStabilityAndPerf`，同一轮 real Kafka
e2e 会采集并断言：

| 主题 | Smoke 结论 | 数据 |
|------|------------|------|
| backpressure | 通过 | `maxPausedPartitions=1`，`finalPausedPartitions=0` |
| graceful quit | 通过 | `gracefulQuitOk=true`，`socketClosedAfterReset=true` |
| dispatch auto-scale | 通过 | `totalScaleOutEvents=220`，`totalScaleInEvents=217`，暴露出过度 scale-in 会造成 consumer churn |
| HTML 观测 | 通过 | `htmlEndpointOk=true` |
| JSON 观测 | 通过 | `jsonEndpointOk=true` |
| Prometheus 观测 | 通过 | `prometheusEndpointOk=true` |
| ring / buffer | 通过 | `ringLiveCount=1580`，`ringFreeSlotCount=260564`，`maxBufferedRecords=1024` |

报告路径：

```bash
/root/Documents/stream_engine/.dbg/billion-production-ready-smoke.json
```

Smoke 使用 `TIDE_KAFKA_V2_LARGE_SCALE_ACK_TARGET=1000000`，主要用于验证验收矩阵和退出/观测/扩缩容行为是否闭环。由于窗口较短，
RSS 会包含 Kafka client、allocator arena、topic assignment 等初始化增长，不能用来最终判断长期内存泄漏。

## Production-ready Long Run

Smoke 后继续用正式 `30,000,000` ack 长窗口验收。第一次长窗口发现两个真实问题：

| 问题 | 现象 | 修复 |
|------|------|------|
| ring graceful quit 泄漏兜底不足 | close/reset 时 live slot 上的 message 可能未走 reclaim | `MsgSlotRing` 析构遍历 live slot 并调用 `DeleteMessageFn` |
| auto-scale scale-in 过热 | `totalScaleOutEvents=1981` / `totalScaleInEvents=1978`，反复创建/关闭 KafkaConsumer，RSS 高水位扩大且可能造成 late ack 失败 | scale-in 必须满足 backlog 低且 read path 空闲至少一个决策窗口，热路径只允许 scale-out |

修复后正式 30M report：

| 主题 | Long-run 结论 | 数据 |
|------|---------------|------|
| throughput | 通过 | `ackedDuringPerf=30,000,382`，`ackedMsgsPerSec=558,458` |
| backpressure | 通过 | `maxPausedPartitions=7`，`finalPausedPartitions=0` |
| memory | 通过 | `finalVmRSSKb=2,952,960`，`peakVmRSSKb=3,679,728`，`rssRetreatFromPeakKb=726,768`，低于 `4GB` RSS gate |
| ring / slot | 通过 | `ringLiveCount=2624`，`ringFreeSlotCount=259520`，`ringCapacity=262144` |
| graceful quit | 通过 | `gracefulQuitOk=true`，`socketClosedAfterReset=true` |
| dispatch auto-scale | 通过 | `totalScaleOutEvents=3`，`totalScaleInEvents=0`，持续 backlog 下保持稳定扩容不震荡 |
| HTML / JSON / Prometheus | 通过 | `htmlEndpointOk=true`，`jsonEndpointOk=true`，`prometheusEndpointOk=true` |
| error | 通过 | `lastError=""` |

报告路径：

```bash
/root/Documents/stream_engine/.dbg/billion-production-ready-long.json
```

## 当前判定

| 项目 | 判定 |
|------|------|
| 性能目标 | 已达到 |
| 代码路径 | focused tests、real e2e performance、production-ready smoke、30M long-run 均已覆盖 |
| 内存泄漏 | 通过。RSS 有 Kafka/allocator 高水位，但 ring/buffer 不泄漏，最终 RSS 低于 4GB gate 且从 peak 回落 |
| graceful quit | 通过。reset 后 worker/socket 清理闭环，ring 析构兜底释放 live message |
| dispatch 扩缩容 | 通过。持续 backlog 下稳定 scale-out，不再热路径 scale-in 震荡 |
| HTML / JSON / Prometheus 观测 | 通过 |
| production ready | 通过 |

当前 `consumer_v2` 已达到 production-ready 状态。后续只建议做面向可运维性的增强，例如把 RSS gate、peak 回落、scale-in idle 条件和 endpoint
健康检查接入 CI 或夜间 real e2e，不再作为上线阻塞项。
