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
| 概览 | 窗口 KPI、avg/p95 sparkline、最近 API + top paths |
| API 延迟 | path 聚合（含 p95）+ 请求样本 |
| 生命周期 | e2ed start/stop + daemon ticks |
| 组件 | `component_ops` 窗口事件 |
| 表浏览 | `cat_*` 表 + 当前时间窗采样 |
| SQL | 只读 SQL（拦截 INSERT/UPDATE/DROP…） |

### 时间范围 + 差分更新

| API | 作用 |
| --- | --- |
| `GET /api/snapshot?range=15m\|1h\|6h\|24h\|7d` | 全量窗口（并行 Quack） |
| `GET /api/delta?range=&since=<ts>` | 仅 `ts > since` 的新行 + 重算 KPI/序列 |
| `GET /api/overview?range=&since=` | 兼容入口（有 since 走 delta） |

前端：分段控件切换 range → snapshot；Live 每 5s delta 合并，不全表闪烁。KPI 为 **窗口计数**（非全表 COUNT）。

## 设计（apple-design）

- System font、毛玻璃侧栏/顶栏、0.5px 分隔、分段控件
- KPI 柔和色条、SVG sparkline（avg 实线 + p95 虚线）
- 指针按下即时反馈；`prefers-reduced-motion` / `reduced-transparency`
- 写路径仍在 e2ed/orchestrator；本台 **只读分析**

## 环境变量

| 变量 | 默认 |
| --- | --- |
| `METRICS_QUACK_URI` | `quack:10.37.125.152:9494` |
| `METRICS_QUACK_TOKEN_FILE` | `~/.config/metrics-console/quack.token` |
| `METRICS_CONSOLE_HOST` | `127.0.0.1` |
| `METRICS_CONSOLE_PORT` | `9496` |
