# Metrics Console

本机 Apple 风格分析台：经 **DuckDB Quack** 只读查询 d2 上的 metrics master。

```text
Browser (Mac :9496)
  → FastAPI metrics-console
  → quack:10.37.125.152:9494  (d2 duckdb-quack.service)
  → cat_e2ed / cat_orchestrator / cat_ops
```

## 依赖

- Python 3.11+（推荐 uv + 3.13）
- 本机可访问 d2 `:9494`（内网已验证 `10.37.125.152`）
- Quack token（**不进 git**）

```bash
mkdir -p ~/.config/metrics-console
ssh d2 'cat /root/data/duck/secrets/quack.token' > ~/.config/metrics-console/quack.token
chmod 600 ~/.config/metrics-console/quack.token
```

## 启动

### A. Tunnel Manager（推荐，launchd 托管）

本机 **服务站** `http://127.0.0.1:9020` 已将 `metrics-console` 注册为 `kind=service`。

macOS LaunchAgent **读不了** `~/Documents`（TCC）。运行时镜像在：

```text
~/Library/Application Support/metrics-console/
```

改代码后同步并重启：

```bash
bash projects/metrics-console/sync-runtime.sh
curl -sS -X POST http://127.0.0.1:9020/api/tunnel/metrics-console/restart
open http://127.0.0.1:9496/
```

| 操作 | API |
| --- | --- |
| 状态 | `GET /api/tunnel/metrics-console` |
| 启动 | `POST /api/tunnel/metrics-console/start`（可 adopt 已监听进程） |
| 停止 | `POST /api/tunnel/metrics-console/stop` |
| 重启 | `POST /api/tunnel/metrics-console/restart` |

`stop_on_exit=false`：Tunnel Manager 自身重启时 **不** 误杀分析台。

### B. 开发机前台（Terminal，可读 Documents）

```bash
cd projects/metrics-console
bash run.sh
# open http://127.0.0.1:9496/
```

## 功能

| 视图 | 内容 |
| --- | --- |
| Overview | KPI、6h API latency 条形图、最近请求 |
| API latency | path 聚合 + 明细 |
| Lifecycle | e2ed start/stop + daemon ticks |
| Tables | 浏览 `cat_*` 表 |
| SQL | 只读 SQL（拦截 INSERT/UPDATE/DROP…） |

## 设计

- System font、毛玻璃侧栏、大字号 KPI、克制 accent（`#0071e3`）
- 按钮 `:active` 即时缩放；`prefers-reduced-motion` / `reduced-transparency` 降级
- 写路径仍在 e2ed/orchestrator；本台 **只读分析**

## 环境变量

| 变量 | 默认 |
| --- | --- |
| `METRICS_QUACK_URI` | `quack:10.37.125.152:9494` |
| `METRICS_QUACK_TOKEN_FILE` | `~/.config/metrics-console/quack.token` |
| `METRICS_CONSOLE_HOST` | `127.0.0.1` |
| `METRICS_CONSOLE_PORT` | `9496` |
