# HTTPD TopicStats 排查辅助文档

## 1. 原始输入

下面保留本次排查的原始输入，后续分析均以这组现象为背景：

```text
[2026-04-28 22:28:21.682+08:00] [info] [1855129|1855396] [src/source/httpd_source.cpp:1222] jobId:c2300b98-f212-4ead-bfe9-6088e2c49d56 taskId:4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 name:httpd raw_inmsgs:0 inmsgs:0, inbytes:0, available_queue_len:64
[2026-04-28 22:28:31.289+08:00] [info] [1855129|1855396] [src/source/httpd_source.cpp:1222] jobId:c2300b98-f212-4ead-bfe9-6088e2c49d56 taskId:4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 name:httpd raw_inmsgs:0 inmsgs:0, inbytes:0, available_queue_len:64
[2026-04-28 22:28:41.296+08:00] [info] [1855129|1855396] [src/source/httpd_source.cpp:1222] jobId:c2300b98-f212-4ead-bfe9-6088e2c49d56 taskId:4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 name:httpd raw_inmsgs:0 inmsgs:0, inbytes:0, available_queue_len:64
[2026-04-28 22:28:51.303+08:00] [info] [1855129|1855396] [src/source/httpd_source.cpp:1222] jobId:c2300b98-f212-4ead-bfe9-6088e2c49d56 taskId:4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 name:httpd raw_inmsgs:0 inmsgs:0, inbytes:0, available_queue_len:64
[2026-04-28 22:29:01.309+08:00] [info] [1855129|1855396] [src/source/httpd_source.cpp:1222] jobId:c2300b98-f212-4ead-bfe9-6088e2c49d56 taskId:4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 name:httpd raw_inmsgs:0 inmsgs:0, inbytes:0, available_queue_len:64
[2026-04-28 22:29:11.316+08:00] [info] [1855129|1855396] [src/source/httpd_source.cpp:1222] jobId:c2300b98-f212-4ead-bfe9-6088e2c49d56 taskId:4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 name:httpd raw_inmsgs:0 inmsgs:0, inbytes:0, available_queue_len:64
[2026-04-28 22:29:21.724+08:00] [info] [1855129|1855396] [src/source/httpd_source.cpp:1222] jobId:c2300b98-f212-4ead-bfe9-6088e2c49d56 taskId:4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 name:httpd raw_inmsgs:0 inmsgs:0, inbytes:0, available_queue_len:64
(base) root@g340-cd50-2000-601-985e-d96b-7da0:/proc/1855129/cwd/logs# grep -a c2300b98-f212-4ead-bfe9-6088e2c49d56 tide_worker.log

http 队列长队可用，但调度侧输出一直是 0:
curl localhost:6789/stable/topicstats?topic=doubao-trace-TUDP.pb
{"job_res_tg_stats":{"c2300b98-f212-4ead-bfe9-6088e2c49d56":{"res_tg_states":{"groupid:19337674-459d-4f85-ac88-10b2017e7416;resourceid:[2605:340:cd50:2000:6bca:bc8d:ffb:724c]:6510":{"num_available":0,"avalive_queue_length":0,"total_queue_length":0,"rows_per_sec":0.0},"groupid:19337674-459d-4f85-ac88-10b2017e7416;resourceid:[2605:340:cd50:f0a:6c5e:6ecf:8a78:8001]:6510":{"num_available":0,"avalive_queue_length":0,"total_queue_length":0,"rows_per_sec":0.0},"groupid:19337674-459d-4f85-ac88-10b2017e7416;resourceid:[2605:340:cd50:2000:601:985e:d96b:7da0]:6510":{"num_available":0,"avalive_queue_length":0,"total_queue_length":0,"rows_per_sec":0.0},"groupid:19337674-459d-4f85-ac88-10b2017e7416;resourceid:[2605:340:cd50:2000:601:985e:d96b:7da0]:7510":{"num_available":0,"avalive_queue_length":0,"total_queue_length":0,"rows_per_sec":0.0},"groupid:19337674-459d-4f85-ac88-10b2017e7416;resourceid:[2605:340:cd50:2000:6bca:bc8d:ffb:724c]:7510":{"num_available":0,"avalive_queue_length":0,"total_queue_length":0,"rows_per_sec":0.0},"groupid:19337674-459d-4f85-ac88-10b2017e7416;resourceid:[2605:340:cd50:f0a:3be7:6ab1:4b77:4cf6]:6510":{"num_available":0,"avalive_queue_length":0,"total_queue_length":0,"rows_per_sec":0.0}}}},"code":0,"message":"success"}

调度代码位置：/root/Documents/flow-scheduler
```

## 2. 补充输入

后续又补充到了下面这组现场输入，说明 `tm` 侧心跳和 statsreport 已经在周期上报：

