# HTTP Port Manager

## 需求

- 将多端口静态 HTTP 服务做成 **macOS 系统服务**（launchd）
- Web 管理界面：创建 / 启停 / 删除多个常驻目录服务
- 监控与 metrics 通过 **SSE** 推送到前端（无轮询）
- 默认承载 `learning/algorithms` 于 `:8000`

## 设计

```text
launchd com.user.http-port-manager
        |
        v
  server.py (single process)
        |
        +-- Control HTTP  :9090
        |     /            dashboard UI
        |     /api/services  list / create / patch / delete
        |     /api/services/{id}/start|stop|restart
        |     /api/events    SSE metrics stream
        |     /api/healthz
        |
        +-- Worker ThreadingHTTPServer :8000  root=algorithms
        +-- Worker ThreadingHTTPServer :N     root=...
        |
        v
  config.json  (persist services + control bind)
  state/pids not needed (in-process workers)
```

## 健康判定

```text
healthy = running AND listening
running  = worker thread alive + server socket open
listening = TCP port accept (probe connect)
```

## Metrics（SSE）

| field | meaning |
| --- | --- |
| `status` | stopped / starting / running / error |
| `pid` | manager process pid（in-process workers share it） |
| `port` / `bind` / `root` | service config |
| `uptime_sec` | since last successful start |
| `requests_total` | HTTP requests handled |
| `bytes_sent` | response body bytes |
| `errors_total` | handler exceptions / 4xx-5xx counts |
| `last_request_at` | last request ISO time |
| `last_error` | last start/handler error |

SSE 规则：状态签名变化才发 `event: update`；无变化发 `: heartbeat`。

## 为何 in-process

- 与 ssh-socks「直接管子进程」不同：静态目录服务无外部二进制依赖
- 指标零 IPC、启停更快、一个 launchd unit 即可
- 仍按「服务」模型暴露 API，后续可扩展 `command` 型外部进程

## macOS TCC

```text
Terminal ./install.sh | ./sync.sh
        |  (可读 Documents)
        v
  rsync root -> ~/Library/Application Support/.../mirrors/<id>
        |
        v
  launchd python3 server.py
        |  (不可读 Documents)
        v
  ThreadingHTTPServer serves mirrors only
```

UI 上 Sync 若源目录对 LaunchAgent 不可读，会提示改跑 `sync.sh`。

## 路径

| path | role |
| --- | --- |
| `tools/http-port-manager/server.py` | 源码入口（仓库） |
| `tools/http-port-manager/config.json` | 默认配置模板 |
| `tools/http-port-manager/launchd/*.plist` | launchd 模板 |
| `tools/http-port-manager/install.sh` | 安装 / 同步 / 重载 |
| `~/Library/Application Support/http-port-manager/` | 运行时副本（避开 Documents TCC） |
| `~/Library/LaunchAgents/com.user.http-port-manager.plist` | 已安装 agent |
| `~/Library/Logs/http-port-manager/` | stdout/stderr |
