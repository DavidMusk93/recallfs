# 项目中的 Redis 使用梳理

这份文档说明本项目如何使用 Redis，以及 Redis 出问题时可以如何定位。

文档聚焦 4 件事：

- Redis 在系统里的职责
- Redis 的配置项和 key 结构
- 数据如何写入 Redis、又如何从 Redis 回流到调度结果
- Redis 异常时的定位路径和实操命令

## 1. Redis 在这个项目里扮演什么角色

Redis 在这个项目里不是 DAG/拓扑来源，而是**运行时状态后端**。

- DAG / topic / group / task / resource 映射来自 Job Manager
- Redis 保存的是每个 `job + task` 的运行状态
- 调度和 `topicstats` 会消费这些状态

换句话说：

- Job Manager 决定“应该有哪些 task / resource”
- Redis 反映“这些 task 现在活不活、忙不忙”

## 2. 总体数据流

```text
                    Redis write path

  +---------------+      +-----------------------+
  | task / worker | ---> | gRPC server entries   |
  +---------------+      | - Heartbeat           |
                         | - Statsreport         |
                         +-----------+-----------+
                                     |
                                     v
                         +-----------------------+
                         | Redis hash per task   |
                         | key: cluster:job:task |
                         | fields: alive,runtime |
                         +-----------------------+


                    Redis read / consume path

  +---------------+      +-----------------------+
  | Job Manager   | ---> | DAGReader             |
  | GetExecutorDAG|      | build known job/task  |
  +---------------+      +-----------+-----------+
                                     |
                                     v
                         +-----------------------+
                         | pull known Redis keys |
                         | HGETALL hash          |
                         +-----------+-----------+
                                     |
                                     v
                         +-----------------------+
                         | StateManager          |
                         | update in-memory      |
                         +-----------+-----------+
                                     |
                     +---------------+---------------+
                     |                               |
                     v                               v
           +-------------------+           +------------------+
           | Schedgroup/policy |           | /topicstats      |
           | get_addr consume  |           | debug/inspection |
           +-------------------+           +------------------+
```

## 3. Redis 相关配置

Redis 的入口配置在 `service.distributed_mode`。

典型配置示例：

```yaml
service:
  distributed_mode:
    !bytedredis { "psm": "bytedance.redis.tidescheduler" }
```

这个例子就在 [tide_scheduler.boe.yaml](file:///root/Documents/flow-scheduler/scheduler/config/tide_scheduler.boe.yaml#L1-L4)。

代码入口：

- 分布式模式定义：[config/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/config/mod.rs#L309-L329)
- 默认模式：[config/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/config/mod.rs#L803-L809)
- 创建分布式后端：[distributed/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/mod.rs#L40-L71)

支持两类 Redis：

- `!stdredis`
  - 直接使用地址连接
- `!bytedredis`
  - 通过 PSM 构建 Redis client

你当前 `boe` 配置就是 `bytedredis`：

- [tide_scheduler.boe.yaml](file:///root/Documents/flow-scheduler/scheduler/config/tide_scheduler.boe.yaml#L1-L4)

### 3.1 关键配置项

`stdredis` 相关：

- `TIDESCHED_STATESTORE_ADDRESS`
- `STDREDIS_ADDRESS`
- `STDREDIS_USERNAME`
- `STDREDIS_PASSWORD`
- `TIDESCHED_STATESTORE_STDREDIS_PASSWORD`

`bytedredis` 相关：

- `BYTEREDIS_PSM`

同步和过期相关：

- `TASKEXPIRE_LIFECYCLE_MS`
  - Redis key 过期时间
- `REDIS_SYNC_INTERVAL_MS`
  - 后台从 Redis 拉取状态的周期

对应代码：

- `StdRedis` / `ByteRedis` 配置解析：[config/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/config/mod.rs#L236-L329)
- `taskexpire_lifecycle_ms` / `redis_sync_interval_ms`：[config/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/config/mod.rs#L720-L724), [config/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/config/mod.rs#L772-L778), [config/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/config/mod.rs#L875-L890)

## 3.2 Redis 连接代码在哪里

### `bytedredis` 连接代码

`bytedredis` 的 client 创建非常直接：

```rust
let builder = Builder::new(c.psm.clone());
let client = builder.build_redis().await?;
```

代码位置：

- [sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L303-L323)

这个 `c.psm` 来自：

- [config/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/config/mod.rs#L292-L307)

### `stdredis` 连接代码

`stdredis` 会先解析 `host:port`，再显式设置连接地址：

```rust
builder = builder
    .with_addrs(Some(vec![ConnectionAddr::Tcp(host.to_string(), port)]))
    .with_auth(AuthType::None);
```

代码位置：

- [sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L326-L392)

### 真正拿连接的代码

无论 `bytedredis` 还是 `stdredis`，后续真正执行 Redis 命令前，都会先拿连接：

```rust
let mut conn = self.client.get_async_connection().await?;
```

这个调用出现在：

- heartbeat 写入：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L106-L123)
- statsreport 写入：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L125-L149)
- close 写入：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L151-L165)
- sync_state 写入：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L167-L189)
- debug 读 key：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L191-L212)
- 后台 pull：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L215-L300)

这点很关键：

- `build_redis()` 成功，不等于每次业务写入时都一定拿得到可用连接
- 你的报错就是在业务写入阶段冒出来的，而不是配置解析阶段

## 3.3 依赖库源码里到底怎么做服务发现

本项目依赖的是：

- [byted-redis client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs)

关键行为如下。

### 第一次建 client

`Builder::build_redis()` 最终进入 `ClientInner::new(...)`：

- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L122-L139)
- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L255-L420)

