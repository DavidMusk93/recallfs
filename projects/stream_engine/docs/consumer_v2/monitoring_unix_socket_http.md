# Consumer V2 Monitoring via Unix Domain Socket HTTP

## 背景

`consumer_v2` 的监控面不是单独起一个 TCP 端口，而是在本机创建一个 Unix domain socket，然后在这个 socket 上直接说 HTTP。

也就是说：

- 传输层是 Unix domain socket
- 应用层协议是 HTTP/1.1
- 不需要额外定义私有协议
- 浏览器、`curl`、Prometheus、排障脚本都可以复用 HTTP 语义

代码入口在 [unified_consumer.cpp](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L2530-L2590) 和 [dispatchRequest](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L2946-L2974)。

## Socket 路径

### 显式指定

优先使用环境变量 `TIDE_KAFKA_CONSUMER_V2_SOCKET_PATH`：

```bash
export TIDE_KAFKA_CONSUMER_V2_SOCKET_PATH=/tmp/tide-kafka-v2/worker.sock
```

### 指定目录

如果没有指定完整路径，可以只指定目录：

```bash
export TIDE_KAFKA_CONSUMER_V2_SOCKET_DIR=/tmp/tide-kafka-v2
```

此时默认生成：

```text
/tmp/tide-kafka-v2/worker_<port-or-pid>.sock
```

其中 `<port-or-pid>` 优先取以下环境变量之一，否则退回到当前进程 `pid`：

- `LISTEN_PORT0`
- `PORT0`
- `TIDE_DEBUG_SERVICE_PORT`

默认目录是：

```text
/var/run/tide
```

对应实现见 [resolveSocketPath](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L355-L385)。

### 权限与生命周期

- socket 文件权限为 `0660`
- 如果目录不存在，运行时会尝试自动创建
- 进程退出或 `resetForTest` 后会关闭监听并删除 socket 文件

对应实现见 [ensureStarted](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L2530-L2579) 和 [stop](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L2591-L2619)。

## Endpoint 一览

监控服务当前支持以下 GET endpoint：

- `/json`：机器可读 JSON，最适合脚本和自动化检查
- `/`：HTML dashboard，适合人工排障
- `/prometheus`：Prometheus text 格式
- `/cluster`：聚合当前 socket 目录下多个 worker socket 的 JSON 结果

实现见 [dispatchRequest](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L2957-L2973)。

## 最常用访问方式

### 1. 直接用 curl 访问 UDS 上的 HTTP

这是最简单、最推荐的方式。

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/json
```

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/
```

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/prometheus
```

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/cluster
```

注意：

- `localhost` 只是 HTTP URL 占位，不会真的走 TCP
- 真实连接目标由 `--unix-socket` 指定
- 当前服务只支持 `GET`

### 2. 用原始 HTTP 请求验证协议

如果你想确认这真的是 HTTP，而不是私有协议，可以直接发原始请求：

```bash
printf 'GET /json HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' \
  | socat - UNIX-CONNECT:/tmp/tide-kafka-v2/worker.sock
```

返回会包含标准 HTTP 响应头：

```text
HTTP/1.1 200 OK
Content-Type: application/json
Content-Length: ...
Connection: close
```

### 3. 用 jq 看重点字段

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/json | jq '.summary'
```

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/json \
  | jq '.consumers[] | {groupId, topics, running, handleCount, desiredHandleCount, workerCount, pausedPartitionCount, totalBufferedRecordCount, currentThroughputMsgsPerSec, lastError}'
```

## JSON 结果怎么读

`/json` 返回两层数据：

- `summary`：当前 worker 进程整体汇总
- `consumers`：每个 `UnifiedConsumer` 实例的明细

JSON 构造逻辑见 [buildJsonResponse](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L2628-L2780)。

### `summary` 常用字段

- `socketPath`：当前监听的 UDS 路径
- `sharedConsumerCount`：当前进程里共享 consumer 实例数
- `workerCount`：已注册 worker 数
- `assignedPartitionCount`：当前分配到的 partition 数
- `pausedPartitionCount`：当前被 backpressure 暂停的 partition 数
- `totalBufferedRecordCount`：dispatch 层累计积压
- `ringLiveCount` / `ringFreeSlotCount`：ring 使用情况
- `rdkThreadCount`：观察到的 rdkafka 线程数
- `estimatedRdkThreadCount`：按 handle 估算的线程预算
- `currentThroughputMsgsPerSec`：当前实时吞吐

### `consumers[]` 常用字段

