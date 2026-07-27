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

```bash
cd projects/metrics-console
uv venv .venv --python 3.13
source .venv/bin/activate
uv pip install -e .
# or: uv pip install duckdb fastapi 'uvicorn[standard]' pydantic-settings

export METRICS_QUACK_URI=quack:10.37.125.152:9494
export METRICS_QUACK_TOKEN_FILE=~/.config/metrics-console/quack.token
export METRICS_CONSOLE_PORT=9496

python -m metrics_console.app
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
