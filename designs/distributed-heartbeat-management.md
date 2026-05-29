# 分布式心跳管理系统设计

## 结论

系统以 `POST /heartbeat` 作为主要接口，只支持 client 主动向 server 发布心跳。client 不需要监听端口，也暂时不暴露 `/kv`。

核心思路是固定会话层模式，把复杂能力放到可扩展的消息层：

- 会话层固定为 `client -> server` 的周期性 `/heartbeat` 请求。
- server 通过 `/heartbeat` 响应向 client 下发指令。
- client 通过后续 `/heartbeat` 及时回复指令执行状态。
- `/heartbeat_fwd` 仅在 server peers 之间转发状态类消息。
- group 可以聚合和转发心跳，也会转发心跳回复。
- 功能扩展优先通过消息类型实现，而不是增加新接口。

当前最小接口：

| 角色 | 接口 | 职责 |
| --- | --- | --- |
| server | `POST /heartbeat` | 接收 client 或 group 心跳，返回 server 下发消息 |
| server | `POST /heartbeat_fwd` | 在 peers 间转发状态类消息，只做状态合并 |

`GET /kv?k=?` 暂时屏蔽。client 不监听端口，避免部署、防火墙、服务发现和安全暴露的复杂度。

## 设计原则

- 接口尽量少，行为变化来自消息结构，不来自新接口。
- 写入接口幂等，方便失败重试。
- server 之间只追求最终一致性，不引入强一致协调协议。
- client 只做主动出站请求，不开放入站服务。
- `/heartbeat` 同时承载上行状态、上行回复和下行指令。
- `/heartbeat_fwd` 只转发状态，不转发控制指令。
- group 是转发与聚合层，不是状态权威。

## 核心角色

### Client

被管理的节点或服务实例。

- 周期性向 server 或 group 发送 `/heartbeat`。
- 维护本地 `client_id`、`epoch`、`seq`。
- 在心跳请求里上报本地状态、能力、指标和指令回复。
- 处理心跳响应里的 server 下发消息。
- 收到需要确认的指令后，可以立即触发一次额外心跳回复。
- 不监听端口，不提供 `/kv` 或其他入站接口。

### Server

心跳状态的接收、缓存和传播节点。

- 接收 `/heartbeat`（单条或批量），更新本地视图。
- 在 `/heartbeat` 响应里下发指令、配置或 redirect。
- 接收 client 在后续心跳中带回的指令回复。
- 通过 `/heartbeat_fwd` 将状态类消息转发给 peers。
- 根据 `ttl` 判断节点存活。

### Heartbeat Group

心跳聚合代理。本身不是新角色，只是一个也监听 `/heartbeat` 的进程。

- 接收 client 的 `/heartbeat`。
- 在聚合窗口内收集多个 client 的心跳。
- 窗口到期后，用同一个 `/heartbeat` 接口（`clients[]` 携带多条）发送给 server。
- 将 server 返回的下发消息拆分并返回给对应 client。
- 转发 client 后续心跳里的回复消息。
- 不做状态裁决，不做 peer 转发。

### Peer

同一个 server 集群中的其他 server。

- 接收 `/heartbeat_fwd`，按版本规则合并状态类消息。
- 不再次转发。
- 不通过 `/heartbeat_fwd` 接收或传播 server 下发指令。

## 会话层与消息层

### 会话层

会话层固定为 client 主动发起请求：

```text
client --POST /heartbeat--> server
client <--heartbeat response-- server
```

这个模式保持不变。即使需要 server 控制 client，也不让 server 反向连接 client，而是在下一次心跳响应里下发消息。

优点：

- client 实现简单，不需要启动 HTTP server。
- 更容易穿透 NAT、防火墙和容器网络边界。
- 安全暴露面更小，只需要保护 server 入站接口。
- server 不需要维护 client 地址可达性。

### 消息层

消息层承载丰富功能。所有功能通过 `messages[]` 和 `reply_to` 扩展。

- 上行消息：client 在 `/heartbeat` 请求中携带状态和回复。
- 下行消息：server 在 `/heartbeat` 响应中携带指令和配置。
- 状态转发：server 只通过 `/heartbeat_fwd` 转发 `state.*` 消息。

```text
+-------------------------------+
| 固定会话层: POST /heartbeat   |
+-------------------------------+
                 |
                 v
+-------------------------------+
| 可扩展消息层: state/cmd/reply |
+-------------------------------+
```

## 最小 API

### `POST /heartbeat`

由 client 或 group 调用 server。也由 client 调用 group。

请求：