```text
time=2026-04-28T22:42:25.477+08:00 thread_id=1855396 file=src/runtime/taskmanager/task_builder.h:95 level=info name=flowschedHearbeat taskId=4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 loopNo=1880
time=2026-04-28T22:42:25.477+08:00 thread_id=1855396 file=src/runtime/taskmanager/task_builder.h:118 level=info name=flowschedStatsReport taskId=4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 loopNo=1880 availQueueLen=64 queueLenCapacity=64
time=2026-04-28T22:43:09.506+08:00 thread_id=1855396 file=src/runtime/taskmanager/task_builder.h:95 level=info name=flowschedHearbeat taskId=4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 loopNo=1890
time=2026-04-28T22:43:09.506+08:00 thread_id=1855396 file=src/runtime/taskmanager/task_builder.h:118 level=info name=flowschedStatsReport taskId=4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 loopNo=1890 availQueueLen=64 queueLenCapacity=64
(base) root@g340-cd50-2000-601-985e-d96b-7da0:/proc/1855129/cwd/logs/taskmanager# ip addr
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN group default qlen 1000
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
    inet 127.0.0.1/8 scope host lo
       valid_lft forever preferred_lft forever
    inet6 ::1/128 scope host
       valid_lft forever preferred_lft forever
2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP group default qlen 1000
    link/ether 00:16:3e:6b:b0:49 brd ff:ff:ff:ff:ff:ff
    inet6 2605:340:cd50:2000:601:985e:d96b:7da0/128 scope global
       valid_lft forever preferred_lft forever
    inet6 fe80::216:3eff:fe6b:b049/64 scope link
       valid_lft forever preferred_lft forever

tm 侧上报正常。
```

之后在 `flowscheduler` 容器内继续执行 Redis 调试工具，又看到下面这组输入：

```text
dp-3c13d49395-5957b79bdd-7lbc8(data.systi.tidesched@stable:prod):flowscheduler# TOOL=pull_key_from_redis \
> JOBID=c2300b98-f212-4ead-bfe9-6088e2c49d56 \
> TASKID=4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9 \
> ./toolset
2026-04-28T15:12:19.308Z ERROR [client::common] failed to send request Status { code: Internal, message: "[byted-redis] no available address", metadata: MetadataMap { headers: {"content-type": "application/grpc", "date": "Tue, 28 Apr 2026 15:12:18 GMT", "content-length": "0"} }, source: None }
Error: failed to send request
```

这组输入带来几个重要结论：

1. `tm` 侧 `Heartbeat` 和 `Statsreport` 定时器确实在运行。
2. `topicstats` 里的部分 `resourceid` 与机器 `eth0` 上的 IPv6 地址可以对上，说明 scheduler 返回的 bucket 至少不是完全无关的数据。
3. `topicstats.job_res_tg_stats` 顶层 `jobId` 已经能和 worker 侧 `jobId` 对上，说明 `topic -> job` 基本映射是通的。
4. `pull_key_from_redis` 不是简单查 scheduler 内存，而是会通过 `distributed.debug()` 直接访问分布式后端。
5. `[byted-redis] no available address` 说明这次调试请求在访问 Redis 地址阶段就失败了，因此它不能证明 key 不存在，但能证明 **当前 Redis 访问链路有异常**。

继续在 `scheduler.log` 中跟踪 `redis` 关键词，又看到下面这组输入：

```text
dp-3c13d49395-5957b79bdd-7lbc8(data.systi.tidesched@stable:prod):logs# tail -f scheduler.log|grep redis
ERROR 2026-04-28 23:18:43,978 v1(7) scheduler/src/server/heartbeat/heartbeat.rs:167 2605:340:cd50:2000:1bb8:82a9:1465:dd4 data.systi.tidesched - stable boe prod canary 0 message=failed to dispatch heartbeat because [byted-redis] no available address
ERROR 2026-04-28 23:18:44,193 v1(7) scheduler/src/server/heartbeat/heartbeat.rs:167 2605:340:cd50:2000:1bb8:82a9:1465:dd4 data.systi.tidesched - stable boe prod canary 0 message=failed to dispatch heartbeat because [byted-redis] no available address
ERROR 2026-04-28 23:18:44,381 v1(7) scheduler/src/server/heartbeat/heartbeat.rs:167 2605:340:cd50:2000:1bb8:82a9:1465:dd4 data.systi.tidesched - stable boe prod canary 0 message=failed to dispatch heartbeat because [byted-redis] no available address
ERROR 2026-04-28 23:18:44,386 v1(7) scheduler/src/server/heartbeat/heartbeat.rs:167 2605:340:cd50:2000:1bb8:82a9:1465:dd4 data.systi.tidesched - stable boe prod canary 0 message=failed to dispatch heartbeat because [byted-redis] no available address
ERROR 2026-04-28 23:18:44,626 v1(7) scheduler/src/server/heartbeat/heartbeat.rs:167 2605:340:cd50:2000:1bb8:82a9:1465:dd4 data.systi.tidesched - stable boe prod canary 0 message=failed to dispatch heartbeat because [byted-redis] no available address
ERROR 2026-04-28 23:18:44,674 v1(7) scheduler/src/statemgr/distributed/sharedredis/mod.rs:95 2605:340:cd50:2000:1bb8:82a9:1465:dd4 data.systi.tidesched - stable boe prod canary 0 message=failed to get all state because [byted-redis] no available address
ERROR 2026-04-28 23:18:44,697 v1(7) scheduler/src/server/heartbeat/heartbeat.rs:167 2605:340:cd50:2000:1bb8:82a9:1465:dd4 data.systi.tidesched - stable boe prod canary 0 message=failed to dispatch heartbeat because [byted-redis] no available address
ERROR 2026-04-28 23:18:46,194 v1(7) scheduler/src/server/heartbeat/heartbeat.rs:167 2605:340:cd50:2000:1bb8:82a9:1465:dd4 data.systi.tidesched - stable boe prod canary 0 message=failed to dispatch heartbeat because [byted-redis] no available address
```