其中逻辑是：

1. 如果显式指定了 `addrs`，直接按地址建连接池
2. 否则如果开启 mesh，走 mesh 地址
3. 否则走 `byted_sd::lookup(&query).await?`

代码位置：

- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L267-L297)
- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L299-L324)
- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L327-L355)

这里非常重要的一点：

- `byted_sd::lookup()` 失败会返回 error
- 但“返回空 endpoint 集合”不是 error
- 所以 client 可以成功创建，但内部 `addrs` 为空

### 每次业务拿连接

真正抛出你这条错误的是这里：

```rust
let addr = {
    let mut rng = rand::rng();
    pools.addrs.choose(&mut rng)
};
if let Some(addr) = addr {
    ...
} else {
    Err(Error::from(anyhow::anyhow!(
        "[byted-redis] no available address"
    )))
}
```

代码位置：

- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L231-L249)

因此这条错误的直接含义非常明确：

- 当前时刻 `pools.addrs` 是空的

也就是说，问题不是“随机挑中的那个地址连不上”；
而是“根本没有地址可选”。

### 后台服务发现刷新

`byted-redis` 会每 10 秒做一次 `byted_sd::lookup()`：

- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L358-L414)

如果 lookup 失败，只会打 warn：

- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L371-L375)

这意味着：

- 启动时 lookup 成功，后续 lookup 失败，不会立刻让已有地址消失
- 真正会把 `addrs` 刷成空，通常更像“lookup 成功但 endpoints 为空”或者刷新逻辑本身有问题

## 3.4 依赖库源码里一个高风险实现细节

在 `byted-redis` 的后台刷新逻辑里，有一个值得重点怀疑的问题：

- 复用旧 pool 的 endpoint，没有被重新 push 回 `new_addrs`

代码如下：

```rust
for ep in new_eps.endpoints.iter() {
    if let Some(pool) = pools.load().pools.get(&ep.clone().into()) {
        new_pools.insert(ep.clone().into(), pool.clone());
        continue;
    }
    ...
    new_pools.insert(ep.clone().into(), pool);
    new_addrs.push(ep.clone().into());
}
```

代码位置：

- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L381-L410)

这里的问题是：

