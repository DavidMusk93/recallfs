# 分布式心跳管理系统设计

## 结论

用尽可能少的接口，完成 client 存活上报、高频心跳聚合、server 间最终一致性同步。

系统只有三个接口：

| 角色 | 接口 | 职责 |
| --- | --- | --- |
| server | `POST /heartbeat` | 接收心跳（单条或批量），写入本地状态，可返回 redirect |
| server | `POST /heartbeat_fwd` | 接收 peer 转发的心跳，只合并状态，不再次转发 |
| client | `GET /kv?k=?` | 暴露简单 key-value 查询能力 |

**不增加新接口**。聚合能力通过 `/heartbeat` 数据结构中的 `nodes[]` 和 `redirect` 字段实现，client、group、server 使用同一个接口。

## 设计原则

- 接口尽量少，行为变化来自数据结构，不来自新接口。
- 写入接口幂等，方便失败重试。
- server 之间只追求最终一致性，不引入强一致协调协议。
- client 不依赖单个 server，server 不要求全局 leader。

## 核心角色

### Client

被管理的节点或服务实例。

- 周期性向 server（或 group）发送 `/heartbeat`。
- 维护本地 `node_id`、`epoch`、`seq`。
- 收到响应中的 `redirect` 后，下次心跳发送到 `group_addr`。

### Server

心跳状态的接收、缓存和传播节点。

- 接收 `/heartbeat`（单条或批量），更新本地视图。
- 可在响应中返回 `redirect`，引导 client 到 heartbeat group。
- 通过 `/heartbeat_fwd` 把心跳转发到 peers。
- 根据 `ttl` 判断节点存活。

### Heartbeat Group

心跳聚合代理。本身不是新角色，只是一个也监听 `/heartbeat` 的进程。

- 接收 client 的 `/heartbeat`。
- 在聚合窗口内收集多个 client 的心跳。
- 窗口到期后，用同一个 `/heartbeat` 接口（`nodes[]` 携带多条）发送给 server。
- 不做状态裁决，不做 peer 转发。

### Peer

同一个 server 集群中的其他 server。

- 接收 `/heartbeat_fwd`，按版本规则合并。
- 不再次转发。

## 最小 API

### `POST /heartbeat`

由 client 或 group 调用 server。也由 client 调用 group。

请求：

```json
{
  "nodes": [
    {
      "node_id": "client-1",
      "epoch": 1,
      "seq": 42,
      "ttl_ms": 15000,
      "addr": "10.0.0.12:8080",
      "meta": {"zone": "az-a", "role": "worker"}
    }
  ]
}
```

- `nodes` 是数组。client 直连时通常只有 1 条；group 聚合后可能有数百条。
- 单条时也用 `nodes[1]`，保持结构统一。

响应：

```json
{
  "ok": true,
  "server_id": "server-a",
  "accepted": 1,
  "redirect": {
    "group_addr": "group-a:9000",
    "ttl_ms": 60000
  }
}
```

- `redirect` 是可选字段。server 决定是否下发。
- client 收到后，在 `ttl_ms` 内优先向 `group_addr` 发送心跳。
- group 不可用时，client 回退到 server 地址。

幂等规则：

- 同一个 `node_id`，`epoch` 更大者优先。
- `epoch` 相同，`seq` 更大者优先。
- 版本相同，重复请求直接返回成功。

### `POST /heartbeat_fwd`

由 server 调用 peer server。

请求：

```json
{
  "source_server_id": "server-a",
  "nodes": [
    {
      "node_id": "client-1",
      "epoch": 1,
      "seq": 42,
      "ttl_ms": 15000,
      "addr": "10.0.0.12:8080",
      "meta": {"zone": "az-a", "role": "worker"},
      "observed_at_ms": 1710000000000
    }
  ]
}
```

响应：

```json
{
  "ok": true,
  "server_id": "server-b",
  "accepted": 1,
  "merged": 1
}
```

- 不再次向其他 peers 转发。
- 使用与 `/heartbeat` 相同的版本合并规则。

### `GET /kv?k=?`

由 client 暴露，供外部系统按需读取扩展信息。

```text
GET /kv?k=load  ->  {"ok": true, "key": "load", "value": "0.42"}
```

## 状态模型

server 本地维护一张节点视图：

```text
NodeState {
  node_id: string
  epoch: uint64
  seq: uint64
  addr: string
  meta: map<string, string>
  ttl_ms: uint64
  observed_at_ms: int64
  expire_at_ms: int64
  source: string          // "direct" | "group" | peer server_id
}
```