这组日志把判断进一步收敛为：

6. `heartbeat` 分发到 distributed 后端时已经直接失败，失败点发生在 scheduler 内部，而不是只出现在调试工具路径上。
7. `sharedredis` 的周期 `pull` 也在失败，说明不仅写路径有问题，读路径同样有问题。
8. 因此本案例中，Redis 地址发现/可达性故障已经被 scheduler 自身日志直接证实，可以视为当前主因。

## 3. 问题画像

现场现象类似下面这样：

- `tide_worker` 中 `src/source/httpd_source.cpp` 周期打印：
  - `raw_inmsgs:0 inmsgs:0 inbytes:0 available_queue_len:64`
- scheduler 的 HTTP 接口：
  - `curl localhost:6789/stable/topicstats?topic=doubao-trace-TUDP.pb`
  - 返回 `num_available=0`
  - 返回 `avalive_queue_length=0`
  - 返回 `total_queue_length=0`
  - 返回 `rows_per_sec=0.0`

这类问题的关键是先分清楚:

1. `httpd_source.cpp` 里的 `available_queue_len` 是 **source 本地消费队列** 的观测值。
2. scheduler `topicstats` 返回的是 **task 级 Statsreport + Heartbeat 进入状态存储后** 的聚合值。
3. 两边虽然都叫 queue/available，但**不一定是同一个队列、也不一定来自同一个层次**。

如果 `topicstats` 里连 `total_queue_length` 都是 `0`，通常说明不是“队列真的为 0”，而是 **Statsreport 没有成功进入最终聚合视图**。在一般场景里，问题位置可能在：

- scheduler 收到请求之前
- scheduler flush 到 Redis 之前
- Redis 写入 / 读取阶段
- Redis 拉回 `StateManager` 之后的聚合阶段

但在本案例里，scheduler 日志已经直接给出：

- `failed to dispatch heartbeat because [byted-redis] no available address`
- `failed to get all state because [byted-redis] no available address`

所以当前应把问题主因收敛到 **Redis 地址发现/可达性故障**，而不是继续泛化地列举所有可能阶段。

## 4. 先说结论

本链路里至少有 5 个不同观察点：

```text
+--------------------+      +--------------------+      +--------------------+      +-------------------+
| httpd source local |      | task-level report  |      | distributed store  |      | scheduler state   |
| queue observation  | ---> | to flow scheduler  | ---> | Redis/shared state | ---> | aggregation view  |
+--------------------+      +--------------------+      +--------------------+      +-------------------+
         |                             |                             |                            |
         v                             v                             v                            v
available_queue_len          queueAvailableLen /            alive / runtime               topicstats:
in httpd_source log          queueTotalLen / rps            in per-task key               num_available
                                                                                          avalive_queue_length
                                                                                          total_queue_length
                                                                                          rows_per_sec
```

在一般场景下，“worker 本地日志有值，但 scheduler 全是 0”常见有 5 类原因：

1. **看错对象**：`httpd_source` 本地日志和 scheduler 聚合指标不是同一个队列。
2. **Heartbeat 没到**：`num_available=0` 说明 alive 没被置为 `true`。
3. **Statsreport 没到**：`avalive_queue_length=0` 且 `total_queue_length=0` 常见于 runtime 未落到 Redis。
4. **DAG / topic 映射或聚合对象不对**：`topic` 虽然能解析到正确 job，但 task / group / resource 或聚合 bucket 仍可能对不上。
5. **时间窗问题**：scheduler 的 statsreport 落盘与 Redis 拉取是异步的，瞬时看可能滞后。

结合本次已经补充的上下文，这 5 类原因的优先级需要调整：

1. **观测点不同** 仍然成立：`httpd_source` 本地队列和 `topicstats` 聚合值不是同一层。
2. **tm 未上报** 的优先级显著下降：因为 `flowschedHearbeat` 和 `flowschedStatsReport` 已经打出，且 `availQueueLen=64 queueLenCapacity=64`。
3. **Redis 地址发现/可达性故障** 已被 scheduler 日志直接证实，这是当前主因。
4. **scheduler 状态存储/聚合视图问题** 仍然存在，但更像是 Redis 故障导致的结果，而不是独立主因。
5. **DAG / topic 映射的更细粒度问题** 在当前证据下优先级明显下降：虽然仍可作为次级检查项，但不是第一落点。