```json
{
  "client_id": "client-1",
  "epoch": 1,
  "seq": 42,
  "ttl_ms": 15000,
  "messages": [
    {
      "message_id": "msg-client-1-42-state",
      "type": "state.heartbeat",
      "version": 1,
      "payload": {
        "status": "alive",
        "load": "0.42",
        "zone": "az-a",
        "role": "worker"
      }
    },
    {
      "message_id": "msg-client-1-41-ack",
      "type": "reply.command_result",
      "version": 1,
      "reply_to": "cmd-server-a-1001",
      "payload": {
        "status": "ok",
        "detail": "config applied"
      }
    }
  ]
}
```

- `messages` 是可扩展消息数组。
- client 至少发送 `state.heartbeat`。
- 如果 client 收到过需要确认的下发指令，应在后续心跳中携带 `reply.*`。

响应：

```json
{
  "ok": true,
  "server_id": "server-a",
  "accepted_seq": 42,
  "messages": [
    {
      "message_id": "cmd-server-a-1002",
      "type": "cmd.update_config",
      "version": 1,
      "deadline_ms": 1710000005000,
      "payload": {
        "config_version": "v2",
        "heartbeat_interval_ms": 3000
      }
    }
  ]
}
```

- server 可以返回空 `messages[]`，表示无指令。
- server 通过响应消息下发指令、配置、redirect 或节流策略。
- client 处理下行消息后，在下一次或更早一次心跳里携带回复。
- 不在 `/heartbeat` 中增加复杂动作参数，动作语义由消息类型定义。

幂等规则：

- 同一个 `client_id`，`epoch` 更大者优先。
- `epoch` 相同，`seq` 更大者优先。
- `message_id` 全局唯一或在 `client_id` 内唯一。
- 重复 `message_id` 不重复执行。
- 回复消息通过 `reply_to` 关联下发指令。

### 批量 `POST /heartbeat`

当 group 聚合多个 client 心跳时，仍使用同一个接口，只是请求结构使用 `clients[]`。

```json
{
  "group_id": "group-a",
  "clients": [
    {
      "client_id": "client-1",
      "epoch": 1,
      "seq": 42,
      "ttl_ms": 15000,
      "messages": [
        {
          "message_id": "msg-client-1-42-state",
          "type": "state.heartbeat",
          "version": 1,
          "payload": {"status": "alive"}
        }
      ]
    }
  ]
}
```

server 响应按 `client_id` 返回下发消息：

```json
{
  "ok": true,
  "server_id": "server-a",
  "clients": [
    {
      "client_id": "client-1",
      "accepted_seq": 42,
      "messages": [
        {
          "message_id": "cmd-server-a-1002",
          "type": "cmd.update_config",
          "version": 1,
          "payload": {"heartbeat_interval_ms": 3000}
        }
      ]
    }
  ]
}
```

group 必须把对应 `client_id` 的响应消息返回给对应 client。client 的回复消息继续经 group 转发给 server。

### `POST /heartbeat_fwd`

由 server 调用 peer server。

请求：

```json
{
  "source_server_id": "server-a",
  "states": [
    {
      "client_id": "client-1",
      "epoch": 1,
      "seq": 42,
      "ttl_ms": 15000,
      "observed_at_ms": 1710000000000,
      "messages": [
        {
          "message_id": "msg-client-1-42-state",
          "type": "state.heartbeat",
          "version": 1,
          "payload": {"status": "alive", "load": "0.42"}
        }
      ]
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

- 只转发 `state.*` 这类状态消息。
- 不转发 `cmd.*` 指令消息。
- 不转发 `reply.*` 回复消息，除非回复被建模为可复制状态摘要。
- 不再次向其他 peers 转发。
- 使用与 `/heartbeat` 相同的版本合并规则。

### `GET /kv?k=?`

暂时屏蔽。

client 不监听端口，因此不提供 `/kv`。后续如果需要读取 client 扩展信息，优先让 client 通过 `state.*` 消息主动上报，或者让 server 通过 `cmd.sample` 要求 client 在下一次心跳中返回采样结果。

## 消息类型

消息类型按前缀分层：

| 前缀 | 方向 | 是否可 `/heartbeat_fwd` | 用途 |
| --- | --- | --- | --- |
| `state.*` | client -> server | 是 | 存活、负载、版本、能力等状态 |
| `cmd.*` | server -> client | 否 | 配置更新、立即心跳、采样、降级等指令 |
| `reply.*` | client -> server | 否 | 指令执行结果、错误、进度 |
| `control.*` | server -> client | 否 | redirect、节流、心跳周期调整 |

推荐从少量消息开始：

| 类型 | 说明 |
| --- | --- |
| `state.heartbeat` | 基础存活状态 |
| `state.metrics` | 简单指标，如 load、memory、qps |
| `state.capability` | client 支持的消息版本和能力 |
| `cmd.update_config` | 下发配置变更 |
| `cmd.report_now` | 要求 client 尽快发送一次心跳回复 |
| `cmd.sample` | 要求 client 采样本地信息 |
| `reply.command_result` | 指令执行结果 |
| `control.redirect` | 引导 client 使用 group |
| `control.throttle` | 调整心跳间隔或消息上报频率 |

## 指令下发与回复

server 不直接连接 client，而是在 `/heartbeat` 响应中下发指令。

```text
+----------+                         +----------+
| client   |                         | server   |
+----+-----+                         +----+-----+
     |                                    |
     | POST /heartbeat: state            |
     |----------------------------------->|
     |                                    |
     | response: cmd.update_config        |
     |<-----------------------------------|
     |                                    |
     | apply command                      |
     |                                    |
     | POST /heartbeat: reply.result      |
     |----------------------------------->|
     |                                    |
