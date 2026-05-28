# Consumer V2 稳定 E2E A/B

## 1. 定位

用于验证 consumer_v2 性能改动是否真的有效，尤其是 async/sync commit、poll drain、dispatch
batch、worker lane 等容易受单次 run 波动影响的优化。

核心原则：

- 单次 e2e 只算 smoke，不用于性能结论。
- throughput run 不 attach eBPF / perf probe。
- tracing run 单独执行，只回答“哪里慢”，不回答“吞吐是多少”。
- A/B 使用同一个 topic，不同 group，避免重新 produce 和 offset 污染。
- 结论使用 median / MAD，不使用单次最高值。

## 2. Runner

脚本：

```bash
dev/kafka_e2e/run_consumer_v2_commit_stable_ab.sh
```

consume C batch/single 脚本：

```bash
dev/kafka_e2e/run_consumer_v2_batch_stable_ab.sh
```

consume C batch size sweep 脚本：

```bash
dev/kafka_e2e/run_consumer_v2_batch_size_sweep.sh
```

默认执行：

```text
warmup,async,sync,sync,async,async,sync,sync,async,async,sync
```

C batch runner 默认执行：

```text
warmup,c-batch,c-single,c-single,c-batch,c-batch,c-single,c-single,c-batch,c-batch,c-single
```

C batch size sweep 默认执行：

```text
warmup,256,512,1024,2048,4096,4096,2048,1024,512,256
```

C batch 对比含义：

| Variant | 实现 | 说明 |
|---|---|---|
| `c-batch` | `rd_kafka_queue_get_consumer` + `rd_kafka_consume_batch_queue` | owner 一次从 librdkafka consumer queue 取一批 `rd_kafka_message_t*` |
| `c-single` | 同一 C batch API，batch size = `1` | 控制变量，只对比批量取数本身 |

流程：

```text
+----------------------------------------------------+
| build binary before runner                          |
| bash dev/test_run.sh kafka_v2_test                  |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| runner starts Kafka once                            |
| produce topic once on first run                     |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| warmup run                                           |
| fresh group, result discarded from stats            |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| paired A/B runs                                      |
| same topic, fresh group per run                     |
+--------------------------+-------------------------+
                           |
                           v
+--------------------------+-------------------------+
| aggregate                                            |
| summary.json: median / MAD / validity               |
+----------------------------------------------------+
```

## 3. 参数

| Env | 默认 | 说明 |
|---|---:|---|
| `KAFKA_E2E_STABLE_RUN_ID` | `stable-commit-$(date +%s)` | 本轮稳定测试 id |
| `KAFKA_E2E_STABLE_TOPIC` | `tide-kafka-v2-${RUN_ID}` | A/B 共用 topic |
| `KAFKA_E2E_STABLE_COUNT` | `100000000` | produce records |
| `KAFKA_E2E_STABLE_ACK_TARGET` | `${COUNT}` | 每轮消费目标 |
| `KAFKA_E2E_STABLE_PARTITION_COUNT` | `125` | partition 数 |
| `KAFKA_E2E_STABLE_WORKER_COUNT` | `48` | worker 数 |
| `KAFKA_E2E_STABLE_COMMIT_INTERVAL_MS` | `1000` | commit A/B 的周期 |
| `KAFKA_E2E_STABLE_ORDER` | `warmup,async,sync,sync,async,async,sync,sync,async,async,sync` | paired order |
| `KAFKA_E2E_STABLE_SKIP_PRODUCE` | `0` | 复用已有 topic 时设为 `1` |
| `KAFKA_E2E_STABLE_REPORT_DIR` | `.dbg/stable-commit-ab-${RUN_ID}` | report 输出目录 |
| `KAFKA_E2E_STABLE_TEST_TIMEOUT_SEC` | `1800` | 单轮 gtest timeout |

## 4. 标准运行

完整稳定测试：