## 5. 端到端链路

### 5.1 总流程

```text
Part A. DAG / topic 映射链路

    JobManager
        |
        | pull executor DAG
        v
    flow-scheduler dag reader
        |
        v
    schedgroup::DAGEvent::on_add_job()
        |
        +--> 识别 source.sharedhttpd
        |      读取 subtask.operator_options["httpd.topic"]
        |
        +--> 构建 topic -> job/group -> resource-taskgroup 映射
        |
        v
    /stable/topicstats?topic=...


Part B. 运行时状态写路径

    tide_worker task
        |
        | timer(1s): heartbeat
        | timer(3s): statsreport
        v
    control::flowsched::GrpcClient
        |
        | gRPC
        v
    flow-scheduler GrpcService
        |
        +--> heartbeat manager
        |      -> distributed.heartbeat()
        |      -> Redis key {cluster}:{jobid}:{taskid}
        |      -> field alive=true, pexpire(ttl)
        |
        +--> statsreport manager
               -> 内存缓存 HashMap<jobid, HashMap<taskid, StatsreportState>>
               -> 每 6s flush 到 distributed.statsreport()
               -> Redis key {cluster}:{jobid}:{taskid}
               -> field runtime={QueueAvailableLen, QueueTotalLen, KvIndicators}


Part C. 运行时状态读路径

    Redis / shared state
        |
        | distributed.start()
        | first full pull + periodic pull
        v
    StateManager::on_distributed_event()
        |
        | per task state:
        |   alive / avalive_queue_length / total_queue_length / rows_per_sec
        v
    aggregate by job + resource-taskgroup
        |
        v
    SchedgroupManager::topic_stats(topic)
        |
        v
    HTTP topicstats JSON
```

### 5.2 关键时序

默认配置下，现场看到的值不是实时直通，而是异步汇总：

```text
worker heartbeat enqueue        every 1s
worker statsreport enqueue      every 3s
scheduler statsreport flush     every 6s
scheduler redis pull            every 5s
alive TTL expire                60s
```

因此刚启动、刚恢复、刚切换 topic 时，`topicstats` 可能会有数秒延迟。

### 5.3 Redis 在这条链路里的作用

在当前 `flowscheduler` 代码中，Redis 不是一个“只给调试工具看的旁路组件”，而是 **distributed 模式下 runtime 状态的共享后端**：

- `heartbeat()` 会把 `alive=true` 和 TTL 写进 Redis。
- `statsreport()` 会把 `runtime` JSON 写进 Redis。
- `distributed.start()` 会在启动时先 full pull 一次，然后周期性从 Redis 拉回所有已知 task 的状态。
- `topicstats` 最终读的是 `StateManager` 的内存聚合视图，而这个视图在 Redis 模式下是通过 **从 Redis pull 回来** 更新的。

所以在 **当前 Redis distributed 模式** 下：

- Redis 写失败，会影响后续 pull 到的 task 状态。
- Redis 读失败，会影响 `StateManager` 刷新。
- Redis 地址发现失败，也会让调试工具 `pull_key_from_redis` 直接报错。

但从架构上说，Redis **不是唯一实现**。`distributed_mode` 还支持：

- `stdredis`
- `bytedredis`
- `partners`
- `none`

也就是说：

- **架构上** Redis 不是必须的，distributed 后端是可替换的。
- **你当前这套实例** 如果跑的是 `bytedredis` / `stdredis`，那 Redis 就是 `topicstats` 这条状态链路的关键依赖。

## 6. stream_engine 侧上报逻辑

### 6.1 `httpd_source` 本地日志在看什么

`HttpListenerSource::OnTicker()` 会打印：

```text
jobId:{} taskId:{} name:{} raw_inmsgs:{} inmsgs:{}, inbytes:{}, available_queue_len:{}
```

其中 `available_queue_len` 来自：

```text
m_consumerObj->AvgQueueLength().avaliable()
```

含义是 **HTTP source 内部 consumer queue 当前可用槽位数**。

注意：

- 这个值不是 scheduler 直接读取的值。
- 这个值也不是 `topicstats.total_queue_length`。
- 单看这里，仍不能证明 scheduler 一定已经收到并聚合成功。
- 但结合补充输入中的 `flowschedStatsReport`，至少可以说明 source 侧和 task 侧看到的可用队列都不是空值。

### 6.2 真正发往 scheduler 的是 task 级 `StatsreportReq`

worker 真正往 scheduler 发数据的入口不在 `httpd_source.cpp`，而在 taskmanager 的定时器：

```text
TaskBuilder::on_trigger_statsreport_event()
```

流程如下：

