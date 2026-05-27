# 分布式心跳管理系统设计初稿

## 结论

这个系统的核心目标是用尽可能少的原子接口，完成 client 存活上报、server 间最终一致性同步，以及 client 扩展查询能力。

系统不提供一个复杂的“节点管理大接口”，而是把能力拆成三个简单接口：

| 角色 | 接口 | 职责 |
| --- | --- | --- |
| server | `POST /heartbeat` | 接收 client 心跳，写入本地状态，并异步转发给 peers |
| server | `POST /heartbeat_fwd` | 接收 peer 转发的心跳，只合并状态，不再次转发 |
| client | `GET /kv?k=?` | 暴露简单 key-value 查询能力，用于后续扩展服务 |

client 与 server 的通信是可选的。client 可以周期性向一个或多个 server 发送 `/heartbeat`，也可以只暴露 `/kv`，由外部系统按需发现和调用。

## 设计原则

- 接口尽量少，每个接口只做一件事。
- 写入接口必须尽量幂等，方便失败重试。
- server 之间只追求最终一致性，不引入强一致协调协议。
- client 不依赖单个 server，server 也不要求全局 leader。
- 复杂能力通过上层组合实现，不在底层接口里塞过多参数。

## 核心角色

### Client

client 是被管理的节点或服务实例。

职责：

- 可选地周期性向 server 发送 `/heartbeat`。
- 暴露 `/kv?k=?`，让其他系统读取扩展信息。
- 自己维护本地 `node_id`、`epoch`、`seq` 等心跳版本信息。

### Server

server 是心跳状态的接收、缓存和传播节点。

职责：

- 接收 client 心跳并更新本地视图。
- 通过 `/heartbeat_fwd` 把心跳转发到 peers。
- 接收 peers 转发的心跳并做版本合并。
- 根据 `ttl` 或 `expire_at` 判断节点是否可能离线。

### Peer

peer 是同一个 server 集群中的其他 server。

职责：

- 接收转发心跳。
- 按版本规则合并状态。
- 不对 `/heartbeat_fwd` 再次转发，避免循环风暴。

## 最小 API

### `POST /heartbeat`

由 client 调用 server。

请求：

```json
{
  "node_id": "client-1",
  "epoch": 1,
  "seq": 42,
  "ttl_ms": 15000,
  "addr": "10.0.0.12:8080",
  "meta": {
    "zone": "az-a",
    "role": "worker"
  }
}
```

响应：

```json
{
  "ok": true,
  "server_id": "server-a",
  "observed_at_ms": 1710000000000
}
```

职责边界：

- 只负责接收和记录心跳。
- 可以异步转发到 peers。
- 不负责保证所有 peers 立刻可见。
- 不负责调用 client 的 `/kv`。

幂等规则：

- 同一个 `node_id` 下，`epoch` 更大者优先。
- `epoch` 相同，则 `seq` 更大者优先。
- `epoch` 和 `seq` 都相同，则重复请求直接返回成功。

### `POST /heartbeat_fwd`

由 server 调用 peer server。

请求：

```json
{
  "source_server_id": "server-a",
  "node_id": "client-1",
  "epoch": 1,
  "seq": 42,
  "ttl_ms": 15000,
  "addr": "10.0.0.12:8080",
  "meta": {
    "zone": "az-a",
    "role": "worker"
  },
  "observed_at_ms": 1710000000000
}
```

响应：

```json
{
  "ok": true,
  "server_id": "server-b",
  "merged": true
}
```

职责边界：

- 只负责合并 peer 转发来的心跳。
- 不再次向其他 peers 转发。
- 不阻塞等待其他 server 确认。
- 不把 peer 转发失败视为 client 心跳失败。

幂等规则：

- 使用与 `/heartbeat` 相同的版本比较规则。
- 旧版本心跳可以返回 `ok=true, merged=false`。

### `GET /kv?k=?`

由 client 暴露，供外部系统或 server 按需读取扩展信息。

请求示例：

```text
GET /kv?k=load
```

响应示例：

