# Pulse 分布式心跳管理系统设计

## 结论

系统以 `POST /heartbeat` 作为主要接口，只支持 agent 主动向 coordinator 发布心跳。agent 不需要监听端口，也暂时不暴露 `/kv`。

核心思路是固定会话层模式，把复杂能力放到可扩展的消息层：

- 会话层固定为 `agent -> coordinator` 的周期性 `/heartbeat` 请求。
- coordinator 通过 `/heartbeat` 响应向 agent 下发指令。
- agent 通过后续 `/heartbeat` 及时回复指令执行状态。
- `/heartbeat_fwd` 仅在 coordinator peers 之间转发状态类消息。
- group 可以聚合和转发心跳，也会转发心跳回复。
- 功能扩展优先通过消息类型实现，而不是增加新接口。

当前最小接口：

| 角色 | 接口 | 职责 |
| --- | --- | --- |
| coordinator | `POST /heartbeat` | 接收 agent 或 group 心跳，返回 coordinator 下发消息 |
| coordinator | `POST /heartbeat_fwd` | 在 peers 间转发状态类消息，只做状态合并 |

`GET /kv?k=?` 暂时屏蔽。agent 不监听端口，避免部署、防火墙、服务发现和安全暴露的复杂度。

## 设计原则

- 接口尽量少，行为变化来自消息结构，不来自新接口。
- 写入接口幂等，方便失败重试。
- coordinator 之间只追求最终一致性，不引入强一致协调协议。
- agent 只做主动出站请求，不开放入站服务。
- `/heartbeat` 同时承载上行状态、上行回复和下行指令。
- `/heartbeat_fwd` 只转发状态，不转发控制指令。
- group 是转发与聚合层，不是状态权威。

## 实体设计与命名

系统名定为 `Pulse`。

`Pulse` 这个名字是优雅的：它天然对应“脉搏、心跳、节律”，能直接表达系统的核心职责，也不会把系统限制成单纯的健康检查工具。后续即使扩展到指令下发、状态采样、配置同步、轻量控制面，`Pulse` 仍然成立，因为这些能力都可以理解为伴随心跳节律流动的消息。

核心实体命名：

| 旧称 | 新称 | 定位 |
| --- | --- | --- |
| system | `Pulse` | 分布式心跳与消息协调系统 |
| server | `Coordinator` | 状态汇聚、消息编排、peer 同步节点 |
| client | `Agent` | 被管理实例上的轻量心跳与执行进程 |
| group | `Pulse Group` | 可选的心跳聚合与消息转发代理 |
| peer | `Peer Coordinator` | 同一集群内的其他 coordinator |

命名取舍：

- `Coordinator` 比 `Server` 更准确，因为它不仅接收请求，还负责状态合并、消息编排、指令下发和 peer 同步。
- `Agent` 比 `Client` 更准确，因为它运行在被管理节点上，既上报状态，也执行 coordinator 下发的消息。
- `Pulse Group` 保留 `Group` 的聚合含义，同时加上系统前缀，避免和普通业务 group 混淆。
- `Peer Coordinator` 明确 peer 是 coordinator 之间的关系，不把 peer 误解成 agent。

建议在代码和协议里统一使用 `agent_id`、`coordinator_id`、`group_id`、`source_coordinator_id`，避免新旧术语混用。

## 核心角色

### Agent

被管理的节点或服务实例。

- 周期性向 coordinator 或 group 发送 `/heartbeat`。
- 维护本地 `agent_id`、`epoch`、`seq`。
- 在心跳请求里上报本地状态、能力、指标和指令回复。
- 处理心跳响应里的 coordinator 下发消息。
- 收到需要确认的指令后，可以立即触发一次额外心跳回复。
- 不监听端口，不提供 `/kv` 或其他入站接口。

### Coordinator

心跳状态的接收、缓存和传播节点。

- 接收 `/heartbeat`（单条或批量），更新本地视图。
- 在 `/heartbeat` 响应里下发指令、配置或 redirect。
- 接收 agent 在后续心跳中带回的指令回复。
- 通过 `/heartbeat_fwd` 将状态类消息转发给 peers。
- 根据 `ttl` 判断节点存活。

### Pulse Group

心跳聚合代理。本身不是新角色，只是一个也监听 `/heartbeat` 的进程。

- 接收 agent 的 `/heartbeat`。
- 在聚合窗口内收集多个 agent 的心跳。
- 窗口到期后，用同一个 `/heartbeat` 接口（`agents[]` 携带多条）发送给 coordinator。
- 将 coordinator 返回的下发消息拆分并返回给对应 agent。
- 转发 agent 后续心跳里的回复消息。
- 不做状态裁决，不做 peer 转发。