```text
CTask
  -> GetQueueLen(0..m_dataNum-1)
  -> 计算平均 queueAvailableLen / queueLenCapacity
  -> channel->Statsreport(...)
  -> GrpcClient::statsreport_reader()
  -> gRPC StatsreportReq
```

要点：

1. 它取的是 **CTask 的 ring queue**，不是 `httpd_source` 自己的内部 consumer queue。
2. `queueAvailableLen` 和 `queueLenCapacity` 是从 `task->GetQueueLen(i)` 计算来的。
3. `rps` 取自 `task->GetRps()`，最终以 `kv_indicators["num-rows-per-second"]` 发到 scheduler。

### 6.3 Heartbeat 和 Statsreport 是两条独立链路

```text
on_trigger_heartbeat_event()   -> channel->SendHeartbeat(...)
on_trigger_statsreport_event() -> channel->Statsreport(...)
```

所以一般分析时要分开看：

- `num_available=0`
  - 优先怀疑 heartbeat 没成功
- `total_queue_length=0`
  - 优先怀疑 statsreport 没成功

结合本次样例，这个判断要再往后走一步：

- `tm` 侧 heartbeat / statsreport 已经证明在触发
- 因此后续更应继续查 scheduler 是否正确接收、落 Redis、以及是否把当前 task 聚合到了正在查询的 topic/job 上

### 6.4 worker 侧建议先查的日志

先在 worker 日志里找这两类关键词：

```bash
grep -a "flowschedHearbeat" tide_worker.log
grep -a "flowschedStatsReport" tide_worker.log
```

如果要限定 task：

```bash
grep -a "flowschedStatsReport.*<taskId>" tide_worker.log
grep -a "flowschedHearbeat.*<taskId>" tide_worker.log
```

关注点：

- 是否周期性出现
- `availQueueLen` / `queueLenCapacity` 是否为预期
- 是否有 gRPC 发送失败、队列 push 失败、client start 失败等异常日志

## 7. flow-scheduler 侧聚合逻辑

### 7.1 `topicstats` 接口本身很薄

HTTP 路由：

```text
/{prefix}/topicstats?topic={topic}
```

处理逻辑本身只做两件事：

1. 从 query 里拿 `topic`
2. 调 `schedgroup_manager.topic_stats(topic)`

注意：

- URL 里的 `{prefix}` 目前只是路径占位，`topicstats` 查找时**没有使用它做过滤**。
- 真正的关键参数只有 query string 里的 `topic=...`。

### 7.2 topic 是怎么映射到 job 的

scheduler 在 DAG 构建阶段会扫描 subtask：

```text
operator_unique_name == "source.sharedhttpd"
```

并读取：

```text
subtask.operator_options["httpd.topic"]
subtask.operator_options["httpd.hostport"]
```

然后建立：

```text
topic -> SchedGroup -> job/group -> resource taskgroup
```

因此：

- `topicstats` 能返回某个 job，说明 **DAG 里 topic 映射已经存在**。
- 但这不等于 runtime 上报一定正常。

### 7.3 scheduler 如何接收 Statsreport

gRPC 服务收到 `StatsreportReq` 后，不会直接更新 `topicstats`，而是先进入一个内存缓存：

```text
GrpcService::statsreport()
  -> statsreport.report(request)
  -> HashMap<job_id, HashMap<task_id, StatsreportState>>
  -> 每 6 秒 flush 到 distributed.statsreport()
```

如果此处出问题，`topicstats` 常见表现是：

- `total_queue_length = 0`
- `avalive_queue_length = 0`
- `rows_per_sec = 0.0`

注意这里还有一个很容易误解的点：

- `statsreport.report(request)` 先写的是 scheduler 进程内的临时缓存。
- `topicstats` 不是直接读这个缓存。
- 在当前 Redis distributed 模式下，它仍然依赖后续 flush 到 Redis，再由 pull 线程拉回 `StateManager`。

### 7.4 scheduler 如何接收 Heartbeat

Heartbeat 是另一条独立链路：

```text
GrpcService::heartbeat()
  -> heartbeat_manager
  -> distributed.heartbeat()
  -> Redis hash field: alive=true
  -> 带 TTL
```

如果 heartbeat 没到或者已过期，`topicstats` 常见表现是：

- `num_available = 0`

### 7.5 Redis 中实际落什么

对于每个 `(cluster, jobid, taskid)`，Redis key 大致是：

```text
{cluster}:{jobid}:{taskid}
```

里面至少有两个关键 field：

```text
alive   -> "true" / "false"
runtime -> JSON
```

`runtime` JSON 主要包含：

```json
{
  "QueueAvailableLen": 64,
  "QueueTotalLen": 64,
  "KvIndicators": {
    "num-rows-per-second": "0"
  }
}
```

如果你在 `topicstats` 里看到：

- `total_queue_length = 0`

那优先看 Redis 里该 task 对应 key 是否根本没有 `runtime` 字段，或 `runtime` 为空/过旧。

### 7.6 `pull_key_from_redis` 为什么会受 Redis 故障影响