```json
{
  "ok": true,
  "key": "load",
  "value": "0.42"
}
```

职责边界：

- 只提供简单 key-value 读取。
- 不承担心跳写入职责。
- 不要求 server 在心跳路径上同步调用。
- key 的语义由上层业务定义。

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
  source_server_id: string
}
```

状态判断：

- `now <= expire_at_ms`：节点视为 `Alive`。
- `now > expire_at_ms`：节点视为 `Suspect` 或 `Expired`。
- 具体是否删除过期状态由后台清理任务决定，不放进写入接口。

## 心跳写入流程

```text
+------------------+
| client           |
+------------------+
          |
          | POST /heartbeat
          v
+------------------+
| server-a         |
| merge local view |
+------------------+
          |
          | async POST /heartbeat_fwd
          v
+------------------+
| server-b/c       |
| merge peer view  |
+------------------+
```

关键点：

- client 只需要确认一个 server 接收成功。
- server 本地写入成功后即可返回。
- peer 转发可以异步执行并重试。
- peer 短暂不可用时，系统进入临时不一致状态，恢复后继续收敛。

## 最终一致性策略

server 之间不选主，不做 quorum 写入。

收敛依赖：

- client 周期性重复发送 `/heartbeat`。
- server 对 peers 周期性或按写入事件转发 `/heartbeat_fwd`。
- peer 使用 `epoch + seq` 做确定性合并。
- 旧心跳不会覆盖新心跳。

推荐默认参数：

| 参数 | 建议值 | 说明 |
| --- | --- | --- |
| `heartbeat_interval_ms` | `5000` | client 心跳周期 |
| `ttl_ms` | `15000` | 允许 3 个周期抖动 |
| `forward_retry` | `3` | peer 转发失败重试次数 |
| `peer_timeout_ms` | `1000` | 单次 peer 请求超时 |

## 失败处理

### Client 到 Server 失败

- client 可以重试同一个 server。
- client 可以切换到另一个 server。
- 重试不会产生副作用，因为 `/heartbeat` 按版本幂等。

### Server 到 Peer 失败

- server 记录转发失败并后台重试。
- 不影响 `/heartbeat` 对 client 返回成功。
- 如果 peer 长时间不可用，peer 本地视图可能过期，恢复后通过后续心跳重新收敛。

### Client 重启

- client 重启后递增 `epoch`。
- 新 `epoch` 可以覆盖旧进程残留的高 `seq`。
- 如果无法持久化 `epoch`，可以用启动时间或随机 incarnation id 替代。

## 扩展方式

基础接口保持不变，扩展能力放到组合层：

| 目标 | 组合方式 |
| --- | --- |
| 读取节点负载 | 先从 server 发现 `addr`，再访问 client `/kv?k=load` |
| 获取节点版本 | 访问 client `/kv?k=version` |
| 简单服务发现 | server 暴露只读列表接口时，返回未过期 `NodeState` |
| 节点摘除 | 上层停止 client 心跳，等待 ttl 过期 |

如果后续需要服务发现查询，可以新增只读接口，例如：

```text
GET /nodes
GET /nodes/{node_id}
```

这类接口应保持只读，不隐式触发修复或转发。

## 非目标

- 不实现强一致成员管理。
- 不实现 leader election。
- 不保证心跳写入后所有 server 立即可见。
- 不在 `/heartbeat` 中内置复杂健康检查。
- 不要求 server 同步调用 client `/kv`。

## 待确认问题

- server peer 列表是静态配置，还是后续由配置中心提供。
- client 是否需要持久化 `epoch`，避免重启后版本回退。
- `meta` 是否限制大小，避免心跳请求被滥用。
- server 是否需要暴露只读查询接口，还是只作为内部状态组件。

## 小结

这个设计优先保证接口简单：

- `/heartbeat` 负责 client 到 server 的存活写入。
- `/heartbeat_fwd` 负责 server 到 peer 的最终一致性传播。
- `/kv?k=?` 负责 client 侧扩展信息读取。

复杂能力不放进底层 API，而是通过周期心跳、异步转发、版本合并和上层查询组合出来。
