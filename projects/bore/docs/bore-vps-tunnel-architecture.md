# bore：VPS TCP 中转与域名入口架构分析

## 结论

[`ekzhang/bore`](https://github.com/ekzhang/bore) 是一个小而完整的 TCP 反向隧道，不是 HTTP 隧道平台，也不是 Cloudflare Tunnel 的替代品。

- 可以：内网机器主动连 VPS，把本地任意 TCP 服务暴露为 `VPS_IP:端口`。
- 可以：在 VPS 前面再放 Caddy、Nginx 或 HAProxy，实现 `app.example.com` 的 HTTP/HTTPS 域名入口。
- 不可以：仅靠 bore 将多个域名自动绑定到不同隧道；它不解析 HTTP `Host`、TLS SNI 或 DNS。
- 不可以：得到 Cloudflare Tunnel 原生具备的边缘网络、自动 hostname 路由、访问策略、身份提供商集成、DDoS/WAF、端到端加密或高可用控制平面。

建议把 bore 用于可信环境中简单、低运维的 TCP 中转。生产 Web 暴露应至少叠加 TLS 终止、每客户端独立凭据、端口与防火墙白名单，以及进程托管与监控；多租户或公网生产入口优先使用 Cloudflare Tunnel、frp、rathole 或自建具备 TLS/mTLS 和服务发现的方案。

## 调研基线与代码规模

分析对象是上游仓库在 2026-08-28 拉取的提交：

| 项目 | 值 |
| --- | --- |
| 上游 | `https://github.com/ekzhang/bore` |
| 提交 | [`00a735a`](https://github.com/ekzhang/bore/tree/00a735a89917642df62d84336a90d9476fa175b5) |
| 提交日期 | 2026-02-04 |
| crate 版本 | `bore-cli 0.6.0` |
| `src/*.rs` | 615 行 |
| `tests/*.rs` | 199 行 |
| Rust 合计 | 814 行 |
| 验证 | `cargo test --all`：11 tests + 1 doctest 均通过 |

README 中“about 400 lines of safe, async Rust”是早期宣传用的约数，已不是当前代码的准确行数。若只粗看客户端与服务端主体，`client.rs` 127 行加 `server.rs` 187 行约 314 行；但协议、认证、CLI 和库入口也是实现的一部分。

## 项目架构详情

### 模块职责

| 模块 | 行数 | 职责 |
| --- | ---: | --- |
| [`main.rs`](https://github.com/ekzhang/bore/blob/00a735a89917642df62d84336a90d9476fa175b5/src/main.rs) | 102 | Clap CLI；构造 `Client` 或 `Server`；读取端口、监听地址和 `--secret`。 |
| [`client.rs`](https://github.com/ekzhang/bore/blob/00a735a89917642df62d84336a90d9476fa175b5/src/client.rs) | 127 | 保持控制连接；收到新公网连接后，另开一条连接回 VPS，再连本地服务，双向复制字节流。 |
| [`server.rs`](https://github.com/ekzhang/bore/blob/00a735a89917642df62d84336a90d9476fa175b5/src/server.rs) | 187 | 监听控制端口；为隧道绑定一个 TCP 端口；暂存外部连接；按 UUID 与客户端数据通道配对。 |
| [`shared.rs`](https://github.com/ekzhang/bore/blob/00a735a89917642df62d84336a90d9476fa175b5/src/shared.rs) | 99 | 控制协议、JSON/NUL 帧、3 秒握手超时、7835 控制端口。 |
| [`auth.rs`](https://github.com/ekzhang/bore/blob/00a735a89917642df62d84336a90d9476fa175b5/src/auth.rs) | 79 | 基于 HMAC-SHA256 的随机挑战应答。 |
| [`lib.rs`](https://github.com/ekzhang/bore/blob/00a735a89917642df62d84336a90d9476fa175b5/src/lib.rs) | 21 | 暴露模块，且 `#![forbid(unsafe_code)]`。 |

它只有一个二进制，按 CLI 角色运行：

```text
+--------------------------+       TCP 7835        +--------------------------+
| bore local               |---------------------->| bore server (VPS)        |
| local_port -> service    |                       | remote_port listener     |
+--------------------------+                       +--------------------------+
```

依赖也与这一边界一致：Tokio 负责异步网络和 `copy_bidirectional`，DashMap 保存待匹配连接，Serde JSON 编码控制帧，UUID 做一次性连接关联，HMAC/SHA-256 做可选认证。

### 控制面与数据面

控制协议是最大 256 bytes 的 NUL 分隔 JSON，不是 HTTP，不使用 QUIC 或 WebSocket：

```text
+---------------------------+                     +---------------------------+
| bore local client         |                     | bore server (VPS)         |
+---------------------------+                     +---------------------------+
              |                                                   |
              |--- TCP 7835 ------------------------------------>|
              |<-- Challenge(UUID), if secret -------------------|
              |--- Authenticate(HMAC), if secret --------------->|
              |--- Hello(requested_port) ----------------------->|
              |<-- Hello(assigned_port) -------------------------|
              |<-- Heartbeat, every ~500 ms ---------------------|
```

外部访问到达公开端口时，服务端不把该连接直接复用在控制流上，而以 UUID 建立单独数据流：

```text
[Internet user] --> [VPS remote_port]
                         |
                         +-- Connection(UUID) --> [bore local client]
                         |                               |
                         |                               +-- TCP --> [local service]
                         |
                         +<-- TCP 7835 + Accept ----------+

[Internet user] <==== TCP bytes ====> [VPS remote_port]
                                      <==== TCP bytes ====> [bore local client]
                                                              <==== TCP bytes ====> [local service]
```

具体行为：

1. 客户端初始 `Hello(0)` 请求随机端口，或 `Hello(port)` 请求固定端口。
2. 服务端仅在 `--min-port..--max-port` 中绑定端口；随机模式至多尝试 150 次。端口被占用或越界会返回 `Error`。
3. 原控制连接存活期间，服务端每约 500 ms 发送 `Heartbeat`，同时轮询该隧道端口。
4. 外部连接进入后，服务端生成 v4 UUID，把 `TcpStream` 放入全局 `DashMap`，并发出 `Connection(uuid)`。
5. 客户端为每个 UUID 新建至 `VPS:7835` 的 TCP 连接，认证后发送 `Accept(uuid)`，再连接 `local_host:local_port`。
6. 服务端从 `DashMap` 取出 UUID 对应的外部连接，二者用 `tokio::io::copy_bidirectional` 原样转发。
7. 10 秒内没有 `Accept` 的外部连接会从 `DashMap` 移除，避免无限积压。

因此 bore 的转发粒度是“一个公网 TCP 端口对应一个本地目标”，不是“一个 hostname 对应一个目标”。

## 通过 VPS 中转

### 最小可用部署

假定 VPS 公网地址为 `vps.example.net`，内网机器的 Web 服务监听 `127.0.0.1:3000`，希望暴露 TCP 端口 3000：

```bash
# VPS：控制平面监听所有网卡；隧道端口仅暴露给本机反向代理。
bore server \
  --bind-addr 0.0.0.0 \
  --bind-tunnels 127.0.0.1 \
  --min-port 20000 \
  --max-port 20100 \
  --secret "$BORE_SECRET"

# 内网机器：固定一个端口，以便代理配置稳定。
bore local 3000 \
  --to vps.example.net \
  --port 20001 \
  --secret "$BORE_SECRET"
```

关键点：

- `--bind-addr` 控制 7835 的地址；`--bind-tunnels` 决定公开服务端口绑定在哪个地址。
- 上例将隧道端口限制在 VPS loopback，因此互联网无法直接访问 `:20001`；只有 VPS 上的反向代理能访问它。
- VPS 防火墙仅应允许所需来源访问 TCP 7835。若内网机器出口 IP 固定，必须只允许该 IP；80/443 面向终端用户开放。
- 若直接暴露原始 TCP 服务，把 `--bind-tunnels` 改为 `0.0.0.0`，并仅在安全组中打开允许的隧道端口。这时用户以 `vps.example.net:20001` 访问，没有 hostname 路由。
- 服务端控制连接断开时，对应 `TcpListener` 随连接处理函数退出而被释放；客户端当前实现不自动重连，应由 systemd、supervisord 或容器编排重启，并监控 TCP 7835 连通性与进程存活。

### HTTP/HTTPS 域名入口

DNS 将 `app.example.com` 的 A/AAAA 记录指向 VPS。然后让 Caddy/Nginx 在 VPS 终止 TLS，并将 HTTP 请求代理到 bore 的 loopback 端口。

```text
[Browser]
    |
    +-- HTTPS Host: app.example.com --> [VPS proxy :443]
                                             |
                                             +-- HTTP --> [bore :20001]
                                                               |
                                                               +-- TCP tunnel --> [local :3000]
```

Caddy 示例：

```caddyfile
app.example.com {
    reverse_proxy 127.0.0.1:20001
}
```

Nginx 等价核心配置：

```nginx
server {
    listen 443 ssl http2;
    server_name app.example.com;

    ssl_certificate     /etc/letsencrypt/live/app.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/app.example.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:20001;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

这种组合可以实现“像 Cloudflare Tunnel 一样通过域名访问内网 HTTP 服务”的用户体验，但职责在 Caddy/Nginx 和 DNS，不在 bore。每个域名需要一个稳定的 bore 端口及一条反向代理路由；自动发现、自动配置和证书策略要另行实现。

对于 HTTPS 透传而不是在 VPS 解密，必须使用支持 TLS SNI 分流的四层代理（例如 HAProxy 或 Nginx stream）将不同 hostname 转发至不同 loopback 端口；证书仍由内网服务维护。bore 本身看不见 SNI，也不能按域名做决策。

## 认证与安全边界

### 已保证的内容

启用 `--secret` 后，每一条到 `:7835` 的控制或数据连接都要完成挑战应答：

```text
server: Challenge(random UUID)
client: Authenticate(hex(HMAC-SHA256(SHA256(secret), UUID)))
```

- UUID 是新生成的随机 challenge；旧响应不能直接重放到新 challenge。
- 服务端使用 `verify_slice` 比较 MAC，验证客户端持有同一 secret。
- `Hello`、`Accept` 都发生在认证之后，因此不知道 secret 的人不能创建隧道，也不能把自己接入已缓存的 UUID。
- 测试覆盖了同 secret 成功和 client/server secret 不匹配失败。

### 未保证的内容

| 风险/能力 | bore 现状 | 要求的补充措施 |
| --- | --- | --- |
| 传输加密 | 没有 TLS；README 也明确后续流量默认不加密。 | 外层 VPN、WireGuard、stunnel/TLS 包装，或选择原生 TLS/mTLS 隧道。HTTP 场景至少在 VPS 对用户侧启用 HTTPS。 |
| 服务端身份认证 | 客户端只证明自己知道 secret，不验证 VPS 身份。 | TLS/mTLS 或 WireGuard，避免 DNS/网络路径劫持和活动中间人。 |
| 多租户隔离 | 一个 server 只有一个全局 secret；没有用户、租户、ACL、端口所有权模型。 | 每个信任域独立 server/实例和 secret；需要用户级策略则改造协议或换工具。 |
| 最小权限 | 持有 secret 的任意客户端可申请允许范围中的任一端口。 | 缩小端口范围；安全组限制 7835 来源；每应用独立实例。 |
| 公网端口保护 | 访问隧道端口的用户无需 bore 认证，这是公开服务的设计。 | 在应用层加身份认证，或只绑定 `127.0.0.1` 并交给反向代理实施访问控制。 |
| 可用性 | 无限重连、限速、配额、审计、HA、DDoS/WAF 均非内置能力。 | systemd 重启、健康检查、日志/指标、反向代理限流，必要时用云边缘产品。 |

特别注意：`--secret` 不能替代 TLS。它防止未授权者使用 VPS 控制面，却不加密业务字节，也不提供客户端对服务端的密码学身份验证。secret 应通过 systemd `EnvironmentFile`、密钥管理系统或受限权限文件注入，不能出现在 shell history、命令行、镜像层或仓库中。

## 与 Cloudflare Tunnel 的对比

两者都通过内网侧主动建立出站连接来规避 NAT 和入站防火墙，但产品边界不同。bore 提供的是一个可自托管的 TCP 连接配对器；Cloudflare Tunnel 是连接器、Cloudflare 全局边缘、DNS/TLS、控制平面和 Zero Trust 产品的组合。

```text
bore

[local service] <-- TCP --> [bore local] <-- TCP --> [single VPS] <-- TCP --> [user]

Cloudflare Tunnel

[local service] <---> [cloudflared] == encrypted outbound links ==> [Cloudflare edge] <-- HTTPS --> [user]
                                      == configuration and identity ==> [Cloudflare control plane]
```

截至 2026-08-28，Cloudflare 的官方配置文档说明：一个 `cloudflared` 实例会建立四条仅出站连接，覆盖至少两个 Cloudflare 数据中心；连接器副本可为同一 tunnel 增加入口。传输默认优先 QUIC，UDP 不可用时可回退 HTTP/2；配置可以由 Dashboard、API 或 Terraform 远程管理。[官方配置文档](https://developers.cloudflare.com/tunnel/configuration/)与[运行参数文档](https://developers.cloudflare.com/tunnel/advanced/run-parameters/)是该结论的可复核来源。

| Dimension | bore | bore + VPS proxy | Cloudflare Tunnel |
| --- | --- | --- | --- |
| Core abstraction | A TCP port paired with a local target. | bore plus a manually operated HTTP/TLS proxy. | A managed tunnel and edge routing service. |
| Public hostname routing | No `Host`, SNI, or DNS awareness. | Static Caddy/Nginx/HAProxy routes map hostname to a fixed bore port. | Public hostname and service routes are first-class configuration. |
| Edge and availability | One VPS is the data-plane and a single failure domain. | Same VPS failure domain unless the operator builds HA. | Multiple outbound links, edge PoPs, and connector replicas are supported. |
| Encryption and identity | HMAC proves a shared client secret; payload is plaintext. | TLS can terminate at the proxy; tunnel still needs a separate protected transport. | Connector-to-edge encrypted transport and platform identity/control-plane integration. |
| Access policy | None beyond the shared secret for control connections. | Proxy/application policy must be built and operated by the user. | Can integrate with Cloudflare Access and other Zero Trust controls. |
| Operations | One binary, one port range, local logs. | DNS, certificate renewal, proxy reload, monitoring, and HA are operator work. | Managed configuration, observability, DNS/TLS, and edge operations are product capabilities. |
| Protocol scope | Generic raw TCP. | Raw TCP plus whatever the VPS proxy understands. | Primarily application and private-network routing; public raw TCP exposure follows Cloudflare product constraints. |

### What bore contributes

bore 的价值不在“功能比 SaaS 隧道更多”，而在将反向 TCP 隧道缩减到一个可审计的最小正确实现：

1. **NAT inversion with no inbound local listener.** 本地机器只向 VPS 发起出站 TCP；公网新连接由 VPS 用 UUID 通知客户端，客户端再主动建立数据连接。这是穿透 NAT 的核心机制，没有引入 HTTP、DNS 或账户系统。
2. **Protocol neutrality.** 数据面只复制字节流，因此 HTTP、SSH、数据库协议和自定义 TCP 协议不需要适配层。是否应当暴露这些服务仍由网络与应用安全策略决定。
3. **Small, inspectable state machine.** 核心状态只有控制连接、端口监听器和 `UUID -> pending TcpStream` 映射；JSON 控制帧限制为 256 bytes，未匹配连接 10 秒回收。完整实现可以逐函数审计，不依赖远端账户或复杂控制平面。
4. **Correct streaming behavior.** `copy_bidirectional` 让两端以 TCP 背压流动，避免把请求完整缓存在内存中；测试覆盖半关闭连接，证明一端发送 FIN 后反方向仍可完成响应。
5. **A deliberate security boundary.** HMAC challenge-response 只保护谁可以请求/接受隧道，不伪装成 TLS。这个边界很窄，但可见且容易在外层用 WireGuard、mTLS 或反向代理补齐。

这些特性使 bore 特别适合临时调试、个人服务、受信任内网或需要理解每一跳的工程场景。它不适合直接承担多租户公网入口的身份、策略和边缘可用性责任。

### Adoption decision

- 只有一两个自有服务，接受单 VPS，并能维护 TLS/防火墙：`bore + Caddy/Nginx` 足够简洁。
- 需要按域名动态注册服务：bore 需要额外控制器，负责分配端口、更新代理配置并安全 reload；此时直接选带 hostname/control-plane 的隧道产品通常更省成本。
- 需要多用户、强认证、审计和公网抗攻击：不要将 bore 的共享 secret 当作平台级安全模型。

## 可复核命令

```bash
git clone https://github.com/ekzhang/bore.git
cd bore
git checkout 00a735a89917642df62d84336a90d9476fa175b5
find src tests -name '*.rs' -print0 | xargs -0 wc -l
cargo test --all
```