`pull_key_from_redis` 这条调试命令不是“读 scheduler 当前内存”，它会走：

```text
GrpcService::pull_key_from_redis()
  -> distributed.debug(tool="pull_key")
  -> Redis client get_async_connection()
  -> hgetall(key)
```

所以当你看到：

```text
[byted-redis] no available address
```

它更接近下面这类问题：

- 当前实例使用的是 `bytedredis` 模式
- byted-redis 客户端拿不到可用地址
- Redis 服务发现、网络、权限或 Redis 自身可用性有问题

它**不能直接证明**：

- 这个 key 不存在
- `tm` 没上报
- `topicstats` 一定是聚合逻辑 bug

它只能直接证明：

- 这次调试请求没有成功访问到 Redis 后端

### 7.7 `topicstats` 最终是怎么聚合的

`StateManager` 会把 task 级状态按 `resourceid + groupid` 做聚合：

```text
per task state
    |
    +--> alive                 -> num_available 累加
    +--> avalive_queue_length  -> avalive_queue_length 累加
    +--> total_queue_length    -> total_queue_length 累加
    +--> rows_per_sec          -> rows_per_sec 累加
    |
    v
per resource-taskgroup state
```

所以 `topicstats` 返回的每个：

```text
groupid:...;resourceid:...
```

本质上是一个 resource-taskgroup bucket 的聚合结果，不是单 task 原始值。

## 8. ID 含义说明

### 8.1 `jobId`

例如：

```text
jobId:c2300b98-f212-4ead-bfe9-6088e2c49d56
```

含义：

- 这是 `stream_engine` 当前 task 所属的运行时 job ID。
- `HeartbeatReq` 和 `StatsreportReq` 都会带上这个 `jobId`。
- scheduler / Redis 中 task 状态的主键维度之一就是 `jobId + taskId`。

排查意义：

- 通用情况下，如果 worker 侧日志中的 `jobId` 和 `topicstats.job_res_tg_stats` 顶层 key 不是同一个值，就要重点怀疑 topic 映射看到的是另一份 job，或者 DAG 仍停留在旧 job 视图。

### 8.2 `taskId`

例如：

```text
taskId:4b95e3b3-9f9c-4e7b-a02d-0d3a460c83a9
```

含义：

- 这是当前具体 task 的运行时 task ID。
- worker 的 `HeartbeatReq` / `StatsreportReq` 都按这个 task 粒度上报。
- Redis 中也是按 `(cluster, jobId, taskId)` 形成 key。

排查意义：

- 只要 `taskId` 对不上，即使 `topicstats` 里能看到某个 job，也可能出现 bucket 存在但所有统计值都是 0 的情况。

### 8.3 `topicstats.job_res_tg_stats` 顶层 key

例如：

```text
c2300b98-f212-4ead-bfe9-6088e2c49d56
```

含义：

- 这是 scheduler 视角下，当前 `topic` 关联到的某个 job ID。
- 这个 ID 不是从 HTTP 请求实时生成的，而是从 DAG/topic 映射中查出来的。

排查意义：

- 在本次样例里，这个顶层 `jobId` 已经与 worker 侧 `jobId` 对上。
- 因此本次问题不再优先怀疑 `topic -> job` 顶层映射错误，而更应继续检查：
  - Redis 原始 key 是否已经写入对应 task
  - 对应 task 是否被聚合进当前 `groupid/resourceid` bucket
  - `alive` / `runtime` 字段为什么在聚合结果中仍表现为 0

### 8.4 `groupid`

例如：

```text
groupid:4a43a2f2-b6f2-43ef-9571-f333a061fec8
```

含义：

- 这是 DAG 中的 task group ID。
- scheduler 在构建 topic group 时，会把某个 `source.sharedhttpd` 所在 group 关联到某个 topic。

排查意义：

- 这个 ID 用来描述“topic 对应的是哪个 group 的流量/资源池”，不是单 task ID。

### 8.5 `resourceid`

例如：

```text
resourceid:[2605:340:cd50:2000:601:985e:d96b:7da0]:6510
```

含义：

- 这是 DAG / task 元数据里的资源标识。
- 一般可理解为某个执行资源实例的地址标识。
- 前半段 IPv6 地址通常就是节点地址，后面的 `6510` / `7510` 是该资源地址的一部分。

结合本次样例：

- `resourceid` 里的 IPv6 `2605:340:cd50:2000:601:985e:d96b:7da0`
- 与 `ip addr` 中 `eth0` 的 IPv6 地址一致

这说明：

- `topicstats` 中至少有一部分 bucket 指向当前机器对应的资源。

### 8.6 `groupid:...;resourceid:...`

例如：

```text
groupid:4a43...;resourceid:[2605:...]:6510
```

含义：

- 这是 `topicstats.res_tg_states` 的 key。
- 表示一个 `resource-taskgroup` 聚合 bucket。
- 它不是单条原始 task 状态，而是一个聚合维度。

桶里的值含义：

