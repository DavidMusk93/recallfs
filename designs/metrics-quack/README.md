# Metrics Quack 主从架构

## 一句话结论

在 **d2 宿主机** 以 DuckDB + Quack（`:9494`）作为 metrics **master**；**dev 容器**内 e2ed / e2e-orchestrator 作为 writer client，经 `172.17.0.1:9494` 写入；按 category **分库（ATTACH）** + 表内 `event_date` 分片，本地 NDJSON 缓冲保证写路径可用。

## 拓扑

```text
Mac
 ├─ ssh d2 :22  ──► d2 host
 │                    /root/data/duck/
 │                    systemd: duckdb-quack.service
 │                    quack:0.0.0.0:9494
 │                    master.duckdb + cat_*.duckdb
 │
 └─ ssh dev :2222 ──► docker container (tide-dev-agent-dev-container)
                        e2ed / e2e-orchestrator
                        metrics client → quack:172.17.0.1:9494
```

| 角色 | 位置 | 职责 |
| --- | --- | --- |
| Master | d2 宿主机 | Quack server、权威存储、分析查询 |
| Writer | dev 容器 | 进程事件 / API / 生命周期写入 |
| 本地缓冲 | `runs/metrics/*.ndjson` | master 不可达时不丢事件 |
| 冷备份 | `/root/data/duck/backup/` | 周期 CHECKPOINT + 文件快照（从副本雏形） |

## 分库分表

| 层级 | 策略 | 说明 |
| --- | --- | --- |
| 分库 | category 独立 `.duckdb` | `cat_e2ed` / `cat_orchestrator` / `cat_ops`，master ATTACH |
| 分 schema | 每库 `main` | 简单；跨 category 分析用 master 上 VIEW |
| 分表 | 热表 + 日分区键 | 热表持续 INSERT；`event_date DATE` 支撑按日过滤/归档 |
| 归档 | Parquet hive | 老数据 `COPY ... TO 'backup/…/dt=YYYY-MM-DD/*.parquet'` |

不在单表上做 MySQL 式物理分表；列式 + 分区键更适合 DuckDB 分析。

## Schema 概要

通用信封（所有 category 表共享字段风格）：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `ts` | TIMESTAMPTZ | 事件时间 |
| `event_date` | DATE | `ts::DATE`，分区/过滤 |
| `host` | VARCHAR | 主机/容器标识 |
| `instance` | VARCHAR | 如 `tide-vs-ck` |
| `source` | VARCHAR | `e2ed` / `orchestrator` |
| `level` | VARCHAR | debug/info/warn/error |
| `event` | VARCHAR | 稳定事件名 |
| `attrs` | JSON | 扩展字段 |
| `trace_id` | VARCHAR | 可选关联 |

category 表见 `schema/001_init.sql`。

## 写路径

```text
Rust (e2ed / orchestratord)
  → MetricsSink.record(event)
  → 内存 ring + 落盘 NDJSON (best-effort)
  → 后台 flush（Python quack client 或本地 duckdb ATTACH）
  → INSERT INTO cat_*.table
```

约束：

- 写失败 **不阻塞** 业务主路径（log warn）
- 批量 INSERT（默认 50 条或 2s）
- token 仅 env / 文件，不进 git

## 运维 SOTA

| 项 | 做法 |
| --- | --- |
| 进程 | `systemd` unit `duckdb-quack` |
| 数据目录 | `/root/data/duck/{db,schema,bin,logs,secrets,backup}` |
| 扩展 | DuckDB ≥1.5.3 `INSTALL quack FROM core` |
| 鉴权 | 固定 token 文件 `secrets/quack.token`（chmod 600） |
| 监听 | `0.0.0.0:9494` + `allow_other_hostname`（内网 docker） |
| 备份 | `duckdb_backup.sh` 周期 CHECKPOINT + copy |
| 观测 | `whoami()`、`duckdb_logs_parsed('Quack')` |

## 环境变量（writer）

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `METRICS_ENABLED` | `1` | 关闭写 metrics |
| `METRICS_QUACK_URI` | `quack:172.17.0.1:9494` | 容器→宿主机 |
| `METRICS_QUACK_TOKEN_FILE` | `/root/data/duck/secrets/quack.token` 或挂载路径 | token |
| `METRICS_INSTANCE` | `tide-vs-ck` | 实例名 |
| `METRICS_BUFFER_DIR` | `<run-dir>/metrics` | 本地缓冲 |

## 阶段

1. d2 server + schema + systemd  
2. dev client 联通  
3. e2ed 接入  
4. orchestrator 接入  
5. 用写入数据改进两项目  

## 参考

- [Quack overview](https://duckdb.org/docs/current/quack/overview.html)
- [Quack reference](https://duckdb.org/docs/current/quack/reference.html)