- `now <= expire_at_ms`：`Alive`
- `now > expire_at_ms`：`Suspect` / `Expired`

## 心跳流程

### 直连路径

```text
+------------+                +------------+                +------------+
| client     |--- /heartbeat -->| server-a   |--- /heartbeat_fwd -->| server-b   |
+------------+                +------------+                +------------+
```

### 聚合路径（redirect）

当 server 判断心跳频率过高时，在响应中返回 `redirect`。

```text
+------------+                +------------+
| client     |--- /heartbeat -->| server-a   |
+------------+                +------+-----+
                                     |
                              response: redirect
                              group_addr: group-a:9000
                                     |
                                     v
+------------+                +------------+                +------------+
| client     |--- /heartbeat -->| group-a    |--- /heartbeat -->| server-a   |
| (next beat)|                | (aggregate)|  (nodes[N])   |            |
+------------+                +------------+                +------+-----+
                                                                  |
                                                           /heartbeat_fwd
                                                                  |
                                                                  v
                                                           +------------+
                                                           | server-b   |
                                                           +------------+
```

关键点：

- 全程只用 `/heartbeat`，数据结构决定行为。
- client 发送时 `nodes[1]`；group 聚合后发送时 `nodes[N]`。
- server 不关心请求来源是 client 还是 group，统一按 `nodes[]` 处理。
- redirect 是优化路径，不是正确性依赖。group 失败时 client 回退直连。

### Client 行为状态机

```text
+-------------+
| Direct Mode |<----------------------------------+
+------+------+                                   |
       |                                          |
       | send /heartbeat to server                |
       v                                          |
+------+------+                                   |
| got redirect|---- no --->[ stay Direct Mode ]    |
+------+------+                                   |
       | yes                                      |
       v                                          |
+------+--------+                                 |
| Redirect Mode |                                 |
| send to group |                                 |
+------+--------+                                 |
       |                                          |
       +--- group unreachable? ----->[ fallback ]-+
       |
       +--- redirect ttl expired? -->[ fallback ]-+
```

### Group 行为

```text
+-------------------------------+
| 收到 /heartbeat from client   |
+-------------------------------+
              |
              v
+-------------------------------+
| 写入本地聚合窗口               |
+-------------------------------+
              |
              v
+-------------------------------+---- flush 条件 ----->+---------------------------+
| 窗口未满 且 时间未到?          |                      | 发送 /heartbeat(nodes[N]) |
+-------------------------------+                      | 到 server                |
                                                       +---------------------------+
```

flush 条件：窗口内心跳数达到 `batch_size`，或等待时间达到 `flush_interval`。

## 最终一致性策略

server 之间不选主，不做 quorum 写入。

收敛依赖：

- client 周期性发送 `/heartbeat`（直连或经 group）。
- server 对 peers 异步转发 `/heartbeat_fwd`。
- 所有合并使用 `epoch + seq` 确定性规则。
- 旧心跳不会覆盖新心跳。

推荐默认参数：

| 参数 | 建议值 | 说明 |
| --- | --- | --- |
| `heartbeat_interval_ms` | `5000` | client 心跳周期 |
| `ttl_ms` | `15000` | 允许 3 个周期抖动 |
| `redirect_ttl_ms` | `60000` | client 使用 group address 的有效期 |
| `group_flush_interval_ms` | `1000` | group 聚合窗口最大等待时间 |
| `group_flush_batch_size` | `1000` | group 单批心跳数量上限 |
| `forward_retry` | `3` | peer 转发失败重试次数 |
| `peer_timeout_ms` | `1000` | 单次 peer 请求超时 |

## 失败处理

| 场景 | 处理 |
| --- | --- |
| client -> server 失败 | 重试或切换 server，幂等无副作用 |
| client -> group 失败 | 回退到直连 server |
| group -> server 失败 | group 重试，依赖幂等合并 |
| server -> peer 失败 | 后台重试，不影响 client 返回 |
| client 重启 | 递增 `epoch`，覆盖旧进程残留 |
| redirect ttl 到期 | client 回到 Direct Mode |

## 非目标

- 不实现强一致成员管理。
- 不实现 leader election。
- 不保证心跳写入后所有 server 立即可见。
- 不要求所有 client 必须经过 group。
- 不把 group 设计成状态权威。

## 小结

- 只用 `/heartbeat` + `/heartbeat_fwd` + `/kv` 三个接口。
- 聚合能力来自数据结构（`nodes[]` + `redirect`），不来自新接口。
- client、group、server 的行为变化，全部由心跳请求/响应中的字段驱动。
- 简单才容易维护。