- `num_available`
  - 该 bucket 下 `alive=true` 的 task 数量
- `avalive_queue_length`
  - 该 bucket 下所有 task 的 `queueAvailableLen` 之和
- `total_queue_length`
  - 该 bucket 下所有 task 的 `queueTotalLen` 之和
- `rows_per_sec`
  - 该 bucket 下所有 task 的 `num-rows-per-second` 之和

### 8.7 URL 中的 `stable`

例如：

```text
curl localhost:6789/stable/topicstats?topic=doubao-trace-TUDP.pb
```

这里的：

```text
stable
```

含义：

- 它只是路由上的路径前缀 `prefix`。
- 当前 `topicstats` 实现里没有使用这个值做 topic 过滤。

排查意义：

- 真正影响查找结果的是 query 参数里的 `topic=...`，不是 `stable`。

## 9. 为什么会出现“worker 看着正常，topicstats 全是 0”

### 9.1 本地 source 队列和 task runtime 队列不是一个东西

这是最容易混淆的点：

```text
httpd_source.cpp log
    看的是 source 内部 consumer queue

task_builder statsreport
    看的是 CTask::GetQueueLen(i) 暴露的 task 运行时队列
```

因此即使：

```text
httpd_source available_queue_len = 64
```

也不能直接推出：

```text
topicstats avalive_queue_length = 64
```

### 9.2 在本次样例里，tm 未上报已经不是首要怀疑点

结合补充输入，当前已经有这些事实：

- `flowschedHearbeat` 周期打印
- `flowschedStatsReport` 周期打印
- `availQueueLen=64 queueLenCapacity=64`
- `resourceid` 中的 IPv6 地址与本机 `eth0` 地址可以对上

这意味着：

- `tm` 侧 heartbeat / statsreport 定时器确实在触发
- task 级上报值本身不是空的
- scheduler 返回的 bucket 至少部分指向当前机器资源

所以在本次样例里，`tm` 完全没上报已经不是首要怀疑点。

### 9.3 当前主因已收敛到 Redis 地址发现/可达性故障

当前已经不只是“怀疑”，而是有 scheduler 自身日志直接证实：

- `failed to dispatch heartbeat because [byted-redis] no available address`
- `failed to get all state because [byted-redis] no available address`

结合你最新补充的上下文：

- worker 侧 `jobId = c2300b98-f212-4ead-bfe9-6088e2c49d56`
- `topicstats.job_res_tg_stats` 顶层 key 也是 `c2300b98-f212-4ead-bfe9-6088e2c49d56`

因此当前不再把“顶层 job 映射错误”作为主要判断，而是把问题主因收敛到：

- scheduler 写 Redis 失败
- scheduler 从 Redis pull 失败
- Redis 地址发现 / 可达性故障

### 9.4 Redis 拉取 / 过期 / 时序窗口问题仍然存在

这类问题仍然可能造成短时全 0：

- statsreport 刚写入，scheduler 还没 pull
- heartbeat TTL 到期后短暂变 0
- scheduler / Redis 抖动导致状态暂时空洞

但在本次样例里，它的优先级低于已经被日志证实的 Redis 故障。

## 10. 现场排查顺序

建议严格按下面顺序查，避免同时怀疑多层。

### 第 1 步：确认 topic 映射到了哪个 job

先看：

```bash
curl -s "http://127.0.0.1:6789/stable/topicstats?topic=doubao-trace-TUDP.pb"
```

确认：

- 返回里有哪些 `job_res_tg_stats`
- 这些 jobId 是否符合预期

如果这里已经能返回 job，说明：

- scheduler 的 DAG/topic 映射至少是存在的

### 第 2 步：在 worker 日志确认 heartbeat / statsreport 是否真的在发

```bash
grep -a "flowschedHearbeat" tide_worker.log
grep -a "flowschedStatsReport" tide_worker.log
```

重点确认：

- 对应 taskId 是否持续出现
- `queueLenCapacity` 是否非 0
- 发送链路是否报错

### 第 3 步：确认 scheduler Redis 链路是否健康

flow-scheduler 自带一个 gRPC 调试接口 `PullKeyFromRedis`，可直接拉单个 task 对应的 Redis hash。

示例：

```bash
cd /root/Documents/flow-scheduler/tests/client
TOOL=pull_key_from_redis \
JOBID=<jobId> \
TASKID=<taskId> \
cargo run --bin toolset
```

如果链路健康，期望至少看到：

```text
alive=true
runtime={"QueueAvailableLen":...,"QueueTotalLen":...,"KvIndicators":...}
```

但在本次样例里，这一步已经被实际日志改写成：

- `PullKeyFromRedis` 调试接口返回 `[byted-redis] no available address`
- `scheduler.log` 中出现 `failed to dispatch heartbeat because [byted-redis] no available address`
- `scheduler.log` 中出现 `failed to get all state because [byted-redis] no available address`

因此这一步当前的结论不是“某个 key 为空”，而是：