```

及时回复策略：

- client 收到需要确认的 `cmd.*` 后，可以立即触发一次额外 `/heartbeat`。
- 如果指令可异步执行，client 先回复 `accepted`，完成后再回复 `ok` 或 `failed`。
- server 通过 `message_id`、`reply_to` 和超时判断指令是否完成。
- 指令重复下发时，client 根据 `message_id` 去重。

回复状态建议：

| 状态 | 含义 |
| --- | --- |
| `accepted` | 已收到，准备执行 |
| `running` | 正在执行 |
| `ok` | 执行成功 |
| `failed` | 执行失败 |
| `unsupported` | client 不支持该消息类型或版本 |

## Group 转发模型

### 普通心跳转发

```text
client --/heartbeat--> group --/heartbeat(clients[])--> server
```

### 下行消息转发

```text
server --response.clients[].messages[]--> group --response.messages[]--> client
```

### 回复消息转发

```text
client --/heartbeat(reply.*)--> group --/heartbeat(clients[].messages[])--> server
```

关键规则：

- group 必须保持请求与响应的 `client_id` 映射。
- group 不解释业务指令，只按 client 维度转发。
- group 可以聚合 `state.*` 和 `reply.*` 上行消息。
- group 不调用 `/heartbeat_fwd`。
- group 失败时，client 可以回退直连 server。

## 状态模型

server 本地维护节点视图和消息账本：

```text
NodeState {
  client_id: string
  epoch: uint64
  seq: uint64
  ttl_ms: uint64
  observed_at_ms: int64
  expire_at_ms: int64
  source: string          // "direct" | "group" | peer server_id
  state: map<string, any>
}

MessageLedger {
  message_id: string
  client_id: string
  type: string
  direction: string       // "up" | "down"
  status: string          // "pending" | "acked" | "done" | "expired"
  created_at_ms: int64
  updated_at_ms: int64
}
```

- `now <= expire_at_ms`：`Alive`
- `now > expire_at_ms`：`Suspect` / `Expired`
- `state.*` 更新 `NodeState`
- `cmd.*` 和 `reply.*` 更新 `MessageLedger`

## 心跳流程

### 直连路径

```text
+----------+
| client   |
+----+-----+
     |
     | /heartbeat(state.*, reply.*)
     v
+----------+
| server-a |
+----+-----+
     |
     | response(cmd.*, control.*)
     v
+----------+
| client   |
+----------+
```

server 再异步转发状态：

```text
+----------+
| server-a |
+----+-----+
     |
     | /heartbeat_fwd(state.*)
     v
+----------+
| server-b |
+----------+
```

### 聚合路径（redirect）

当 server 判断心跳频率过高时，可以通过 `control.redirect` 引导 client 使用 group。

```text
+----------+
| client   |
+----+-----+
     |
     | /heartbeat(state.*, reply.*)
     v
+----------+
| server-a |
+----+-----+
     |
     | response: control.redirect
     | group_addr: group-a:9000
     v

  === client 下次心跳 ===

+----------+
| client   |
+----+-----+
     |
     | /heartbeat(state.*, reply.*)
     v
+----------+
| group-a  |
| 聚合窗口  |
+----+-----+
     |
     | /heartbeat(clients[])
     v
+----------+
| server-a |
+----+-----+
     |
     | response clients[].messages[]
     v
+----------+
| group-a  |
+----+-----+
     |
     | response messages[]
     v
+----------+
| client   |
+----------+
```

server 仍只将状态类消息同步到 peers：

```text
+----------+
| server-a |
+----+-----+
     |
     | /heartbeat_fwd(state.*)
     v