```bash
bash dev/test_run.sh kafka_v2_test

KAFKA_E2E_STABLE_COUNT=100000000 \
KAFKA_E2E_STABLE_ACK_TARGET=100000000 \
KAFKA_E2E_STABLE_COMMIT_INTERVAL_MS=1000 \
bash dev/kafka_e2e/run_consumer_v2_commit_stable_ab.sh
```

快速验证 runner / report：

```bash
KAFKA_E2E_STABLE_COUNT=30000000 \
KAFKA_E2E_STABLE_ACK_TARGET=30000000 \
KAFKA_E2E_STABLE_COMMIT_INTERVAL_MS=100 \
KAFKA_E2E_STABLE_ORDER=warmup,async,sync,sync,async \
bash dev/kafka_e2e/run_consumer_v2_commit_stable_ab.sh
```

C batch 快速验证：

```bash
KAFKA_E2E_STABLE_COUNT=30000000 \
KAFKA_E2E_STABLE_ACK_TARGET=30000000 \
KAFKA_E2E_STABLE_COMMIT_INTERVAL_MS=100 \
KAFKA_E2E_STABLE_ORDER=warmup,c-batch,c-single,c-single,c-batch \
bash dev/kafka_e2e/run_consumer_v2_batch_stable_ab.sh
```

C batch size sweep：

```bash
KAFKA_E2E_SWEEP_COUNT=30000000 \
KAFKA_E2E_SWEEP_ACK_TARGET=30000000 \
KAFKA_E2E_SWEEP_COMMIT_INTERVAL_MS=100 \
KAFKA_E2E_SWEEP_ORDER=warmup,256,512,1024,2048,4096,4096,2048,1024,512,256 \
bash dev/kafka_e2e/run_consumer_v2_batch_size_sweep.sh
```

复用已有 topic：

```bash
KAFKA_E2E_STABLE_TOPIC=tide-kafka-v2-existing-topic \
KAFKA_E2E_STABLE_SKIP_PRODUCE=1 \
KAFKA_E2E_STABLE_COUNT=30000000 \
KAFKA_E2E_STABLE_ACK_TARGET=30000000 \
bash dev/kafka_e2e/run_consumer_v2_commit_stable_ab.sh
```

## 5. 判定规则

每个 report 的 correctness gate：

| 字段 | 要求 |
|---|---|
| `ackedDuringPerf` | 等于 `KAFKA_E2E_STABLE_ACK_TARGET` |
| `totalCommitFailures` | `0` |
| `totalCommitCallbackFailures` | `0` |
| `totalPeriodicCommitCalls` | 与 `durationMs / commitIntervalMs` 大致匹配 |

聚合规则：

```text
deltaPct = (median(async) - median(sync)) / median(sync) * 100
thresholdPct = max(3%, 3 * max(MAD(async), MAD(sync)) / median(sync) * 100)

if correctness gate fails:
    result = invalid
elif abs(deltaPct) <= thresholdPct:
    result = neutral
elif deltaPct > thresholdPct:
    result = async_improvement
else:
    result = async_regression
```

## 6. 输出

runner 生成：

```text
.dbg/stable-commit-ab-${RUN_ID}/
  ${RUN_ID}-0-warmup.json
  ${RUN_ID}-1-async.json
  ${RUN_ID}-2-sync.json
  ...
  summary.json
```

`summary.json` 包含：

- per-run qps / duration / commit callback / validity。
- per-variant median / MAD / min / max。
- async vs sync comparison result。

## 7. 注意事项

- 不要在 runner 中 attach eBPF；需要 eBPF 时使用单独 tracing run。
- 不要把 warmup 纳入统计。
- 不要在 A/B 中间 rebuild binary。
- 不要在 A/B 中间重新 produce topic。
- 如果 `brokerCommittedCatchup=true`，该行只能作为 suspicious 结果，不应直接用于吞吐结论。
- 如果差异小于阈值，结论必须写 `neutral`，不能写“提升”。

## 8. 示例结果

快速 paired A/B 示例：

