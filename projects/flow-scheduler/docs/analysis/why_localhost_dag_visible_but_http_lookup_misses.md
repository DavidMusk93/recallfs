# 为什么 localhost 能看到 DAG，但 HTTP 调度接口查不到

样例文件：

- `docs/sample/47dc7bf6-481b-487b-bd89-41420c6c58d7.txt`

目标 job：

- `47dc7bf6-481b-487b-bd89-41420c6c58d7`

目标 topic：

- `dwd_frontier_flow_log_access_log_hi`

## 1. 先看样例结论

样例里 `localhost` 的 `/statistics` 返回：

```json
"dwd_frontier_flow_log_access_log_hi": [
  {
    "jobid": "47dc7bf6-481b-487b-bd89-41420c6c58d7",
    "groupid": "d9ab122e-6e90-44fb-a632-97c657793653"
  }
]
```

样例里 `localhost` 的 `/stable/topicstats` 也返回：

```json
"47dc7bf6-481b-487b-bd89-41420c6c58d7": {
  "res_tg_states": {
    "...": {
      "num_available": 6,
      "avalive_queue_length": 6144,
      "total_queue_length": 6144,
      "rows_per_sec": 59.155556
    }
  }
}
```

这说明：

- DAGReader 已经拿到这个 job
- SchedgroupManager 已经把它放进 topic `dwd_frontier_flow_log_access_log_hi`
- StateManager 已经建了 job/task/resource 状态
- Redis heartbeat/statsreport 运行态也有数据

所以问题不在“DAG 没解析”。

更准确的判断是：

```text
debug/statistics 能看到 DAG
        !=
getaddr HTTP 调度接口一定能按你的查询方式命中它
```

## 2. 三类 HTTP 接口不是同一个判断逻辑

```text
  /statistics
       |
       v
  直接 dump 内存:
  DAG cache + topic map + job states

  /{prefix}/topicstats?topic=...
       |
       v
  按 topic 查 SchedGroup.stats()

  /services?topic=...
  /{prefix}/services?topic=...
       |
       v
  按 topic 查 SchedGroup.get_addr()
  再执行调度策略
```

因此：

- `/statistics` 能看到 job，只代表内存里有 DAG 和 topic map
- `/topicstats` 能看到 job，只代表这个 topic 下有 job 的 runtime state
- `/services` 查不到，可能是 topic 参数不对，也可能是调度策略无法选出可用地址

## 3. 代码路径对比

### 3.1 `/statistics`

路由：

- `scheduler/src/server/mod.rs`
  - `warp::path("statistics")`

处理：

- `scheduler/src/server/statistics.rs`

它返回：

```rust
topics: sm.read().await.topicgroup_keys().await,
job_caches: dr.read().await.cache_statistics().await,
job_states: statemgr.jobstats_statistics().await,
```

含义：

- `topics` 来自 `SchedgroupManager.topic_groups`
- `job_caches` 来自 `DAGReader.cache`
- `job_states` 来自 `StateManager`

它不执行调度策略，也不关心请求 topic 是否和 getaddr 参数一致。

### 3.2 `/{prefix}/topicstats?topic=...`

路由：

- `scheduler/src/server/mod.rs`
  - `warp::path!(String / "topicstats")`

处理：

- `scheduler/src/server/topicstats.rs`

关键代码：

```rust
let topic = match p.get("topic") {
    Some(topic) => topic,
    None => ...
};

schedgroup_manager
    .read()
    .await
    .topic_stats(topic)
    .await
```

注意：

- prefix 例如 `stable` 会被捕获，但 handler 里参数名是 `_prefix`
- 当前代码不使用 prefix 做过滤
- 真正使用的只有 query 参数 `topic`

所以：

```bash
curl 'http://127.0.0.1:6789/stable/topicstats?topic=dwd_frontier_flow_log_access_log_hi'
```

和：

```bash
curl 'http://127.0.0.1:6789/xxx/topicstats?topic=dwd_frontier_flow_log_access_log_hi'
```

在当前代码里都会查同一个 topic。

### 3.3 `/services?topic=...`

路由：

- `scheduler/src/server/mod.rs`
  - `warp::path!("services")`
  - `warp::path!(String / "services")`

处理：

- `scheduler/src/server/get_addr.rs`

关键代码：