- `cluster` / `groupId` / `topics`：消费实例身份
- `running` / `subscribed` / `closed`：生命周期状态
- `handleCount` / `desiredHandleCount`：当前 poll handle 数和期望值
- `workerCount`：当前下游 worker 数
- `assignedPartitionCount` / `pausedPartitionCount`：分配与反压状态
- `activeLeaseCount` / `readyPartitionCount`：调度压力状态
- `totalBufferedRecordCount`：缓存积压
- `totalPolledRecords` / `totalAckedRecords`：入口和出口累计量
- `currentThroughputMsgsPerSec`：实时吞吐
- `dispatcherQueueDepth`：dispatcher command queue 当前深度
- `totalDispatcherIngressCommands` / `totalDispatcherAckCommands` / `totalDispatcherDemandCommands`：dispatcher 三类命令累计量，可按采样窗口换算 per-sec
- `totalDispatcherLoopIterations` / `totalDispatcherLoopLatencyUs`：dispatcher owner 处理次数与累计耗时，可用于计算平均 loop latency
- `totalWorkerReadTimeouts` / `totalMailboxEmptyReads` / `totalPartialDispatchBatches`：worker 空读、超时和 partial dispatch 主动化效果
- `lastPolledMs` / `lastReadMs` / `lastAckedMs`：最近活跃时间
- `lastError`：最后一条运行时错误

## HTML Dashboard 怎么看

访问方式：

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/
```

如果要在浏览器里看，通常会先把 UDS 桥接成一个本地 TCP HTTP 端口，见后文“UDS 转普通 HTTP”。

HTML 页面特征：

- 页面标题是 `Tide Kafka Consumer Dashboard`
- 自动刷新间隔是 `1s`
- 首页上半区是汇总表，下半区是每个 consumer 的明细表

关键展示项见 [buildHtmlResponseBody](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L2841-L2888)：

- 汇总区：
  - `shared consumers`
  - `workers`
  - `rdk threads`
  - `estimated rdk`
  - `assigned`
  - `paused`
  - `buffered`
  - `throughput msg/s`
  - `ring live/free`
- consumer 明细区：
  - `cluster`
  - `group`
  - `topics`
  - `running`
  - `handles`
  - `scale events`
  - `workers`
  - `assigned`
  - `paused`
  - `buffered`
  - `polled`
  - `bytes`
  - `acked`
  - `pause/resume`
  - `last error`

## Prometheus 指标怎么接

访问方式：

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/prometheus
```

当前导出的指标见 [buildPrometheusResponseBody](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L2891-L2929)，包括：

- `tide_kafka_consumer_v2_shared_consumers`
- `tide_kafka_consumer_v2_workers`
- `tide_kafka_consumer_v2_rdk_threads`
- `tide_kafka_consumer_v2_estimated_rdk_threads`
- `tide_kafka_consumer_v2_paused_partitions`
- `tide_kafka_consumer_v2_buffered_records`
- `tide_kafka_consumer_v2_polled_records`
- `tide_kafka_consumer_v2_polled_bytes`
- `tide_kafka_consumer_v2_acked_records`
- `tide_kafka_consumer_v2_dispatched_batches`
- `tide_kafka_consumer_v2_dispatcher_queue_depth`
- `tide_kafka_consumer_v2_dispatcher_ingress_commands`
- `tide_kafka_consumer_v2_dispatcher_ack_commands`
- `tide_kafka_consumer_v2_dispatcher_demand_commands`
- `tide_kafka_consumer_v2_dispatcher_loop_iterations`
- `tide_kafka_consumer_v2_dispatcher_loop_latency_us`
- `tide_kafka_consumer_v2_worker_read_timeouts`
- `tide_kafka_consumer_v2_partial_dispatch_batches`
- `tide_kafka_consumer_v2_mailbox_empty_reads`
- `tide_kafka_consumer_v2_pause_calls`
- `tide_kafka_consumer_v2_resume_calls`
- `tide_kafka_consumer_v2_scale_out_events`
- `tide_kafka_consumer_v2_scale_in_events`
- `tide_kafka_consumer_v2_throughput_msgs_per_sec`

### Prometheus 抓取建议

Prometheus 原生更习惯抓 TCP HTTP endpoint，因此常见做法不是让 Prometheus 直接连 UDS，而是在本机桥接一个只读本地端口，例如：

```bash
socat TCP-LISTEN:19095,reuseaddr,fork UNIX-CONNECT:/tmp/tide-kafka-v2/worker.sock
```

然后让 Prometheus 抓：

```text
http://127.0.0.1:19095/prometheus
```

## `/cluster` 是干什么的

`/cluster` 不是 Kafka cluster 级别监控，而是“当前 socket 目录下的 worker socket 聚合视图”。

它会：

- 读取同目录下的 `discovery.json`
- 扫描 `worker_*.sock`
- 对其它 worker socket 发 `/json`
- 聚合成一个总 JSON

实现见 [buildClusterResponse](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L2783-L2838)。

对应 discovery 文件逻辑见：