```bash
KAFKA_E2E_STABLE_RUN_ID=final-stable-commit-1779367941 \
KAFKA_E2E_STABLE_TOPIC=tide-kafka-v2-final-async-100-1779367309 \
KAFKA_E2E_STABLE_SKIP_PRODUCE=1 \
KAFKA_E2E_STABLE_COUNT=30000000 \
KAFKA_E2E_STABLE_ACK_TARGET=30000000 \
KAFKA_E2E_STABLE_PARTITION_COUNT=125 \
KAFKA_E2E_STABLE_WORKER_COUNT=48 \
KAFKA_E2E_STABLE_COMMIT_INTERVAL_MS=100 \
KAFKA_E2E_STABLE_ORDER=warmup,async,sync,sync,async \
bash dev/kafka_e2e/run_consumer_v2_commit_stable_ab.sh
```

`summary.json`：

| Variant | Runs | Median Msg/s | MAD | Min | Max | Result |
|---|---:|---:|---:|---:|---:|---|
| async | `2` | `1,619,390` | `570` | `1,618,820` | `1,619,960` | neutral |
| sync forced | `2` | `1,616,995` | `2,965` | `1,614,030` | `1,619,960` | neutral |

Comparison:

```text
deltaPct = +0.148%
thresholdPct = 3.000%
result = neutral
```

注意：该示例使用 `n=2` 和 `30M` records，只用于验证 runner 与回归风险；正式性能结论仍应使用默认 order 和更长窗口。

## 9. C Batch 示例结果

C-level consume batch 快速 paired A/B：

```bash
KAFKA_E2E_STABLE_RUN_ID=c-batch-stable-1779420054 \
KAFKA_E2E_STABLE_TOPIC=tide-kafka-v2-final-async-100-1779367309 \
KAFKA_E2E_STABLE_SKIP_PRODUCE=1 \
KAFKA_E2E_STABLE_COUNT=30000000 \
KAFKA_E2E_STABLE_ACK_TARGET=30000000 \
KAFKA_E2E_STABLE_PARTITION_COUNT=125 \
KAFKA_E2E_STABLE_WORKER_COUNT=48 \
KAFKA_E2E_STABLE_COMMIT_INTERVAL_MS=100 \
KAFKA_E2E_STABLE_POLL_DRAIN_BATCH_SIZE=1024 \
KAFKA_E2E_STABLE_ORDER=warmup,c-batch,c-single,c-single,c-batch \
bash dev/kafka_e2e/run_consumer_v2_batch_stable_ab.sh
```

`summary.json`：

| Variant | Runs | Median Msg/s | MAD | Result |
|---|---:|---:|---:|---|
| c-batch | `2` | `1,616,470` | `2,090` | batch_improvement |
| c-single | `2` | `193,174` | `4,778` | baseline |

Comparison:

```text
deltaPct = +736.795%
thresholdPct = 7.420%
result = batch_improvement
```

注意：该示例对比的是同一 C API 下的 `batch size=1024` 与 `batch size=1`，不是 C++ wrapper 对比。正式结论仍应使用默认 order 和更长窗口。

## 10. C Batch Size Sweep 示例结果

Quick sweep：

| Batch Size | Runs | Median Msg/s | MAD | Avg Poll Batch | Max Poll Batch |
|---:|---:|---:|---:|---:|---:|
| 256 | `2` | `1,602,965` | `9,765` | `251.31` | `256` |
| 512 | `2` | `1,610,825` | `1,295` | `495.37` | `512` |
| 1024 | `2` | `1,610,910` | `170` | `956.82` | `1024` |
| 2048 | `4` | `1,618,570` | `4,535` | `1,783.99` | `2048` |
| 4096 | `4` | `1,615,075` | `6,425` | `3,182.62` | `4096` |
| 8192 | `2` | `1,614,040` | `3,820` | `5,296.66` | `8192` |

Decision:

```text
default pollDrainBatchSize = 2048
```

原因：

- `2048` 合并 median 最高。
- `4096/8192` 没有显著超过 `2048`。
- `2048` 的 owner tile 小于 `4096/8192`，对 pause/resume、control lane、commit lane 的响应风险更小。