### Peer

同一个 coordinator 集群中的其他 coordinator。

- 接收 `/heartbeat_fwd`，按版本规则合并状态类消息。
- 不再次转发。
- 不通过 `/heartbeat_fwd` 接收或传播 coordinator 下发指令。

## 会话层与消息层

### 会话层

会话层固定为 agent 主动发起请求：

```text
agent --POST /heartbeat--> coordinator
agent <--heartbeat response-- coordinator
```

这个模式保持不变。即使需要 coordinator 控制 agent，也不让 coordinator 反向连接 agent，而是在下一次心跳响应里下发消息。

优点：

- agent 实现简单，不需要启动 HTTP 服务。
- 更容易穿透 NAT、防火墙和容器网络边界。
- 安全暴露面更小，只需要保护 coordinator 入站接口。
- coordinator 不需要维护 agent 地址可达性。

### 消息层

消息层承载丰富功能。所有功能通过 `messages[]` 和 `reply_to` 扩展。

- 上行消息：agent 在 `/heartbeat` 请求中携带状态和回复。
- 下行消息：coordinator 在 `/heartbeat` 响应中携带指令和配置。
- 状态转发：coordinator 只通过 `/heartbeat_fwd` 转发 `state.*` 消息。

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

由 agent 或 group 调用 coordinator。也由 agent 调用 group。

请求：

```json
{
  "agent_id": "agent-1",
  "epoch": 1,
  "seq": 42,
  "ttl_ms": 15000,
  "messages": [
    {
      "message_id": "msg-agent-1-42-state",
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
      "message_id": "msg-agent-1-41-ack",
      "type": "reply.command_result",
      "version": 1,
      "reply_to": "cmd-coordinator-a-1001",
      "payload": {
        "status": "ok",
        "detail": "config applied"
      }
    }
  ]
}
```

- `messages` 是可扩展消息数组。
- agent 至少发送 `state.heartbeat`。
- 如果 agent 收到过需要确认的下发指令，应在后续心跳中携带 `reply.*`。

响应：