- [writeDiscoveryFile](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L706-L719)
- [removeDiscoveryFile](file:///root/Documents/stream_engine/src/source/kafka/consumer_v2/unified_consumer.cpp#L721-L737)

### 适合场景

- 一个进程目录下有多个 worker socket
- 想从一个入口看所有 worker
- 不想自己枚举每个 `worker_*.sock`

## UDS 转普通 HTTP

`consumer_v2` 本身已经在讲 HTTP，所以“转 HTTP”本质上不是协议转换，而是：

- 从 `Unix domain socket + HTTP`
- 桥接为 `TCP socket + HTTP`

### 方案 1：socat

最简单：

```bash
socat TCP-LISTEN:18080,reuseaddr,fork UNIX-CONNECT:/tmp/tide-kafka-v2/worker.sock
```

之后就可以：

```bash
curl http://127.0.0.1:18080/json
curl http://127.0.0.1:18080/
curl http://127.0.0.1:18080/prometheus
curl http://127.0.0.1:18080/cluster
```

适合：

- 本机临时排障
- 给浏览器查看 HTML dashboard
- 给本机 agent / probe 抓取

### 方案 2：nginx 反向代理

如果你已经有 nginx，可以把本机 TCP 或已有入口代理到 UDS：

```nginx
server {
    listen 18080;
    server_name localhost;

    location / {
        proxy_pass http://unix:/tmp/tide-kafka-v2/worker.sock:/;
        proxy_set_header Host localhost;
    }
}
```

然后访问：

```bash
curl http://127.0.0.1:18080/json
```

适合：

- 希望接统一反向代理
- 想加鉴权、限流、审计
- 想给浏览器或 Prometheus 暴露标准 HTTP 地址

### 方案 3：systemd socket / sidecar

生产里如果你不想临时起 `socat`，可以：

- 用 sidecar 常驻桥接 UDS 到 `127.0.0.1:<port>`
- 或用 systemd 托管桥接进程

原则上只要桥接层不改写 HTTP body，`/json`、`/`、`/prometheus`、`/cluster` 都可直接透传。

## 典型排障手册

### 1. 看 consumer 是否活着

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/json \
  | jq '.consumers[] | {groupId, running, subscribed, closed, pollThreadExited, lastError}'
```

重点看：

- `running=true`
- `subscribed=true`
- `closed=false`
- `lastError=""`

### 2. 看是不是反压

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/json \
  | jq '.summary | {pausedPartitionCount, totalBufferedRecordCount, ringLiveCount, ringFreeSlotCount}'
```

判断要点：

- `pausedPartitionCount > 0`：正在触发 backpressure
- `totalBufferedRecordCount` 高且不回落：下游可能跟不上
- `ringFreeSlotCount` 接近 `0`：ring 快被打满

### 3. 看是否卡在 poll / read / ack

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/json \
  | jq '.consumers[] | {groupId, currentThroughputMsgsPerSec, lastPolledMs, lastReadMs, lastAckedMs, totalPolledRecords, totalAckedRecords}'
```

判断要点：

- `lastPolledMs` 很久没变：可能 poll 卡住
- `totalPolledRecords` 增、`totalAckedRecords` 不增：说明后段堵住
- `currentThroughputMsgsPerSec` 接近 `0`：消费基本停滞

### 4. 看 auto-scale 是否在工作

```bash
curl --unix-socket /tmp/tide-kafka-v2/worker.sock http://localhost/json \
  | jq '.consumers[] | {groupId, autoScaleEnabled, handleCount, desiredHandleCount, totalScaleOutEvents, totalScaleInEvents, activeLeaseCount, readyPartitionCount}'
```

判断要点：

- `desiredHandleCount > handleCount`：刚做出扩容决策，实际 handle 还在追平
- `totalScaleOutEvents` 持续增长：说明 backlog 还在顶
- `totalScaleInEvents` 频繁抖动：说明扩缩容可能过热

## 常见问题

### 1. 为什么浏览器不能直接打开 socket 文件

浏览器不会直接连 Unix socket 文件。你需要先桥接成 TCP HTTP，例如用 `socat` 或 nginx。

### 2. 为什么 `curl --unix-socket` 里还要写 `http://localhost/...`

因为 `curl` 仍然要构造一个合法 HTTP URL；真正连接目标由 `--unix-socket` 覆盖。

### 3. 为什么访问失败

优先检查：

- 进程是否真的启用了 `consumer_v2`
- `TIDE_KAFKA_CONSUMER_V2_SOCKET_PATH` 是否正确
- socket 文件是否存在
- 运行用户是否有 `0660` 访问权限
- 进程是否已经退出并清理了 socket

### 4. 为什么 `/cluster` 结果为空或不完整

优先检查：

- worker 是否都在同一个 socket 目录下
- `discovery.json` 是否存在
- 其它 worker socket 是否还能连接

## 推荐实践

- 自动化脚本优先用 `/json`
- 人工排障优先看 `/` 和 `/json`
- 指标采集优先用 `/prometheus`
- 多 worker 汇总优先用 `/cluster`
- 生产上尽量固定 `TIDE_KAFKA_CONSUMER_V2_SOCKET_PATH` 或 `TIDE_KAFKA_CONSUMER_V2_SOCKET_DIR`，避免排障时到处猜 socket 名称
- 如果要接浏览器或 Prometheus，建议显式桥接到 `127.0.0.1:<port>`，不要直接对外暴露
