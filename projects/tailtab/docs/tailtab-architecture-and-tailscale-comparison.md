# Tailtab 架构分析与 Tailscale 对比

## 结论

[`Stocist/Tailtab`](https://github.com/Stocist/Tailtab) 不是 Tailscale 的替代实现，也不自建 VPN 控制面。它是一个浏览器作用域的 Tailscale 适配层：

- 浏览器扩展决定哪些请求进入 Tailtab。
- Native Messaging 宿主为当前浏览器 profile 启动一个嵌入式 `tsnet.Server`。
- 宿主在 `127.0.0.1` 上提供带临时凭据的 HTTP/SOCKS5 代理。
- `tsnet` 继续使用 Tailscale 或 Headscale 控制面，并复用 Tailscale 的 WireGuard、NAT 穿透、DERP/Peer Relay、MagicDNS、子网路由和 exit node 数据面。

因此，两者不是同层竞品：

| 选择 | 最适合的目标 |
| --- | --- |
| Tailtab | 只让一个浏览器 profile 进入 tailnet；同机多个 profile 分属不同 tailnet；不希望安装系统级 VPN 或取得管理员权限。 |
| 原生 Tailscale 客户端 | 让整台设备上的任意应用获得 L3 网络能力；需要 UDP、ICMP、SSH、Serve/Funnel、入站监听、系统 DNS、设备策略和完整运维诊断。 |
| 两者并用 | 系统 Tailscale 承担日常设备网络；Tailtab 只给隔离的浏览器 profile 提供另一套身份或另一条 tailnet 路径。 |

推荐把 Tailtab 当作“浏览器专用 `tsnet` 节点”，而不是“轻量版 Tailscale”。它当前仍标记为 experimental，Chromium 扩展需要开发者模式加载，宿主二进制尚未签名或 notarize。个人实验、开发环境和多账户隔离场景可用；受管企业终端不应在缺少软件分发、签名、版本锁定、监控和事件响应方案时直接推广。

## 调研基线

分析与验证时间为 2026-09-09。

| 对象 | 基线 |
| --- | --- |
| Tailtab 上游 | [`Stocist/Tailtab`](https://github.com/Stocist/Tailtab) |
| Tailtab 提交 | [`58dd707`](https://github.com/Stocist/Tailtab/tree/58dd707b4473f12b4b9bb8b2a5275b684241a7d6)，提交时间 2026-09-09 |
| 最新 release | [`v0.2.2`](https://github.com/Stocist/Tailtab/releases/tag/v0.2.2)，发布于 2026-09-08 |
| 核心依赖 | Go 1.27；`tailscale.com v1.102.3` |
| 实现规模 | 非测试 Go 2,710 行；运行时扩展 JavaScript 1,517 行 |
| 测试规模 | Go 测试 2,896 行、90 个 `Test*`；扩展测试 1,751 行、81 个场景 |
| 本地验证 | `./scripts/test.sh` 全部通过；构建后运行 live smoke，确认 loopback proxy 启动且 Tailscale control plane 返回登录 URL |
| Tailscale 上游 | [`tailscale/tailscale`](https://github.com/tailscale/tailscale) |
| Tailscale 提交 | [`3945b82`](https://github.com/tailscale/tailscale/tree/3945b82f8a9550b54c33e61d4ed2227862d53e8a)，提交时间 2026-09-08 |

上游 README 的平台说明需要按成熟度理解：

- 作者日常使用的是 macOS 上的 Edge 和 Zen。
- Zen/Firefox 的 `v0.2.2` XPI 由 Mozilla 签名，可永久安装，但该包没有 `update_url`，release 也没有 `updates.json`，因此不会自更新。
- Edge/Chrome 包仍以 unpacked extension 方式安装。
- Linux 和 Windows 通过了同一套 CI smoke test，但实际使用更少。
- Chrome 和 Firefox 被标为“应当可用，但尚未充分测试”。

## 系统定位

### Tailtab 增加了什么

Tailtab 没有重新实现 Tailscale 网络协议。它围绕 `tsnet` 增加四项浏览器集成能力：

1. 每个浏览器 profile 生成独立 UUID，并映射到独立 `tsnet` 状态目录。
2. 通过 Native Messaging 在扩展和 Go 宿主之间传递命令与状态。
3. 把浏览器 HTTP/HTTPS 请求转换成 `tsnet` 的用户态 TCP dial。
4. 在浏览器规则与宿主代理两侧执行目标过滤；宿主运行时会双检目标，但宿主故障后的 fail-closed 范围取决于浏览器和目标类型。

### 组件职责

| 组件 | 关键实现 | 职责 |
| --- | --- | --- |
| Chromium/Firefox 扩展 | [`background.js`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/extension/background.js)、[`rules.js`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/extension/rules.js) | 生成 profile UUID；连接 native host；安装 PAC 或 Firefox per-request proxy；处理代理认证；展示状态；故障重连。 |
| Native Messaging 协议 | [`internal/nm/nm.go`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/internal/nm/nm.go) | 4-byte little-endian 长度前缀 + JSON；限制单帧 1 MiB；校验 profile UUID 与 control URL。 |
| 宿主编排 | [`cmd/tailtab/host.go`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/cmd/tailtab/host.go) | 一个进程只服务一个 profile；启动 node 和代理；桥接 `init/up/down/logout/switch/addaccount/exitnode`；推送状态。 |
| 嵌入式节点 | [`internal/node/node.go`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/internal/node/node.go) | 配置并启动 `tsnet.Server`；监听 IPN bus；读取 peers、accounts、routes、exit nodes；持久化 profile 状态。 |
| Loopback 代理 | [`internal/proxy/proxy.go`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/internal/proxy/proxy.go) | 在同一随机端口复用 HTTP 与 SOCKS5；校验临时 token；执行目标 guard；调用 `UserDial`。 |
| 安装器 | [`internal/install/install.go`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/internal/install/install.go) | 在用户目录安装 Native Messaging manifest；通过扩展 ID allowlist 限定可启动宿主的扩展。 |
| Tailscale `tsnet` | [`tsnet`](https://github.com/tailscale/tailscale/tree/3945b82f8a9550b54c33e61d4ed2227862d53e8a/tsnet) | 在进程内运行完整 Tailscale 节点、gVisor 用户态网络栈、LocalBackend、WireGuard 和连接协商。 |

## 控制面与数据面

```text
Control plane

Browser popup
    |
    | native messages
    v
Tailtab host ---- LocalAPI / IPN bus ---- tsnet node
                                           |
                                           | login, keys, netmap, DNS, policy
                                           v
                              Tailscale coordination service
                                      or Headscale

Data plane

Browser request
    |
    | PAC or per-request proxy decision
    v
Loopback HTTP/SOCKS5 proxy
    |
    | token check + destination guard
    v
tsnet UserDial -> gVisor netstack -> WireGuard transport
                                      |
                         +------------+------------+
                         |                         |
                         v                         v
                  direct UDP path          Peer Relay or DERP
                         |                         |
                         +------------+------------+
                                      |
                                      v
                              tailnet destination
```

Tailscale 官方将 tailnet 分成协调服务控制面和设备本地数据面。控制面负责身份、设备注册、公钥和 netmap 分发、DNS、访问策略、NAT 端点协调与 DERP 选择；数据面在设备之间用 WireGuard 搬运加密流量。业务数据不经过协调服务。

Tailtab 完整保留了这个边界。它增加的 native message、loopback proxy 和 PAC 都在本机；到远端 peer 的连接仍由 `tsnet` 数据面建立。正常情况下先通过 DERP 建立可用路径，再尝试升级到 direct UDP；若失败，可使用 Peer Relay 或继续走 DERP。三种路径都保持 WireGuard 端到端加密，差异主要是延迟和吞吐。

## 启动与状态生命周期

### 安装阶段

安装器把宿主二进制放在用户目录，并写入 `com.stocist.tailtab.json`：

- macOS：`~/Library/Application Support/<browser>/NativeMessagingHosts/`
- Linux：`~/.config/<browser>/NativeMessagingHosts/` 与 `~/.mozilla/native-messaging-hosts/`
- Windows：manifest 位于 `%LOCALAPPDATA%\tailtab\`，HKCU registry 指向它

manifest 同时记录宿主绝对路径和允许启动它的 Chromium extension ID 或 Gecko add-on ID。安装不需要 root，但“无 root”不等于“无本机代码”：扩展仍能启动一个当前用户权限的原生进程。

### Profile 初始化

```text
Extension             Native host              tsnet                 Control
    |                      |                      |                      |
    |-- connectNative ---->|                      |                      |
    |-- init ------------->|                      |                      |
    |   profile UUID       |-- create state dir ->|                      |
    |   browser name       |-- Start ------------>|-- register/map ----->|
    |   control URL        |                      |<-- netmap/policy ----|
    |                      |-- bind 127.0.0.1:0   |                      |
    |<-- status -----------|                      |                      |
    |   port + token       |                      |                      |
    |-- install routing -->|                      |                      |
```

关键约束：

1. 扩展首次启动时在 `storage.local` 生成 UUID。不同浏览器 profile 的 extension storage 天然隔离，因此得到不同节点。
2. 宿主拒绝第二次 `init`。一个宿主进程只能绑定一个 profile，避免两个节点共享 `tsnet` 状态目录。
3. 状态目录是 `<user-config>/tailtab/<profile-uuid>/`。其中 `tailscaled.state` 保存节点和账户状态；`control-url` 固定协调服务器；`exit-node.<account-id>` 保存每账户 exit node。
4. `tsnet.Server` 使用非 ephemeral 节点。浏览器或宿主重启后复用原节点密钥，不会每次制造新机器。
5. 宿主通过 LocalAPI/IPN bus 接收 state、health、prefs、peers、accounts、routes 和登录 URL，再转换成扩展状态。
6. 代理在随机 loopback 端口监听；32-byte 随机 token 每个宿主进程重新生成，只保存在宿主与扩展内存中。

### 请求路径

Chromium 与 Firefox 的前半段不同，后半段相同：

| 浏览器家族 | 浏览器侧路由 | 代理协议 | 凭据注入 |
| --- | --- | --- | --- |
| Chromium/Edge | 扩展安装 mandatory PAC | HTTP proxy；HTTPS 使用 CONNECT | `webRequest.onAuthRequired` 响应 407 |
| Firefox/Zen | `proxy.onRequest` 对每个 URL 决策 | SOCKS5，`proxyDNS: true` | RFC 1929 username/password |

代理收到连接后的顺序是：

1. 先验证 `tailtab:<process-token>`，HTTP 采用 constant-time 比较。
2. 再执行 destination guard，拒绝不属于当前模式的目标。
3. 最后调用 `tsnet` 内部 dialer 的 `UserDial`，让 MagicDNS、路由、WireGuard 和 Tailscale policy 生效。
4. HTTP 转发会移除 `Proxy-Authorization` 与 hop-by-hop headers，不向远端暴露本地凭据或 `X-Forwarded-For`。

浏览器路由不是安全边界的终点。即使 PAC 或扩展错误地把目标送进代理，宿主 guard 仍会在实际网络 dial 前复核。这是 Tailtab 防止 loopback listener 退化成开放代理的核心设计。

## 路由语义

### Split mode

默认只代理以下目标：

- 单标签 MagicDNS 名称，例如 `wiki`
- `*.ts.net`
- 当前 control plane 下发的 MagicDNS suffix
- `100.64.0.0/10`
- `fd7a:115c:a1e0::/48`
- peer 下发且通过本地限制的 approved primary subnet routes

其他公网目标保持 `DIRECT`，普通浏览不依赖 Tailtab。子网路由不能宽于 IPv4 `/8` 或 IPv6 `/16`，且不得覆盖 loopback、link-local、multicast、unspecified 等保留范围。

### Exit mode

选中且在线的 exit node 后：

- 公网 hostname 和公网 IP 进入代理，再由 `tsnet` 通过 exit node 发出。
- Tailscale 地址与 approved subnet routes 继续进入 tailnet。
- loopback、link-local 和 private literal IP 保持本机直连或被宿主拒绝。
- exit node 已选但离线且宿主仍运行时，public traffic 继续被送向不可用代理，不会静默降级为本地公网出口。

这里与原生 Tailscale 有一个容易忽略的差异：

| 行为 | Tailtab | 原生 Tailscale exit node |
| --- | --- | --- |
| 公网流量 | 当前浏览器 profile 经 exit node | 整台设备默认路由经 exit node |
| 本地 LAN | private literal IP 默认直连，没有 UI 开关 | 默认不允许 LAN；显式开启 `Allow LAN access` 才直连 |
| 其他应用 | 不受影响 | 受系统路由影响 |
| 故障 kill switch | exit node 离线且宿主仍运行时阻断代理流量；宿主崩溃时仅 Chromium 保留有限的 parked-PAC 保护，Firefox/Zen 回退 `DIRECT` | 覆盖系统级 exit route |

所以 Tailtab 的 exit mode 是“浏览器公网代理 + LAN 旁路”，不是系统级 full tunnel。并且规则按请求中的 hostname 做判断，不在 guard 后执行一次“DNS 解析结果是否为 private IP”的二次过滤；“private 保持本地”对 literal IP 最明确，对任意本地域名不能作同等保证。

### 故障语义

| 事件 | 行为 |
| --- | --- |
| 用户主动 Disconnect 或 Logout | Chromium 清除 proxy setting；Firefox/Zen 的请求处理器因 `proxyPort` 为零而返回 `DIRECT`。 |
| Native host 意外退出 | Chromium 的 `onDisconnect` 清空 token，并用一个**没有 `subnetRoutes`** 的新对象替换 `status`，随后把按剩余状态生成的 PAC 指向 `0.0.0.1:1`。MagicDNS、Tailscale 地址和已选 exit mode 仍可被 park，但依赖已批准 subnet route 的 literal IP 可能变成 `DIRECT`。Firefox/Zen 在 `proxyPort` 为零时对所有请求返回 `DIRECT`。 |
| Chromium MV3 worker 重启 | 先 park 继承的 PAC，再启动新 host；新 port/token 就绪后重装 PAC。 |
| Exit node 离线、宿主仍运行 | 保留 exit selection；公网请求阻断，不回退本机公网。 |
| Proxy setting 被 policy 或其他扩展占用 | 不争抢控制权；UI 报告“节点连接但浏览器未实际路由”。 |
| PAC 构造或安装失败 | 报错，不宣称已路由；mandatory PAC 不允许脚本失败时直接回退。 |

因此，宿主崩溃时的 kill switch 只应表述为 Chromium 对 MagicDNS、Tailscale 地址和 exit-mode 公网目标的保护，并保留上述 subnet-route 缺口；Firefox/Zen 不提供宿主崩溃 kill switch。若该行为是硬要求，应选 Chromium，并单独规避或验收 subnet-routed literal IP。无论浏览器为何，只看节点 `Running` 都不足以判断功能可用，还必须确认 proxy ownership、port/token、路由配置和 exit-node active state。

## 身份、策略与信任边界

### 身份模型

- 一个浏览器 profile 对应一个 Tailtab node 和一个独立状态目录。
- 一个 Tailtab node 可以保存多个 Tailscale login profiles，并在它们之间切换。
- 同一个浏览器 profile 中的所有账户必须使用同一个 control server。
- control server 在首次登录时固定。已有账户后，设置页不能把该 profile 静默改指另一服务器。
- 新 control server 可以是 Headscale，但生产环境应只使用受信任的 HTTPS endpoint。

账户切换复用了 Tailscale login profiles，不会同时运行多个节点。切换时 Tailtab 立即清空旧账户的 peers、routes、exit nodes、tailnet 和地址，等待新账户状态到达；然后恢复 hostname、`WantRunning`、`RouteAll` 和该账户的 exit-node preference。

### 访问控制

Tailtab 节点仍是 tailnet 中的普通设备：

- Tailscale control plane 只向它分发获准访问的 peer 公钥、路由与策略。
- Grants/ACLs 在 Tailscale 客户端侧编译执行。
- Tailtab 的 destination guard 只能进一步收窄本地代理目标，不能绕过 tailnet policy。
- Tailtab 不提供自己的用户、组、角色、审批、设备 posture 或审计控制面。

因此最小权限应配置在 tailnet policy，而不是依赖浏览器 profile 名称作为安全策略。

示例使用该节点的稳定 Tailscale IP 建立 host alias，只允许访问带 `tag:internal-web` 的 HTTP/HTTPS 服务：

```json
{
  "hosts": {
    "tailtab-corp": "100.100.100.10"
  },
  "grants": [
    {
      "src": ["tailtab-corp"],
      "dst": ["tag:internal-web"],
      "ip": ["tcp:80", "tcp:443"]
    }
  ]
}
```

实际地址应从 admin console 中该 Tailtab 节点读取。若要允许它使用 exit node，还需按当前 policy 显式授予 `autogroup:internet`；“允许连接 exit-node 机器本身”不等于“允许把它当互联网网关”。

### 本地安全边界

| 边界 | 已有保护 | 剩余责任 |
| --- | --- | --- |
| 扩展 -> native host | Native Messaging manifest allowlist；profile/browser/control URL 校验；1 MiB 帧限制 | 扩展本身和浏览器 profile 必须可信。 |
| 本机进程 -> loopback proxy | 每进程 32-byte token；HTTP 先认证后判目标；SOCKS5 禁止 no-auth | 同一用户下的本机恶意代码仍是强对手，不能把 loopback 当远程隔离边界。 |
| Proxy -> destination | browser rule + host guard 双检；宿主收到的请求会拒绝保留地址和恶意宽路由 | guard 依据输入 hostname，不验证最终 DNS 解析地址；浏览器在宿主故障时还有下述 egress gap。 |
| Node -> tailnet | WireGuard；Tailscale Grants/ACLs；control plane 分发获准 peer | tailnet 管理员、control server、identity provider 和 policy 仍是信任根。 |
| Release -> endpoint | installer 校验同一 release 中的 `SHA256SUMS` | checksum 防传输损坏，不等于发布者签名；宿主尚未 code-sign/notarize。 |

### 已知风险

1. `*.ts.net` 中不属于当前 tailnet 的名称可能在 `UserDial` 中回退到系统 resolver，形成一个窄 DNS egress 路径。
2. 自定义 control server 下发的 MagicDNS suffix 只做语法和边界校验。恶意服务器仍可给出类似 `co.uk` 的二级公共域并扩大代理范围。
3. Chromium 需要 `webRequest`、`webRequestAuthProvider` 和 `<all_urls>` 才能回答 proxy 407。这是实现约束，也是高权限扩展的供应链风险。
4. `tsnet` 会向 `log.tailscale.com` 上传自身日志，Tailtab 当前没有受支持的关闭方式。
5. Tailtab 不提供账户删除 UI；登出后仍需在 admin console 删除机器。
6. 它只代理浏览器可表达的 HTTP/SOCKS TCP 流量，不提供通用 UDP、ICMP、系统 DNS、入站端口或其他进程的透明网络。
7. 宿主崩溃时，Chromium 会在丢失 `subnetRoutes` 后 park PAC，导致部分 subnet-routed literal IP 变成 `DIRECT`；Firefox/Zen 因 `proxyPort=0` 对所有请求返回 `DIRECT`。
8. Firefox/Zen 未启用 **Run in Private Windows** 时，private windows 不受 Tailtab 路由覆盖。

## 与 Tailscale 的系统性对比

| 维度 | Tailtab | 原生 Tailscale |
| --- | --- | --- |
| 抽象层 | 浏览器 profile 的应用层代理适配器 | 设备级 overlay network 客户端与托管控制面 |
| 控制面 | 不自带；使用 Tailscale 或 Headscale | Tailscale coordination service；可用 Headscale 替代部分控制面 |
| 数据面 | 嵌入式 `tsnet`，因此仍是 Tailscale WireGuard 数据面 | `tailscaled`/平台客户端管理系统 TUN 或 userspace 模式 |
| 流量范围 | 当前 profile 的 HTTP/HTTPS 等代理流量 | 整机任意应用的 IP 流量 |
| 身份粒度 | 每浏览器 profile 一个节点 | 通常每设备或每服务一个节点 |
| 系统权限 | 用户目录安装，无 root/admin | 一般需要安装系统服务、VPN extension 或 TUN |
| DNS | 只对被代理请求使用 `proxyDNS`/`UserDial`；规则前置分类 hostname | 系统级 MagicDNS、split DNS 与 resolver 集成 |
| 子网路由消费 | 支持被批准 route，但浏览器规则和 host guard 会额外收窄 | OS route table 或 netstack 按 control-plane route 工作 |
| Exit node | 只影响 profile；private literal IP 默认旁路；宿主在线但 exit node 离线时阻断公网，宿主崩溃保护仅限 Chromium 且有 subnet-route 缺口 | 设备级默认路由；LAN access 默认关闭、可显式开启 |
| 入站服务 | 未暴露 `tsnet.Listen`；不能作为通用服务端 | 可监听 tailnet、Tailscale SSH、Serve、Funnel、Services |
| 协议 | HTTP proxy 与 SOCKS5 TCP | TCP、UDP、ICMP 等 IP 层能力 |
| 策略 | 继承 Tailscale policy，外加本地 destination guard | Grants/ACLs、posture、tags、routes、system policy 等 |
| 可观测性 | Popup 状态、warnings、宿主 stderr、`tsnet` 日志 | `tailscale status/ping/netcheck/bugreport`、admin console、审计与 flow logs |
| 更新与分发 | GitHub release；`v0.2.2` 的 Firefox XPI 和 Chromium unpacked 包都需手工升级 | 各平台正式安装包、自动更新与企业分发能力 |
| 成熟度 | Experimental | 生产产品与大规模多平台客户端 |

Tailscale 的 userspace networking mode 也能提供 SOCKS5/HTTP proxy，但主要面向容器和 serverless。Tailtab 的独特价值不在“userspace proxy”本身，而在把 profile 生命周期、browser routing、proxy authentication、账户切换、popup 状态和 Chromium parked-PAC 恢复组合成一个浏览器产品。

## 如何使用

### 最小安装

当前 release 为 `0.2.2`，但安装器在该 release 之后才加入仓库，因此 `v0.2.2/scripts/install.sh` 是 404。macOS/Linux 应固定较新的安装脚本提交，同时用 `TAILTAB_VERSION=0.2.2` 固定下载的 release assets：

```bash
curl -fsSLo /tmp/tailtab-install.sh \
  https://raw.githubusercontent.com/Stocist/Tailtab/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/scripts/install.sh

less /tmp/tailtab-install.sh
TAILTAB_VERSION=0.2.2 sh /tmp/tailtab-install.sh
```

这个脚本比 `v0.2.2` 新，支持 macOS 和 Linux。Windows 使用同一提交中的 PowerShell 安装器：

```powershell
$script = "$env:TEMP\tailtab-install.ps1"
Invoke-WebRequest `
  https://raw.githubusercontent.com/Stocist/Tailtab/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/scripts/install.ps1 `
  -OutFile $script
Get-Content $script
$env:TAILTAB_VERSION = "0.2.2"
& $script
```

两种脚本都会：

1. 按 OS/architecture 下载宿主二进制。
2. 用 release 中的 `SHA256SUMS` 校验文件。
3. 安装到 `~/.local/bin/tailtab` 或 `%LOCALAPPDATA%\tailtab\tailtab.exe`。
4. 为支持的浏览器写 Native Messaging manifest。

固定脚本提交只保证下载的脚本文本不再漂移；`SHA256SUMS` 与二进制来自同一 GitHub release，只能发现传输损坏，不能替代发布者签名或防御 release 账户被攻破。

然后安装扩展：

- Zen/Firefox：打开 release 中 `tailtab-0.2.2.xpi`；若要覆盖 private windows，还必须在 `about:addons` 为 Tailtab 启用 **Run in Private Windows**，否则这些窗口会绕过 Tailtab。
- Edge/Chrome：解压 `tailtab-chromium-0.2.2.zip`，在扩展页开启 developer mode 并 Load unpacked。

`v0.2.2` XPI 不包含 `update_url`，该 release 也没有 `updates.json`，所以必须手工升级。只有后续 release 的实际 XPI 包含 `update_url`，且同一 release 确实发布 `updates.json` 时，Firefox 自动更新才成立；较新源码中存在这套机制，不代表 `v0.2.2` 已具备它。

macOS 首次运行可能被 Gatekeeper 拦截，因为二进制未 notarize。不要全局关闭 Gatekeeper；只在确认 release、checksum 和文件来源后，对该二进制执行一次明确放行。

### 首次连接

1. 为该用途创建独立浏览器 profile，例如 `corp-tailnet`。
2. 安装 Tailtab 扩展并打开 popup。
3. 若使用 Headscale，在首次登录前进入 Settings，填入受信任的 HTTPS control URL。
4. 点击 Connect。
5. 在打开的 Tailscale/Headscale 登录页审批新节点。
6. 在 admin console 确认机器名、owner、Tailscale IP、key expiry 和可访问策略。
7. 用一个允许的 tailnet URL 和一个普通公网 URL 分别验证 split routing。

示例：

```text
http://wiki/              -> Tailtab proxy -> tailnet
https://service.ts.net/   -> Tailtab proxy -> tailnet
https://github.com/       -> direct in split mode
```

### 验证

Tailtab popup 应同时满足：

- node state 为 `Connected`/`Running`
- 没有 proxy ownership 或 PAC error
- 显示预期 tailnet、device name 和 Tailscale IP
- 若选择 exit node，状态显示该节点 active，而不是 selected-but-offline

实际验证建议：

```bash
# 只记录宿主机公网出口基线；这条命令不验证 Tailtab 浏览器路由。
curl https://ifconfig.me

# 以下地址必须在安装 Tailtab 的目标浏览器 profile 内打开。
https://ifconfig.me
https://www.dnsleaktest.com
```

命令行 `curl` 不会自动使用 Tailtab，因为 token 不对 popup 暴露；它只提供 host egress baseline。实际验收必须在目标 Tailtab browser profile 完成：

1. Split mode：访问允许的 tailnet URL，并在服务端确认源身份是 Tailtab 节点；再访问 `https://ifconfig.me`，确认公网出口等于 host baseline。
2. Exit mode：选择在线 exit node 后，在同一 profile 验证 `ifconfig.me` 显示 exit-node 出口，并运行 DNS leak test。
3. Offline：保持宿主运行并让已选 exit node 离线，确认该 profile 的公网请求失败，而不是回到 host baseline。
4. Host crash：在测试环境中让 native host 保持不可用、阻止自动重连后，分别测试 MagicDNS、exit-mode 公网地址和一个 subnet-routed literal IP。Chromium 前两者应被 parked PAC 阻断，但当前实现可能让该 subnet IP 变成 `DIRECT`；Firefox/Zen 会对这些请求返回 `DIRECT`，不能作为 kill switch。

不应通过修改扩展把 token 持久化后供其他进程复用。

### 多账户与 Headscale

- 同一 control server 上的多个账户可以放在一个 browser profile，通过 popup 切换。
- Tailscale SaaS 与 Headscale 必须使用不同 browser profiles。
- 已登录 profile 的 control server pin 不应被改写；要迁移服务器，先在 admin console 注销/删除旧节点，再使用新的 browser profile。
- Headscale 只替代协调控制面，不自动替代所有 Tailscale 托管能力。DERP、OIDC、DNS、HA、备份、升级、证书与可观测性由部署者明确承担。

## 如何优雅使用

### 推荐拓扑

```text
Host operating system
    |
    +-- Native Tailscale node
    |      |
    |      +-- shell, IDE, SSH, databases, system services
    |
    +-- Browser profile: corp
    |      |
    |      +-- Tailtab node -> corporate tailnet web services
    |
    +-- Browser profile: lab
           |
           +-- Tailtab node -> lab Headscale web services
```

这套组合把身份边界放在 profile，而不是频繁登录/登出：

1. 原生 Tailscale 处理整机网络、开发工具和非 HTTP 协议。
2. Tailtab profile 只承载需要另一身份或另一 control server 的 Web 工作流。
3. 每个 profile 只加入一个信任域，并使用最小权限 grant。
4. exit node 只在确实需要 profile 级公网出口时开启；不要把它误认为整机 VPN。

### 团队部署准则

| 事项 | 推荐做法 |
| --- | --- |
| 版本 | 固定 release，不跟随 `latest`；升级前在测试 profile 验证。 |
| 来源 | 镜像并复核 release；企业环境补充签名、公证或内部制品签名。 |
| Browser profile | 一用途一 profile；避免安装无关高权限扩展。 |
| Control server | 首次登录前配置；生产只用 HTTPS；一个 profile 一个 server。 |
| Policy | deny by default；以稳定 Tailscale IP/host alias 精确限制源，以 service tag 限制目标与端口。 |
| Key lifecycle | 明确 key expiry；过期、离职、profile 删除时同步清理 admin console 中的节点。 |
| Exit node | 明确 public egress、DNS resolver 与 LAN bypass；分别做 IP 和 DNS leak 验证。 |
| 故障演练 | 分浏览器测试 host crash、Chromium subnet-route literal IP、worker restart、exit node offline、proxy policy conflict 和版本不一致。 |
| 可观测性 | 监控 popup warning 与 control-plane machine 状态；不要用系统 `tailscale status` 代表嵌入式 Tailtab node。 |

### 不推荐的用法

- 不要用 Tailtab 替代服务器、数据库客户端、SSH、UDP 应用或整机零信任接入。
- 不要把同一个 browser profile 当作多个组织/control server 的共享容器。
- 不要持久化或导出 proxy token；它的短生命周期是本地隔离设计的一部分。
- 不要从 Chromium 移除 parked PAC 或 mandatory PAC；它们保护 MagicDNS 与 exit path，但当前宿主崩溃路径仍不覆盖丢失的 subnet routes。
- 不要只按 `Running` 判断可用；必须同时验证浏览器 proxy setting 已被当前扩展控制。
- 不要把 private literal IP 旁路的 exit mode 描述成全流量 VPN。
- 不要在企业环境直接依赖未签名 native binary 和 unpacked Chromium extension。

## 决策建议

采用 Tailtab 的必要条件：

1. 需求明确是“浏览器 profile 级身份与路由隔离”。
2. 目标主要是 Web/TCP 服务，而非通用设备网络。
3. 可以接受 experimental 项目的分发和维护成本。
4. tailnet policy 能把该节点限制到最小目标和端口。
5. 能验证并接受 `tsnet` logging、扩展权限、手工更新、Firefox private-window 配置、浏览器特定故障语义和已知 egress gap。

任一条件不成立时，优先使用原生 Tailscale 客户端。若唯一诉求是容器中的 userspace proxy，应直接评估 `tailscaled --tun=userspace-networking`，因为它减少了浏览器扩展和 Native Messaging 两层生命周期。

## 证据账本

| 结论 | 主要证据 |
| --- | --- |
| Tailtab 是每 profile 一个 `tsnet` node | [`README`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/README.md)、[`Node.Start`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/internal/node/node.go#L284-L366)、[`handleInit`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/cmd/tailtab/host.go#L246-L291) |
| 代理使用临时 token 和双层 destination check | [`proxy.NewToken/guard/dialTailnet`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/internal/proxy/proxy.go#L42-L53)、[`proxy.go`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/internal/proxy/proxy.go#L260-L287)、[`handler`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/internal/proxy/proxy.go#L507-L620) |
| Chromium/Firefox 分别使用 PAC 与 per-request SOCKS5 | [`background.js`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/extension/background.js#L13-L160)、[`rules.js`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/extension/rules.js#L226-L269) |
| Chromium 宿主崩溃时 park proxy，但 `onDisconnect` 丢失 `subnetRoutes`；Firefox 无 port 时 `DIRECT` | [`proxy.onRequest`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/extension/background.js#L68-L95)、[`parkProxy/onDisconnect`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/extension/background.js#L217-L315)、[`setStatus`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/extension/background.js#L407-L435) |
| `v0.2.2` 无安装脚本和 Firefox 自动更新元数据；相关机制在后续源码中加入 | [`v0.2.2` tree](https://github.com/Stocist/Tailtab/tree/v0.2.2)、[`v0.2.2` Firefox manifest](https://github.com/Stocist/Tailtab/blob/v0.2.2/extension/manifest.firefox.json)、[后续 Firefox manifest](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/extension/manifest.firefox.json)、[后续 release workflow](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/.github/workflows/release.yml#L78-L126) |
| Firefox private windows 需要显式授权 | [`README`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/README.md#L114-L121)、[`popup warning`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/extension/popup.js#L583-L595) |
| Control server per profile 固定 | [`pinControlURL`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/internal/node/node.go#L200-L242)、[`AddAccount`](https://github.com/Stocist/Tailtab/blob/58dd707b4473f12b4b9bb8b2a5275b684241a7d6/internal/node/node.go#L873-L920) |
| Tailscale 控制面和数据面职责 | [Control and data planes](https://tailscale.com/docs/concepts/control-data-planes) |
| Direct、Peer Relay、DERP 路径 | [Connection types](https://tailscale.com/docs/reference/connection-types) |
| `tsnet` 是独立用户态 Tailscale node | [`tsnet` README](https://github.com/tailscale/tailscale/blob/3945b82f8a9550b54c33e61d4ed2227862d53e8a/tsnet/README.md) |
| Grants deny-by-default 及 selector 语义 | [Access control](https://tailscale.com/docs/features/access-control)、[Grants syntax](https://tailscale.com/docs/reference/syntax/grants) |
| 原生 exit node 的 LAN 默认值 | [Exit nodes](https://tailscale.com/docs/features/exit-nodes) |
| 原生 userspace proxy 的产品边界 | [Userspace networking mode](https://tailscale.com/docs/concepts/userspace-networking) |

## 可复核命令

```bash
git clone https://github.com/Stocist/Tailtab.git
cd Tailtab
git checkout 58dd707b4473f12b4b9bb8b2a5275b684241a7d6

go version
node --version
cat go.mod

find cmd internal -name '*.go' ! -name '*_test.go' -print0 | xargs -0 cat | wc -l
find cmd internal -name '*_test.go' -print0 | xargs -0 cat | wc -l
cat extension/background.js extension/rules.js extension/popup.js extension/options.js | wc -l

./scripts/test.sh
./scripts/build.sh
node scripts/smoke.js bin/tailtab
```