- scheduler 到 Redis 的读写链路本身已经故障

### 第 4 步：Redis 恢复后再核对 task 聚合是否一致

在 Redis 故障修复后，再对齐三处：

1. worker 日志里的 `jobId` / `taskId`
2. Redis key 的 `jobid` / `taskid`
3. scheduler DAG 中该 topic 对应 group 下的 task 集

只要 taskid 不一致，`topicstats` 就会返回“有 job、有 bucket，但状态全 0”的假象。

### 第 5 步：Redis 恢复后再考虑时序窗口

如果 Redis 链路恢复，且第 2 步和第 3 步都正常，但 `topicstats` 偶发为 0，再考虑等待 1 到 2 个同步周期后再看：

```text
statsreport flush 6s + redis pull 5s
```

通常等 10 到 15 秒再观察一次更稳妥。

## 11. 对这次现象的判读建议

针对你给出的样例：

```text
httpd_source:
  raw_inmsgs=0
  inmsgs=0
  available_queue_len=64

topicstats:
  num_available=0
  avalive_queue_length=0
  total_queue_length=0
  rows_per_sec=0.0
```

结合补充输入后，这次现象不再适合写成“worker 没发 / 本地队列不是一回事”的二选一。

当前更准确的判断是：

1. `httpd_source` 本地日志和 `topicstats` 仍然不是同一层观测点，这个前提继续成立。
2. `tm` 侧 `Heartbeat` 和 `Statsreport` 已经证明在发，且 task 级上报值不是空的。
3. `resourceid` 中的 IPv6 地址与本机 `eth0` 地址能对上，说明 scheduler 返回的 bucket 至少部分指向当前机器。
4. `topicstats` 顶层 `jobId` 已经和 worker 侧 `jobId` 对上，因此 `topic -> job` 顶层映射不是当前主问题。
5. scheduler 日志已经直接证实 heartbeat 分发失败和 sharedredis pull 失败，因此当前主因应收敛到 Redis 地址发现 / 可达性故障。

其中 `total_queue_length=0` 是很强的信号；结合现有日志，当前优先级最高的检查点是：

```text
byted-redis 地址发现为什么失败
        ->
scheduler 到 Redis 的读写链路为什么不可用
        ->
Redis 恢复后，该 task 的 alive/runtime 是否能正常落 key 并被聚合
```

## 12. 代码定位速查

### 12.1 stream_engine

- `src/source/httpd_source.cpp`
  - `HttpListenerSource::OnTicker()`
- `src/source/httpd_source.h`
  - `HttpListenerSource::GetQueueLength()`
  - `ConsumerObj::AvgQueueLength()`
- `src/runtime/taskmanager/task_builder.h`
  - `on_trigger_heartbeat_event()`
  - `on_trigger_statsreport_event()`
  - `init_sched_channel()`
- `src/control/flowsched/grpc_client.cpp`
  - `GrpcClient::Start()`
  - `GrpcClient::heartbeat_reader()`
  - `GrpcClient::statsreport_reader()`
- `src/control/flowsched/client_base.cpp`
  - `ChannelBase::Statsreport()`
  - `ChannelBase::PopLatestStatsreport()`
- `src/protocol/tide_flowsched_api.proto`
  - `HeartbeatReq`
  - `StatsreportReq`
  - `StatsreportState`

### 12.2 flow-scheduler

- `scheduler/src/server/mod.rs`
  - `GrpcService::heartbeat()`
  - `GrpcService::statsreport()`
  - HTTP route `/{prefix}/topicstats`
- `scheduler/src/server/topicstats.rs`
  - `execute()`
- `scheduler/src/server/statsreport.rs`
  - statsreport 缓存与 6 秒 flush
- `scheduler/src/schedgroup/mod.rs`
  - `SchedgroupManager::topic_stats()`
  - `DAGEvent::find_sharedhttpd()`
  - `DAGEvent::on_add_job()`
- `scheduler/src/statemgr/distributed/sharedredis/mod.rs`
  - Redis `alive` / `runtime` 读写
- `scheduler/src/statemgr/mod.rs`
  - task state -> resource-taskgroup 聚合
- `scheduler/proto/service.proto`
  - `HeartbeatReq`
  - `StatsreportReq`
  - `PullKeyFromRedisReq`

## 13. 最短排查闭环

如果现场时间很紧，建议只做这 4 件事：

```text
1. curl /stable/topicstats?topic=...
2. grep worker log: flowschedHearbeat / flowschedStatsReport
3. pull_key_from_redis(jobId, taskId)
4. 对齐 taskId 是否属于该 topic 的 DAG
```

只要这 4 步跑完，基本就能把问题收敛到：

- `tm` 是否真的把 heartbeat / statsreport 发到了 scheduler
- scheduler 是否真的接收并写入了 Redis 原始 key
- 当前 `topic -> job -> group -> task -> resource` 映射是否一致
- DAG 版本或聚合 bucket 是否仍停留在旧视图
- 或者只是看错了队列层级