```rust
fn extract_topic(p: &HashMap<String, String>) -> Option<&str> {
    p.get("topic").map_or(None, |v| Some(v.as_str()))
}
```

也就是说 getaddr 只认：

```text
topic=...
```

或者批量：

```text
topics=a,b,c
```

它不认：

```text
jobid=...
name=...
connector.topic=...
```

之后进入：

```rust
SchedgroupManager::get_addr(topic, ...)
```

如果 topic 不存在：

```rust
Error::NotFound(format!(
    "the specified topic {} has no corresponding job group",
    topic
))
```

## 4. 这个样例里最可能的“查不到”原因

### 4.1 用错 topic 名

样例证明 scheduler 中存在的 topic 是：

```text
dwd_frontier_flow_log_access_log_hi
```

不是：

```text
dwd_frontier_flow_log_access_log_hi.pb
```

也不是 jobid：

```text
47dc7bf6-481b-487b-bd89-41420c6c58d7
```

Kafka protocol 的 topic 来源是 DAG subtask option：

```text
connector.topic
```

代码：

- `scheduler/src/schedgroup/mod.rs::DAGEvent::find_kafkaprotocol`

关键逻辑：

```rust
let topic = subtask.operator_options
    .get("connector.topic")
    ...
```

所以 HTTP getaddr 必须这样查：

```bash
curl -s 'http://127.0.0.1:6789/services?topic=dwd_frontier_flow_log_access_log_hi'
```

或者：

```bash
curl -s 'http://127.0.0.1:6789/stable/services?topic=dwd_frontier_flow_log_access_log_hi'
```

如果你查的是：

```bash
curl -s 'http://127.0.0.1:6789/services?topic=dwd_frontier_flow_log_access_log_hi.pb'
```

代码会去找 `topic_groups["dwd_frontier_flow_log_access_log_hi.pb"]`，自然找不到。

### 4.2 用 jobid 查 getaddr

`/statistics` 和 `/topicstats` 能显示 jobid，但 `/services` 调度入口不是按 jobid 查。

错误示例：

```bash
curl -s 'http://127.0.0.1:6789/services?jobid=47dc7bf6-481b-487b-bd89-41420c6c58d7'
```

这时 `extract_topic()` 拿不到 topic。

如果当前有多个 topic，`NotopicPolicy::Random` 可能随机返回别的 topic；如果策略是 `error`，则直接报 no topic。

### 4.3 请求打到了不同实例

样例是在 pod 内：

```bash
curl http://127.0.0.1:6789/...
```

这一定打到当前进程。

如果外部 HTTP 入口打不到，可能是：

- 外部网关转发到另一个 scheduler 实例
- 另一个实例加载了不同 `ENV`
- 另一个实例还没拉到这个 DAG
- 另一个实例 Redis 状态异常
- 外部网关改写了 path 或 query

代码层面上，scheduler 进程间不是共享内存的。每个实例都有自己的：

- `DAGReader.cache`
- `SchedgroupManager.topic_groups`
- `StateManager.job_states`

Redis 只共享 runtime state，不共享 `topic_groups` 本身。

所以外部入口必须在目标实例上也验证：

```bash
curl -s 'http://<external-host>:6789/statistics' \
  | jq '.topics["dwd_frontier_flow_log_access_log_hi"]'
```

如果 external `/statistics` 没有这个 job，而 localhost 有，说明不是同一个进程状态。

### 4.4 path 可以带 prefix，但必须是 `/prefix/services`

当前 getaddr 路由有两个：

```text
/services
/{prefix}/services
```

`/{prefix}/services` 的 prefix 会被 `execute_other(_prefix, ...)` 忽略。

所以：

```bash
/stable/services?topic=...
```

能进入 getaddr。

但下面这种不是 getaddr：

```bash
/stable/topicstats?topic=...
```

它只是查统计，不返回调度地址。

## 5. 如果 `/services` 仍然返回失败

如果 topic 正确，但 `/services` 仍然失败，需要看返回的 `err_msg`。

### 5.1 topic 不存在

响应类似：

```json
{
  "code": -1,
  "err_msg": "the specified topic xxx has no corresponding job group"
}
```

代码原因：

```rust
self.topic_groups.read().await.get(topic)
```

没找到。

排查：

```bash
curl -s 'http://127.0.0.1:6789/statistics' \
  | jq '.topics | keys'
```

确认 topic 字符串是否完全一致。