- 老地址进入 `new_pools`
- 但没有进入 `new_addrs`
- 最终业务选地址是从 `pools.addrs.choose(...)` 里选
- 所以一旦发生一次 refresh，并且新结果里大部分地址都是“已有地址复用”
- 就可能出现：
  - `new_pools` 不空
  - 但 `new_addrs` 为空或不完整
  - 于是业务侧报 `[byted-redis] no available address`

换句话说，这里存在一种非常可疑的场景：

```text
初始 lookup:
  endpoints = [A, B]
  addrs = [A, B]

下一次 refresh:
  endpoints 还是 [A, B]
  但 hash 变化了
  A/B 都命中“复用旧 pool”
  new_pools = [A, B]
  new_addrs = []

结果:
  pools.addrs.choose(...) -> None
  报错 [byted-redis] no available address
```

如果你现场日志是：

- 服务刚启动时正常
- 过一段时间开始大量出现 `no available address`

那这个实现细节要被放到非常靠前的位置去怀疑。

## 4. Redis key 和 value 长什么样

### 4.1 key 结构

key 由下面的函数拼出来：

```text
{cluster}:{jobid}:{taskid}
```

代码位置：

- [sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L58-L60)

例如：

```text
fringedb-newly:b0332309-dde4-448c-96ab-7293395d9bc7:task-001
```

这里的 `cluster` 来自 `service.cluster`。

### 4.2 value 结构

Redis 里每个 key 是一个 hash，当前项目主要会写两个字段：

- `alive`
- `runtime`

含义：

- `alive`
  - 字符串布尔值
  - 来自心跳
- `runtime`
  - 一段 JSON 字符串
  - 来自 statsreport

`runtime` JSON 结构大致是：

```json
{
  "QueueAvailableLen": 0,
  "QueueTotalLen": 0,
  "KvIndicators": {
    "num-rows-per-second": "0.0"
  }
}
```

解析代码：

- `Runtime` / `new_state()`：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L23-L56)

## 5. 哪些地方会写 Redis

### 5.1 Heartbeat 写 `alive`

worker 通过 gRPC `Heartbeat` 上报存活。

流程：

```text
worker
  -> GrpcService.heartbeat()
  -> HeartbeatManager
  -> flush_impl()
  -> distributed.heartbeat(...)
  -> Redis HSET alive=true
  -> Redis PEXPIRE
```

关键代码：

