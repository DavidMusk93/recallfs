# Kafka Protocol Address Config

本文说明 `connector.type = kafka_protocol` 作为 Kafka server 端时，Kafka client 应该连接哪个地址，以及相关地址配置的格式和代码解析逻辑。

## 配置总览

| 配置项 | 作用 | 格式 | 默认值 |
|---|---|---|---|
| `connector.port` | 本进程 Kafka server bind/listen 的端口 | 单个端口号，如 `9951` | `9999` |
| `port_range` | 调度/资源层可用端口范围，当前 Kafka server 解析逻辑不直接读取 | 端口范围，如 `9951-9954` | 无 |
| `kafka.metadata.enable` | 没有 `kafka.schedule.host` 时，是否由本 server 直接返回 Kafka metadata | `true` / `false` | `true` |
| `kafka.ttgw.hostport` | 直接返回 metadata 时，返回给 Kafka client 的 broker 地址 | `host:port`，多个用 `;` 分隔 | 空 |
| `kafka.schedule.host` | metadata 请求转发目标 host，通常是调度服务地址 | 单个 host/IP | 空 |
| `kafka.schedule.port` | metadata 请求转发目标 port | 单个端口号 | `0` |
| `kafka.schedule.node0.hostport` | metadata 转发失败兜底 response 中的 scheduler broker 地址 | `host:port`，多个用 `;` 分隔 | 空 |
| `kafka.schedule.node1.hostport` | metadata 转发失败兜底 response 中的 tide broker 地址 | `host:port`，多个用 `;` 分隔 | 空 |

## 地址格式

`kafka.ttgw.hostport`、`kafka.schedule.node0.hostport`、`kafka.schedule.node1.hostport` 使用同一套解析函数：

```text
host:port
host1:port1;host2:port2
```

代码解析规则：

```cpp
split(ipport.data(), ipport.size(), ';', out);
for (const auto& d : out) {
    size_t pos = d.find(':');
    if (pos != std::string::npos) {
        res.emplace_back(d.substr(0, pos), d.substr(pos + 1));
    }
}
```

注意：

- 多个地址用英文分号 `;` 分隔。
- 每个地址按第一个冒号 `:` 拆成 host 和 port。
- 代码没有专门处理 IPv6 的方括号格式，`fdbd:dc02:24:106::27%eth0:9989` 这类 IPv6 地址会被第一个冒号截断。
- 如果要配置 IPv6，建议优先使用 `kafka.schedule.host` + `kafka.schedule.port` 这类 host/port 分离的配置，或者先确认现有解析函数是否满足实际格式。

## Metadata 返回优先级

Kafka client produce 前通常会先发 `MetadataRequest`。当前 server 处理顺序如下：

```text
MetadataRequest
|
+-- 如果 kafka.schedule.host 非空
|   |
|   +-- 将 metadata request 转发到 kafka.schedule.host:kafka.schedule.port
|
+-- 否则由本 server 处理
    |
    +-- 如果 kafka.metadata.enable=false
    |   |
    |   +-- 返回 unknown_server_error
    |
    +-- 如果 kafka.ttgw.hostport 非空且 client 不是 IPv6
    |   |
    |   +-- metadata broker = kafka.ttgw.hostport 中随机一个 host:port
    |
    +-- 否则
        |
        +-- metadata broker = MY_HOST_IPV6 或 MY_HOST_IP + 端口
```

对应代码入口：

- 配置读取：`src/source/mq/kafka_protocol_source.cpp`
- 地址解析：`src/source/mq/utils/config.h`
- metadata 返回：`src/source/mq/kafka/server/handlers/metadata.cpp`
- 本机地址：`src/util/base/env.h`

## 常见配置方式

### 1. Client 直连本机或 pod 地址

适用于 Kafka client 能直接访问 worker 的 `MY_HOST_IP:connector.port`。

```sql
'connector.type' = 'kafka_protocol',
'connector.mode' = 'source',
'connector.port' = '9951',
'connector.topic' = 'dwd_frontier_flow_log_access_log_hi',
'kafka.metadata.enable' = 'true'
```

metadata 返回地址：

```text
MY_HOST_IP:9951
```

如果环境变量 `TIDE_USE_TCE_SOURCE_PORT` 非 `0` 且设置了 `PORT2`，metadata 返回端口会优先使用 `PORT2`。

### 2. Client 通过 VIP / 四层转发访问

适用于 Kafka client 只能访问对外 VIP、L4 或网关地址。

```sql
'connector.type' = 'kafka_protocol',
'connector.mode' = 'source',
'connector.port' = '9951',
'connector.topic' = 'dwd_frontier_flow_log_access_log_hi',
'kafka.metadata.enable' = 'true',
'kafka.ttgw.hostport' = 'frontier-vip.example.com:9951'
```

metadata 返回地址：

```text
frontier-vip.example.com:9951
```

多个入口可以这样写：

```sql
'kafka.ttgw.hostport' = 'vip1.example.com:9951;vip2.example.com:9951'
```

代码会随机选择其中一个返回。

### 3. Metadata 由调度服务返回

适用于 metadata broker 地址由外部调度服务决定，当前 server 只负责转发 metadata request。

```sql
'connector.type' = 'kafka_protocol',
'connector.mode' = 'source',
'connector.port' = '9951',
'connector.topic' = 'dwd_frontier_flow_log_access_log_hi',
'kafka.schedule.host' = 'scheduler.example.com',
'kafka.schedule.port' = '9989'
```

此时 `kafka.metadata.enable` 不参与本地 metadata 返回，因为请求已经被转发。

## 关于 Frontier VIP / TTGW

代码里没有名为 `frontier.vip` 的配置，也没有自动通过四层转发反查真实调度地址的逻辑。

这里的 `kafka.ttgw.hostport` 可以理解为 Kafka 的 advertised broker address：

```text
Kafka client 可访问的对外 broker 地址
```

这个地址可以是：

- TTGW 地址
- Frontier VIP 地址
- 四层转发地址
- 其他 Kafka client 能连通的网关地址

只要 Kafka client 能访问它，就可以放到 `kafka.ttgw.hostport`。

## Produce 前置条件

即使 metadata 返回正常，produce 还要求 topic 已进入 `RUNNING`：

```text
source Init 注册 topic/partition
source Run 标记 partition ready
所有 partition ready 后 TopicInfo 状态变为 RUNNING
produce 才会被接受
```

如果 produce 太早，可能返回 `not_leader_for_partition` 或表现为 client 端 topic 不可用。

可以在日志中确认：

```text
topic=<topic> partition=<n> ready
topic=<topic> partition=<n> mark status running
```

## 排查建议

1. 如果 client 报找不到 topic，先确认 `kafka.metadata.enable=true` 或配置了 `kafka.schedule.host/port`。
2. 如果 client metadata 中返回了内网 IP，说明没有配置 `kafka.ttgw.hostport`，或者 client 被识别为 IPv6。
3. 如果 client 通过 VIP 访问，建议显式配置 `kafka.ttgw.hostport=<vip>:<port>`。
4. 如果使用 IPv6 地址，避免直接放进 `kafka.ttgw.hostport` 的多地址解析格式，优先使用 host/port 分离的 `kafka.schedule.host` 和 `kafka.schedule.port`。
5. 如果 metadata 正常但 produce 失败，检查 topic 是否已经所有 source partition ready。