+----------+
| server-b |
+----------+
```

关键点：

- client 与 group、server 的主路径全程只用 `/heartbeat`。
- client 发送单 client 心跳；group 聚合后发送 `clients[]`。
- server 不关心请求来源是 client 还是 group，统一按消息语义处理。
- group 需要把 server 返回的下行消息转给对应 client。
- redirect 是优化路径，不是正确性依赖。group 失败时 client 回退直连。

### Client 行为状态机

```text
+-------------------+
| Direct Mode       |
| 发送到 server     |
+--------+----------+
         |
         | 收到响应
         v
+-------------------+---- no ----->[ 保持 Direct Mode ]
| 响应含 redirect?  |
+-------------------+
         | yes
         v
+-------------------+
| Redirect Mode     |
| 发送到 group_addr |
+--------+----------+
         |
         +---- 收到 cmd.*? --------->[ 处理后触发 reply 心跳 ]
         |
         +---- group 不可达? ------->[ 回退 Direct Mode ]
         |
         +---- redirect ttl 到期? -->[ 回退 Direct Mode ]
```

### Group 行为

```text
+----------------------------+
| 收到 /heartbeat            |
+----------------------------+
             |
             v
+----------------------------+
| 写入本地聚合窗口            |
+----------------------------+
             |
             v
+----------------------------+---- yes ----->+----------------------------+
| 达到 batch_size            |               | 发送 /heartbeat(clients[]) |
| 或 flush_interval 到期?    |               | 到 server                  |
+----------------------------+               +----------------------------+
             | no
             v
       [ 继续等待 ]
```

group 收到 server 响应后，必须按 `client_id` 拆分 `clients[].messages[]`，再把对应消息放进原始 client 的 `/heartbeat` 响应。

## 最终一致性策略

server 之间不选主，不做 quorum 写入。

收敛依赖：

- client 周期性发送 `/heartbeat`（直连或经 group）。
- group 聚合后仍通过 `/heartbeat` 转发。
- server 对 peers 异步发送 `/heartbeat_fwd`。
- `/heartbeat_fwd` 只传播 `state.*`。
- 所有合并使用 `epoch + seq` 确定性规则。
- 旧状态不会覆盖新状态。

推荐默认参数：

| 参数 | 建议值 | 说明 |
| --- | --- | --- |
| `heartbeat_interval_ms` | `5000` | client 心跳周期 |
| `urgent_heartbeat_delay_ms` | `100` | 收到需确认指令后触发额外心跳的延迟 |
| `ttl_ms` | `15000` | 允许 3 个周期抖动 |
| `redirect_ttl_ms` | `60000` | client 使用 group address 的有效期 |
| `group_flush_interval_ms` | `1000` | group 聚合窗口最大等待时间 |
| `group_flush_batch_size` | `1000` | group 单批 client 数量上限 |
| `command_timeout_ms` | `30000` | server 等待指令回复的默认超时 |
| `forward_retry` | `3` | peer 转发失败重试次数 |
| `peer_timeout_ms` | `1000` | 单次 peer 请求超时 |

## 失败处理

| 场景 | 处理 |
| --- | --- |
| client -> server 失败 | 重试或切换 server，幂等无副作用 |
| client -> group 失败 | 回退到直连 server |
| group -> server 失败 | group 重试，依赖幂等合并 |
| group 返回失败 | client 下次心跳可重发未确认 `reply.*` |
| server -> peer 失败 | 后台重试，不影响 client 返回 |
| 指令回复超时 | server 标记 `expired`，可重发或人工处理 |
| client 重启 | 递增 `epoch`，覆盖旧进程残留 |
| 消息版本不支持 | client 回复 `unsupported` |
| redirect ttl 到期 | client 回到 Direct Mode |

## 非目标

- 不实现强一致成员管理。
- 不实现 leader election。
- 不保证心跳写入后所有 server 立即可见。
- 不要求所有 client 必须经过 group。
- 不把 group 设计成状态权威。
- 不要求 client 监听端口。
- 暂时不启用 `/kv`。
- 不通过 `/heartbeat_fwd` 转发指令类消息。

## 小结

这个设计把系统复杂度放在消息层，而不是接口层：

- `/heartbeat` 是主接口，承载 client 上行状态、上行回复和 server 下行指令。
- `/heartbeat_fwd` 只做状态类消息的 peer 间传播。
- client 不监听端口，部署和安全模型更简单。
- group 负责聚合与转发，包括转发心跳回复。
- 后续能力通过 `state.*`、`cmd.*`、`reply.*`、`control.*` 扩展。