```json
{
  "ok": true,
  "coordinator_id": "coordinator-a",
  "accepted_seq": 42,
  "messages": [
    {
      "message_id": "cmd-coordinator-a-1002",
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

- coordinator 可以返回空 `messages[]`，表示无指令。
- coordinator 通过响应消息下发指令、配置、redirect 或节流策略。
- agent 处理下行消息后，在下一次或更早一次心跳里携带回复。
- 不在 `/heartbeat` 中增加复杂动作参数，动作语义由消息类型定义。

幂等规则：

- 同一个 `agent_id`，`epoch` 更大者优先。
- `epoch` 相同，`seq` 更大者优先。
- `message_id` 全局唯一或在 `agent_id` 内唯一。
- 重复 `message_id` 不重复执行。
- 回复消息通过 `reply_to` 关联下发指令。

### 批量 `POST /heartbeat`

当 group 聚合多个 agent 心跳时，仍使用同一个接口，只是请求结构使用 `agents[]`。

```json
{
  "group_id": "group-a",
  "agents": [
    {
      "agent_id": "agent-1",
      "epoch": 1,
      "seq": 42,
      "ttl_ms": 15000,
      "messages": [
        {
          "message_id": "msg-agent-1-42-state",
          "type": "state.heartbeat",
          "version": 1,
          "payload": {"status": "alive"}
        }
      ]
    }
  ]
}
```

coordinator 响应按 `agent_id` 返回下发消息：

```json
{
  "ok": true,
  "coordinator_id": "coordinator-a",
  "agents": [
    {
      "agent_id": "agent-1",
      "accepted_seq": 42,
      "messages": [
        {
          "message_id": "cmd-coordinator-a-1002",
          "type": "cmd.update_config",
          "version": 1,
          "payload": {"heartbeat_interval_ms": 3000}
        }
      ]
    }
  ]
}
```

group 必须把对应 `agent_id` 的响应消息返回给对应 agent。agent 的回复消息继续经 group 转发给 coordinator。

### `POST /heartbeat_fwd`

由 coordinator 调用 peer coordinator。

请求：

```json
{
  "source_coordinator_id": "coordinator-a",
  "states": [
    {
      "agent_id": "agent-1",
      "epoch": 1,
      "seq": 42,
      "ttl_ms": 15000,
      "observed_at_ms": 1710000000000,
      "messages": [
        {
          "message_id": "msg-agent-1-42-state",
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
  "coordinator_id": "coordinator-b",
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

agent 不监听端口，因此不提供 `/kv`。后续如果需要读取 agent 扩展信息，优先让 agent 通过 `state.*` 消息主动上报，或者让 coordinator 通过 `cmd.sample` 要求 agent 在下一次心跳中返回采样结果。

## 消息类型

消息类型按前缀分层：

| 前缀 | 方向 | 是否可 `/heartbeat_fwd` | 用途 |
| --- | --- | --- | --- |
| `state.*` | agent -> coordinator | 是 | 存活、负载、版本、能力等状态 |
| `cmd.*` | coordinator -> agent | 否 | 配置更新、立即心跳、采样、降级等指令 |
| `reply.*` | agent -> coordinator | 否 | 指令执行结果、错误、进度 |
| `control.*` | coordinator -> agent | 否 | redirect、节流、心跳周期调整 |

推荐从少量消息开始：

| 类型 | 说明 |
| --- | --- |
| `state.heartbeat` | 基础存活状态 |
| `state.metrics` | 简单指标，如 load、memory、qps |
| `state.capability` | agent 支持的消息版本和能力 |
| `cmd.update_config` | 下发配置变更 |
| `cmd.report_now` | 要求 agent 尽快发送一次心跳回复 |
| `cmd.sample` | 要求 agent 采样本地信息 |
| `reply.command_result` | 指令执行结果 |
| `control.redirect` | 引导 agent 使用 group |
| `control.throttle` | 调整心跳间隔或消息上报频率 |

## 指令下发与回复

coordinator 不直接连接 agent，而是在 `/heartbeat` 响应中下发指令。

```text
+----------+                         +----------+
| agent   |                         | coordinator   |
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

- agent 收到需要确认的 `cmd.*` 后，可以立即触发一次额外 `/heartbeat`。
- 如果指令可异步执行，agent 先回复 `accepted`，完成后再回复 `ok` 或 `failed`。
- coordinator 通过 `message_id`、`reply_to` 和超时判断指令是否完成。
- 指令重复下发时，agent 根据 `message_id` 去重。

回复状态建议：

| 状态 | 含义 |
| --- | --- |
| `accepted` | 已收到，准备执行 |
| `running` | 正在执行 |
| `ok` | 执行成功 |
| `failed` | 执行失败 |
| `unsupported` | agent 不支持该消息类型或版本 |

## Group 转发模型

### 普通心跳转发

```text
agent --/heartbeat--> group --/heartbeat(agents[])--> coordinator
```

### 下行消息转发

```text
coordinator --response.agents[].messages[]--> group --response.messages[]--> agent
```

### 回复消息转发

```text
agent --/heartbeat(reply.*)--> group --/heartbeat(agents[].messages[])--> coordinator
```

关键规则：

- group 必须保持请求与响应的 `agent_id` 映射。
- group 不解释业务指令，只按 agent 维度转发。
- group 可以聚合 `state.*` 和 `reply.*` 上行消息。
- group 不调用 `/heartbeat_fwd`。
- group 失败时，agent 可以回退直连 coordinator。

## 状态模型

coordinator 本地维护节点视图和消息账本：

```text
NodeState {
  agent_id: string
  epoch: uint64
  seq: uint64
  ttl_ms: uint64
  observed_at_ms: int64
  expire_at_ms: int64
  source: string          // "direct" | "group" | peer coordinator_id
  state: map<string, any>
}

MessageLedger {
  message_id: string
  agent_id: string
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
| agent   |
+----+-----+
     |
     | /heartbeat(state.*, reply.*)
     v
+----------+
| coordinator-a |
+----+-----+
     |
     | response(cmd.*, control.*)
     v
+----------+
| agent   |
+----------+
```

coordinator 再异步转发状态：

```text
+----------+
| coordinator-a |
+----+-----+
     |
     | /heartbeat_fwd(state.*)
     v
+----------+
| coordinator-b |
+----------+
```

### 聚合路径（redirect）

当 coordinator 判断心跳频率过高时，可以通过 `control.redirect` 引导 agent 使用 group。

```text
+----------+
| agent   |
+----+-----+
     |
     | /heartbeat(state.*, reply.*)
     v
+----------+
| coordinator-a |
+----+-----+
     |
     | response: control.redirect
     | group_addr: group-a:9000
     v

  === agent 下次心跳 ===

+----------+
| agent   |
+----+-----+
     |
     | /heartbeat(state.*, reply.*)
     v
+----------+
| group-a  |
| 聚合窗口  |
+----+-----+
     |
     | /heartbeat(agents[])
     v
+----------+
| coordinator-a |
+----+-----+
     |
     | response agents[].messages[]
     v
+----------+
| group-a  |
+----+-----+
     |
     | response messages[]
     v
+----------+
| agent   |
+----------+
```

coordinator 仍只将状态类消息同步到 peers：

```text
+----------+
| coordinator-a |
+----+-----+
     |
     | /heartbeat_fwd(state.*)
     v
+----------+
| coordinator-b |
+----------+
```

关键点：

- agent 与 group、coordinator 的主路径全程只用 `/heartbeat`。
- agent 发送单 agent 心跳；group 聚合后发送 `agents[]`。
- coordinator 不关心请求来源是 agent 还是 group，统一按消息语义处理。
- group 需要把 coordinator 返回的下行消息转给对应 agent。
- redirect 是优化路径，不是正确性依赖。group 失败时 agent 回退直连。

### Agent 行为状态机

```text
+-------------------+
| Direct Mode       |
| 发送到 coordinator     |
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
| 达到 batch_size            |               | 发送 /heartbeat(agents[]) |
| 或 flush_interval 到期?    |               | 到 coordinator                  |
+----------------------------+               +----------------------------+
             | no
             v
       [ 继续等待 ]
```

group 收到 coordinator 响应后，必须按 `agent_id` 拆分 `agents[].messages[]`，再把对应消息放进原始 agent 的 `/heartbeat` 响应。

## 最终一致性策略

coordinator 之间不选主，不做 quorum 写入。

收敛依赖：

- agent 周期性发送 `/heartbeat`（直连或经 group）。
- group 聚合后仍通过 `/heartbeat` 转发。
- coordinator 对 peers 异步发送 `/heartbeat_fwd`。
- `/heartbeat_fwd` 只传播 `state.*`。
- 所有合并使用 `epoch + seq` 确定性规则。
- 旧状态不会覆盖新状态。

推荐默认参数：

| 参数 | 建议值 | 说明 |
| --- | --- | --- |
| `heartbeat_interval_ms` | `5000` | agent 心跳周期 |
| `urgent_heartbeat_delay_ms` | `100` | 收到需确认指令后触发额外心跳的延迟 |
| `ttl_ms` | `15000` | 允许 3 个周期抖动 |
| `redirect_ttl_ms` | `60000` | agent 使用 group address 的有效期 |
| `group_flush_interval_ms` | `1000` | group 聚合窗口最大等待时间 |
| `group_flush_batch_size` | `1000` | group 单批 agent 数量上限 |
| `command_timeout_ms` | `30000` | coordinator 等待指令回复的默认超时 |
| `forward_retry` | `3` | peer 转发失败重试次数 |
| `peer_timeout_ms` | `1000` | 单次 peer 请求超时 |

## 失败处理

| 场景 | 处理 |
| --- | --- |
| agent -> coordinator 失败 | 重试或切换 coordinator，幂等无副作用 |
| agent -> group 失败 | 回退到直连 coordinator |
| group -> coordinator 失败 | group 重试，依赖幂等合并 |
| group 返回失败 | agent 下次心跳可重发未确认 `reply.*` |
| coordinator -> peer 失败 | 后台重试，不影响 agent 返回 |
| 指令回复超时 | coordinator 标记 `expired`，可重发或人工处理 |
| agent 重启 | 递增 `epoch`，覆盖旧进程残留 |
| 消息版本不支持 | agent 回复 `unsupported` |
| redirect ttl 到期 | agent 回到 Direct Mode |

## 非目标

- 不实现强一致成员管理。
- 不实现 leader election。
- 不保证心跳写入后所有 coordinator 立即可见。
- 不要求所有 agent 必须经过 group。
- 不把 group 设计成状态权威。
- 不要求 agent 监听端口。
- 暂时不启用 `/kv`。
- 不通过 `/heartbeat_fwd` 转发指令类消息。

## 小结

这个设计把系统复杂度放在消息层，而不是接口层：

- `/heartbeat` 是主接口，承载 agent 上行状态、上行回复和 coordinator 下行指令。
- `/heartbeat_fwd` 只做状态类消息的 peer 间传播。
- agent 不监听端口，部署和安全模型更简单。
- group 负责聚合与转发，包括转发心跳回复。
- 后续能力通过 `state.*`、`cmd.*`、`reply.*`、`control.*` 扩展。