### 5.2 有 topic，但没有可用下游

响应可能包含：

```text
no valid downstream was found
```

代码原因：

- topic group 存在
- 但是调度策略内部没有可选 `ResTG`

对于默认 `load-only` 策略，代码会基于 StateManager 构建 `tg_states`。

如果 runtime state 没有可用实例，getaddr 会失败。

排查：

```bash
curl -s 'http://127.0.0.1:6789/stable/topicstats?topic=dwd_frontier_flow_log_access_log_hi' \
  | jq '.job_res_tg_stats["47dc7bf6-481b-487b-bd89-41420c6c58d7"].res_tg_states'
```

如果都是：

```json
"num_available": 0
```

说明 DAG 有，但没有可用 runtime。

但样例里 `num_available=6`，所以这条在样例中不是主因。

### 5.3 策略按 IDC/region 选不到

如果 DAG 使用 `idc-and-load` 策略，getaddr 还会依赖：

- 请求 IP
- `X-Forwarded-For`
- `X-Real-Ip`
- `x-vdc`
- `x-isp`
- `x-region`
- IP metadata / access map

同一个 topic：

- localhost curl 没带这些头
- 外部网关带了这些头

可能走到不同策略分支。

这不是 DAG 解析问题，而是调度策略选择问题。

## 6. 建议的验证命令

### 6.1 在同一个进程上验证三件事

```bash
export TOPIC='dwd_frontier_flow_log_access_log_hi'
export JOBID='47dc7bf6-481b-487b-bd89-41420c6c58d7'

curl -s 'http://127.0.0.1:6789/statistics' \
  | jq --arg topic "$TOPIC" --arg job "$JOBID" '{
      topic_entry: .topics[$topic],
      has_job_in_topic: ((.topics[$topic] | tostring) | contains($job)),
      has_job_cache: (.job_caches[$job] != null),
      has_job_state: (.job_states[$job] != null)
    }'

curl -s "http://127.0.0.1:6789/stable/topicstats?topic=$TOPIC" \
  | jq --arg job "$JOBID" '.job_res_tg_stats[$job]'

curl -s "http://127.0.0.1:6789/stable/services?topic=$TOPIC"
```

预期：

- `has_job_in_topic=true`
- `has_job_cache=true`
- `has_job_state=true`
- `topicstats` 返回该 job 的 `res_tg_states`
- `/stable/services` 返回 `code=0`

### 6.2 对比外部 HTTP 入口

```bash
export HOST='<external-host>'
export TOPIC='dwd_frontier_flow_log_access_log_hi'
export JOBID='47dc7bf6-481b-487b-bd89-41420c6c58d7'

curl -s "http://$HOST/statistics" \
  | jq --arg topic "$TOPIC" --arg job "$JOBID" '{
      topic_entry: .topics[$topic],
      has_job_in_topic: ((.topics[$topic] | tostring) | contains($job)),
      has_job_cache: (.job_caches[$job] != null),
      has_job_state: (.job_states[$job] != null)
    }'

curl -s "http://$HOST/stable/topicstats?topic=$TOPIC" \
  | jq --arg job "$JOBID" '.job_res_tg_stats[$job]'

curl -s "http://$HOST/stable/services?topic=$TOPIC"
```

如果 localhost 成功、external 失败，重点比较：

- external 是否打到同一个实例
- external `/statistics` 是否也有这个 topic/job
- external 是否被网关改写 query
- external 请求的 topic 是否多了 `.pb`
- external 请求是否缺少 `topic=` 参数
- external 是否带了导致策略分支变化的 headers

## 7. 一句话结论

这个样例已经证明：

```text
job 47dc7bf6-481b-487b-bd89-41420c6c58d7
已经被解析为 topic:
dwd_frontier_flow_log_access_log_hi
```

如果另一个 HTTP 接口“查不到”，从代码层面优先解释为：

```text
查询条件不一致：
  /statistics 按内存 DAG/job dump
  /topicstats 按 topic 查 stats
  /services 按 topic 执行调度

或者请求实例不一致：
  localhost 是当前进程
  external HTTP 可能是另一个进程/环境/网关路径
```

最常见错误是：

```text
用 jobid 查 /services
或用 dwd_frontier_flow_log_access_log_hi.pb 查，
但真实调度 topic 是 dwd_frontier_flow_log_access_log_hi
```