- gRPC 入口：[server/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/mod.rs#L125-L149)
- flush 到分布式层：[heartbeat.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/heartbeat/heartbeat.rs#L130-L169)
- Redis 写入：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L106-L123)

这一步会做两件事：

- `HSET alive true`
- `PEXPIRE key taskexpire_lifecycle_ms`

这意味着：

- 只要心跳持续到达，key 会持续续期
- 如果心跳断了，key 会自然过期

### 5.2 Statsreport 写 `runtime`

worker 通过 gRPC `Statsreport` 上报队列长度和指标。

流程：

```text
worker
  -> GrpcService.statsreport()
  -> Statsreport::report() 先缓存
  -> 后台每 6 秒 flush
  -> distributed.statsreport(...)
  -> Redis HSET runtime=<json>
```

关键代码：

- gRPC 入口：[server/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/mod.rs#L152-L165)
- 本地缓存与 flush：[statsreport.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/statsreport.rs#L21-L67)
- Redis 写入：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L125-L149)

注意：

- `Statsreport::report()` 当前实现对同一个 `(job_id, task_id)` 在一次 flush 周期内只保留第一次插入的值
- 如果你怀疑统计值更新不及时，这里是一个要优先看的点

你看到的这类日志：

```text
failed to dispatch statsreport because [byted-redis] no available address
failed to dispatch heartbeat because [byted-redis] no available address
```

就是上面这两条业务写路径在调用 `distributed.statsreport(...)` / `distributed.heartbeat(...)` 后，底层 `get_async_connection()` 或后续 Redis 连接选择失败抛出来的。

### 5.3 Close / SyncState 也会写 Redis

除了 heartbeat / statsreport，还有两种路径会写 Redis：

- `close()`
  - 把 `alive` 写成 `false`
- `sync_state()`
  - 把内存状态同步回 Redis

代码：

- [sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L151-L189)

## 6. 哪些地方会读 Redis

Redis 的读取不发生在 HTTP 请求路径里，而是由后台轮询完成。

流程：

```text
Server::run()
  -> make_shared_state_manager(...)
  -> distributed.start()
  -> Redis::start()
  -> 先全量 pull 一次
  -> 后台循环 pull
  -> StateManager::on_distributed_event(...)
  -> 更新内存态
```

关键代码：

- 创建状态管理器：[statemgr/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/mod.rs#L720-L760)
- 启动 distributed：[server/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/mod.rs#L354-L410)
- Redis 启动与周期拉取：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L73-L104)
- 实际 pull：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs#L215-L300)

### 6.1 pull 的逻辑很关键

`pull()` 不会扫全库，它只会读**当前 DAG 和状态管理器里已知的 task**。

也就是：

- 先看 `job_states`
- 对每个已知 `(jobid, taskid)` 拼 key
- 再去 Redis `HGETALL`

这意味着：

- Redis 里有孤儿 key，不一定会被消费
- 如果 DAG 没拉到某个 job/task，即使 Redis 有状态也不会进入调度视图

## 7. Redis 数据如何影响调度和 topicstats

```text
Redis hash
  -> pull()
  -> new_state()
  -> StateManager per-task state
  -> 聚合成 per-resource-taskgroup state
  -> Schedgroup / policy 消费
  -> get_addr / topicstats 返回结果
```

其中：

- `alive` 会影响 `num_available`
- `runtime.QueueAvailableLen` 会影响 `avalive_queue_length`
- `runtime.QueueTotalLen` 会影响 `total_queue_length`
- `runtime.KvIndicators["num-rows-per-second"]` 会影响 `rows_per_sec`

如果 Redis 读出来是空 hash 或字段缺失，代码会回退到默认值：

- `alive = false`
- queue 长度 = `0`
- `rows_per_sec = 0.0`

所以你经常会看到一种现象：

- DAG 里资源还在
- `topicstats` 里也能看到 resource bucket
- 但全是 0

这通常说明问题更接近 Redis/心跳/统计上报层，而不是 DAG 层。

## 8. Redis 排障流程图

```text
现象：topicstats 全 0 / get_addr 不选某些资源 / 资源突然离线
  |
  +--> 1. 先看配置是否真的走 Redis
  |       - distributed_mode 是 stdredis 还是 bytedredis？
  |       - cluster 是什么？
  |       - TASKEXPIRE_LIFECYCLE_MS / REDIS_SYNC_INTERVAL_MS 是多少？
  |
  +--> 2. 看 DAG 是否已知这个 job/task
  |       - 如果 DAG 没拉到，Redis 有 key 也没用
  |
  +--> 3. 看 worker 是否在发 Heartbeat / Statsreport
  |       - 没有 heartbeat -> alive 不会续期
  |       - 没有 statsreport -> runtime 不会更新
  |
  +--> 4. 直接看 Redis key
  |       - key = {cluster}:{jobid}:{taskid}
  |       - 看 alive / runtime / TTL
  |
  +--> 5. 看服务是否成功 pull Redis
  |       - 是否有 get all state failed
  |       - 是否有 parse runtime failed
  |       - 是否有 key empty / expired 日志
  |
  +--> 6. 看 StateManager 聚合后的结果
          - pull 成功但 topicstats 仍异常，继续查 job/task 与 res_tg 聚合关系
```

## 9. 我该从哪里开始定位 Redis 问题

### 9.1 第一步：确认当前实例是否真的使用 Redis

看配置文件里的：

- `service.distributed_mode`
- `service.cluster`
- `service.taskexpire_lifecycle_ms`
- `service.redis_sync_interval_ms`

例如：

- [tide_scheduler.boe.yaml](file:///root/Documents/flow-scheduler/scheduler/config/tide_scheduler.boe.yaml#L1-L8)

也要记得环境变量可能覆盖 YAML。

### 9.2 第二步：确认 key 应该长什么样

key 公式：

```text
{cluster}:{jobid}:{taskid}
```

例如如果：

- `cluster = fringedb-newly`
- `jobid = b0332309-dde4-448c-96ab-7293395d9bc7`
- `taskid = xxx`

那么 key 就是：

```text
fringedb-newly:b0332309-dde4-448c-96ab-7293395d9bc7:xxx
```

### 9.3 第三步：直接拉 Redis key 内容

项目已经自带一个调试入口，不需要你手工写代码。

服务端 gRPC 调试接口：

- `PullKeyFromRedis`

实现位置：

- [server/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/mod.rs#L254-L279)

客户端工具：

- [toolset/main.rs](file:///root/Documents/flow-scheduler/tests/client/src/bin/toolset/main.rs#L8-L28)
- [pull_key_from_redis.rs](file:///root/Documents/flow-scheduler/tests/client/src/modules/pull_key_from_redis.rs#L1-L24)

示例：

```bash
cd tests/client
TOOL=pull_key_from_redis JOBID=<jobid> TASKID=<taskid> cargo run --bin toolset
```

你应该重点看返回里是否有：

- `alive`
- `runtime`

如果 `runtime` 存在，再看 JSON 里是否有：

- `QueueAvailableLen`
- `QueueTotalLen`
- `KvIndicators`
- `KvIndicators["num-rows-per-second"]`

### 9.4 第四步：如果 key 不存在或是空的，优先查什么

先查 heartbeat 路径：

- worker 是否真的在发 `HeartbeatReq`
- 服务端是否有 `failed to dispatch heartbeat`
- TTL 是否太短，导致 key 很快过期

再查 statsreport 路径：

- worker 是否真的在发 `StatsreportReq`
- 服务端是否有 `failed to dispatch statsreport`
- `runtime` JSON 是否写进去了

### 9.5 第五步：如果 Redis 正常但页面/接口不正常，查什么

查 pull 和聚合层：

- 是否有 `failed to get all state`
- 是否有 `parse job ... state ... because ...`
- 是否有 `key ... obtained from redis is empty`
- 是否有 `expired from sharedredis`

这些日志都在：

- [sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs)

## 10. 典型故障与定位建议

### 故障零：`[byted-redis] no available address`

这是你当前日志最直接对应的问题。

含义可以先粗暴理解成两类：

1. `bytedredis` 通过 PSM 没发现到任何 Redis 地址
2. 发现到了地址，但当前没有一个地址可用

对应到代码链路：

```text
service.distributed_mode = !bytedredis
  -> ByteRedis.psm
  -> Builder::new(psm)
  -> build_redis()
  -> get_async_connection()
  -> heartbeat/statsreport 写 Redis
```

你当前报错出现位置：

- [statsreport.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/statsreport.rs#L54-L55)
- [heartbeat.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/heartbeat/heartbeat.rs#L166-L167)

这说明：

- worker 的 heartbeat / statsreport 已经到达服务
- 失败点在“写 Redis”而不是“收请求”

#### 优先排查什么

1. 确认当前实例的 `PSM` 是什么
   - 配置文件看 `service.distributed_mode`
   - 环境变量看 `BYTEREDIS_PSM`
2. 确认当前环境能否通过这个 PSM 发现 Redis 实例
3. 确认发现到的实例是否真的健康、是否允许当前机房访问
4. 确认不是所有地址都下线、摘流或网络不可达

#### 这类错误最常见的原因

- `BYTEREDIS_PSM` 被环境变量覆盖成错误值
- 配置里的 PSM 写错
- 当前机房/环境查不到这个 PSM 的地址
- Redis 服务发现成功，但实例全挂或全不可连
- 网络策略、ACL、机房隔离导致所有地址都不可用

#### 如果命令行 `sd lookup` 能查到地址，说明什么

例如你已经验证过：

```text
sd lookup bytedance.redis.tidescheduler
  -> 返回多个 IPv6 + port
```

这条证据非常重要，因为它说明：

1. `PSM` 本身大概率没写错
2. 当前环境的服务发现控制面是能返回 endpoint 的
3. “Redis 地址为空”不一定发生在控制面，而可能发生在 client 进程内

所以排查优先级应该调整为：

1. 先怀疑 `byted-redis` 进程内地址列表是否被刷空
2. 再怀疑依赖库 refresh 逻辑是否有 bug
3. 最后再看 Redis 实例连通性和鉴权

也就是说：

- `sd lookup` 命令行能查到地址
- 不等于你的应用进程此刻 `pools.addrs` 一定非空

因为 `byted-redis` 真正抛错的位置，是从内存里的 `pools.addrs` 选地址：

- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L231-L249)

如果这里的 `pools.addrs` 为空，就会直接报：

```text
[byted-redis] no available address
```

即使同一时刻你在 shell 里执行 `sd lookup` 仍然能看到一堆地址。

#### 先做哪几个最小检查

先确认配置侧：

- 当前配置文件是否真的是 `!bytedredis`
- `BYTEREDIS_PSM` 是否覆盖了 YAML

再确认运行侧：

- 服务启动前后的配置日志里最终 `distributed_mode` 是什么
- 同时段是否只有这一台报错，还是同集群实例都报错
- 是否 heartbeat 和 statsreport 同时失败

如果二者同时失败，优先判断为：

- Redis 地址发现失败
- 或 Redis 整体不可连

而不是某条业务逻辑 bug

### 故障一：`topicstats` 里所有 bucket 都是 0

优先排查：

1. Redis key 是否存在
2. `alive` 是否一直是 `false` 或已过期
3. `runtime` 是否为空
4. worker 是否有 heartbeat/statsreport
5. DAG 中是否还保留这些 task/resource

### 故障二：某些 task 突然从视图里“掉了”

优先排查：

1. key 是否过期
2. `TASKEXPIRE_LIFECYCLE_MS` 是否太短
3. 心跳是否间歇性中断
4. DAG 是否更新后删除了该 task

### 故障三：Redis 里有 key，但系统里看不到状态

优先排查：

1. key 的 `cluster` 前缀是否和当前实例配置一致
2. DAG 是否已经把这个 job/task 纳入 `job_states`
3. 服务是否成功执行了 `pull()`
4. value 是否能被 `new_state()` 正常解析

### 故障四：连接不上 Redis

优先排查：

1. `stdredis` 地址格式是否正确
2. 用户名/密码是否被环境变量覆盖
3. `bytedredis` 的 PSM 是否正确
4. 启动日志里是否出现 `build redis client` 相关错误

如果报错文本是：

```text
[byted-redis] no available address
```

再加 3 个判断：

5. PSM 是否在当前环境返回了空地址集
6. 返回的地址是否全部 unhealthy / 被摘流
7. 当前机器到这些地址是否全部网络不可达

## 10.1 针对 `no available address` 的实战排查路径

```text
报错:
  [byted-redis] no available address

先判断:
  收请求正常吗?
    是 -> 说明 heartbeat/statsreport 已到服务端
       -> 问题在 Redis 发现/连接层

再判断:
  是单机报错还是全实例报错?
    单机 -> 优先看单机网络、路由、ACL、环境变量覆盖
    全实例 -> 优先看 PSM、Redis 服务发现、Redis 服务健康

继续判断:
  YAML 和环境变量的 PSM 一致吗?
    否 -> 先修正配置覆盖
    是 -> 继续查服务发现结果

最后判断:
  Redis 地址有返回吗?
    没返回 -> 服务发现问题
    有返回但都不可连 -> 连接/网络/实例健康问题
```

## 10.2 你现在这条日志应该怎么读

以这条为例：

```text
ERROR ... scheduler/src/server/statsreport.rs:55 ... failed to dispatch statsreport because [byted-redis] no available address
```

说明：

1. gRPC `StatsreportReq` 已经进到服务
2. `Statsreport::report()` 已收下数据
3. 后台 flush 到 `distributed.statsreport(...)` 时失败
4. 失败更接近：
   - `bytedredis` 地址发现为空
   - 或没有一个可用地址能建立连接

这条也一样：

```text
ERROR ... scheduler/src/server/heartbeat/heartbeat.rs:167 ... failed to dispatch heartbeat because [byted-redis] no available address
```

说明：

1. gRPC `HeartbeatReq` 已到服务
2. `flush_impl()` 已准备写 Redis
3. 失败发生在 Redis 连接阶段

所以从优先级上，应先查：

1. `bytedance.redis.tidescheduler` 这个 PSM 在当前环境是否可发现
2. 是否被 `BYTEREDIS_PSM` 覆盖
3. Redis 服务端地址是否健康
4. 当前宿主机到 Redis 地址是否可达

## 10.3 结合你这次 `sd lookup` 的新判断

你已经拿到如下事实：

```text
sd lookup bytedance.redis.tidescheduler
  -> 能返回多个 endpoint
```

这会显著改变判断：

- “服务发现完全没结果”这个方向优先级下降
- “依赖库内部状态和刷新逻辑有问题”优先级上升

当前更值得怀疑的是：

1. `byted-redis` 后台 refresh 后把 `new_addrs` 刷空
2. 应用内看到的 endpoint 集合和命令行查询结果不一致
3. 不是 lookup 失败，而是 lookup 成功后 client 内部地址列表丢失

尤其要注意前面提到的源码实现：

- [client.rs](file:///root/.cargo/registry/src/rust-preonline.byted.org-106cff7ebb4f1ebc/byted-redis-0.13.3/src/client.rs#L381-L410)

这里复用旧 pool 时，没有把地址 push 回 `new_addrs`。

这意味着完全可能出现下面这种现象：

```text
命令行:
  sd lookup bytedance.redis.tidescheduler
  -> 有 5 个 endpoint

应用进程内:
  new_pools = 5
  new_addrs = 0

业务结果:
  get_async_connection()
  -> [byted-redis] no available address
```

所以，单看你现在手头的证据，我会把怀疑顺序排成：

1. `byted-redis` refresh bug
2. 进程内地址列表和 shell 查询结果不一致
3. Redis 单点连接/鉴权失败

而不是：

1. PSM 配错
2. 服务发现查不到 Redis

## 11. 快速排障 Checklist

```text
[ ] 当前实例 distributed_mode 是什么
[ ] 当前实例最终生效的 PSM 是什么
[ ] 当前实例 cluster 是什么
[ ] 目标 jobid/taskid 是什么
[ ] Redis key 是否按 {cluster}:{jobid}:{taskid} 拼对了
[ ] 是否出现 [byted-redis] no available address
[ ] key 里有没有 alive
[ ] key 里有没有 runtime
[ ] runtime 里有没有 QueueAvailableLen / QueueTotalLen / KvIndicators
[ ] worker 是否持续发 heartbeat
[ ] worker 是否发 statsreport
[ ] 服务端有没有 pull Redis 失败日志
[ ] bytedredis 是否能发现出地址
[ ] 发现出的地址是否可连
[ ] DAG 是否包含该 job/task
```

## 12. 相关代码索引

- 分布式后端选择：[distributed/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/mod.rs)
- Redis 实现：[sharedredis/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/distributed/sharedredis/mod.rs)
- 配置定义：[config/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/config/mod.rs)
- 状态管理器装配：[statemgr/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/statemgr/mod.rs#L720-L760)
- 服务启动装配：[server/mod.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/mod.rs#L348-L410)
- Heartbeat 写入路径：[heartbeat.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/heartbeat/heartbeat.rs)
- Statsreport 写入路径：[statsreport.rs](file:///root/Documents/flow-scheduler/scheduler/src/server/statsreport.rs)
- 拉 Redis key 调试工具：[pull_key_from_redis.rs](file:///root/Documents/flow-scheduler/tests/client/src/modules/pull_key_from_redis.rs)
