# SQLite WAL-Reset Bug 研究

> 原文：[How we tracked down a 16-year-old SQLite bug](https://tailscale.com/blog/sqlite-wal-reset-bug)
>
> 阅读与验证日期：2026-09-08。本文以 SQLite 官方文档、修复提交、
> 新增回归测试和真实 SQLite `3.51.2`/`3.51.3` C 复现为证据。

## 1. 结论先行

这是一个 **WAL checkpoint 与另一连接的 WAL reset 并发时产生的状态代际
混淆**。旧 checkpointer 缓存了上一代 WAL 的 `mxFrame`，writer 随后重置
WAL、递增 salt 并从 frame 1 写入新事务；旧 checkpointer 没有重新核对
salt，却把共享 `nBackfill` 更新为旧 WAL 的较大值。后续 checkpoint 因而
跳过新 WAL 中已经 commit 的 frame，造成永久数据丢失，部分布局下进一步
表现为索引/表不一致和数据库损坏。

关键边界：

- 受影响：SQLite `3.7.0` 到 `3.51.2`。
- 已修复：`3.51.3+`；官方另有 `3.44.6`、`3.50.7` 回移版本。
- 必要条件：WAL 模式、同一文件至少两个独立连接、不同线程或进程、
  write/reset 与 checkpoint 命中特定并发窗口。
- 单进程不等于安全；多个连接仍可触发。
- `PRAGMA integrity_check` 不是充分探针：本研究实际观测到
  `committed=106, recovered=104, integrity=ok`。

无法升级时，按可靠性排序：

1. **能重编 SQLite：回移官方 salt 校验补丁。**
2. **必须保留 WAL：让同一数据库的全部 write 和 checkpoint 经过一个全局
   owner/gate；多进程必须使用跨进程协调。**
3. **可以牺牲 WAL 并发：在停写窗口切回 rollback journal。**
4. 仅降低 checkpoint 频率、改 `synchronous`、增大 `busy_timeout`、改用
   `FULL`/`TRUNCATE` 但允许其降级为 PASSIVE，都不是完整修复。

## 2. 触发状态机

先定义三个 wal-index 字段：

| 字段 | 含义 |
| --- | --- |
| `mxFrame` | 当前 WAL 最后一个有效 frame |
| `nBackfill` | 已确认回填进主数据库的最大 frame |
| `aSalt` | WAL 代际；reset/wrap 后变化 |

触发顺序不是普通的“两线程同时写”：

```text
checkpointer A                 writer B                  wal-index
     |                            |                     nBackfill = L
     | read cached header         |                     salt = S
     | mxFrame = L                |
     |                            | reset WAL
     |                            | append m frames      nBackfill = 0
     |                            | commit               salt = S + 1
     | acquire read-lock 0        |
     | stale test: 0 < L          |
     | backfill old range         |
     | publish nBackfill = L      |                     nBackfill = L
     |                            | append past frame L
     | later checkpoint skips new frames 1..L
     v
acknowledged write is absent from the database file
```

完整触发条件是：

1. 第一次 checkpoint 完整回填，使 WAL 处于可 reset 状态。
2. 第二次 checkpoint 读取旧 header，缓存较大的 `mxFrame=L`。
3. 第二次 checkpoint 尚未取得 read-lock 0 时，另一连接 commit；writer
   将 WAL reset，设置 `nBackfill=0`、更新 salt，并从开头写入少量新 frame。
4. 第二次 checkpoint 继续使用旧 `mxFrame=L`，误把 `nBackfill` 写回 `L`。
5. 更多事务让新 WAL 超过 `L`。
6. 第三次 checkpoint 相信 `nBackfill=L`，跳过本代 WAL 前 `L` 个 frame。

PASSIVE checkpoint 持有 checkpoint lock，所以不会与另一个 checkpoint
重叠；但它不持有 writer lock，因此可以与 writer 重叠。问题恰好位于
“读取 header”和“取得 read-lock 0”之间。

## 3. 修复为什么有效

SQLite 的修复很小，但检查位置决定了正确性：

```c
WalIndexHdr *pLive = (WalIndexHdr*)walIndexHdr(pWal);
int bChg = memcmp(pLive->aSalt, pWal->hdr.aSalt,
                  sizeof(pWal->hdr.aSalt));
if( 0==bChg ){
  /* backfill and publish nBackfill */
}
```

该比较发生在 read-lock 0 已取得之后。若实时 salt 与缓存 salt 不同，
说明 WAL 在窗口中已经 reset；旧 checkpoint 不再回填，也不再发布旧
`nBackfill`。只在早期读取 header 时比较 salt 没有意义，因为 writer 仍可
在比较后 reset。

上游同时加入 `sqlite3FaultSim(660)` 和新的 `test/walrestart.test`，让测试
在这个精确位置调用第二连接执行 `UPDATE`，稳定制造过去无法稳定命中的
调度顺序。完整补丁见 `evidence/upstream-fix.diff`。

## 4. 为什么 16 年没有测出来

结论不是“SQLite 没有测试”。官方列出的测试能力包括：

- deployed configuration 的 100% branch coverage；
- 数百万测试和数亿次 release soak 实例；
- `mptester` 多进程压力测试与 `threadtest3` 多线程压力测试；
- I/O fault、crash/power-loss、fuzz、assert 和动态分析。

缺口在测试维度与 oracle，而不是单纯代码覆盖率：

| 缺口 | 为什么普通测试会漏 |
| --- | --- |
| 并发 schedule | branch coverage 只能证明两条分支都执行过，不能证明跨连接的五阶段交错被执行 |
| 窗口极窄 | 官方在修复时无法自然复现，只能加入 test-control callback 强制插入 writer |
| 原因与结果延迟 | 第二次 checkpoint 写坏进度，第三次 checkpoint 才真正跳过新 frame |
| oracle 不完整 | 丢失整笔事务时 B-tree 仍可自洽，`integrity_check` 可以返回 `ok` |
| 常见负载放大不足 | 默认自动 checkpoint 远少于 Tailscale 的主动、激进 checkpoint |
| 故障归因困难 | 极低频损坏更容易被归因到存储、内存、锁或应用误用 |

2026-08-24，SQLite 官方文档才补充 Phil Eaton 的自然复现器。它利用大文件
`mmap` 后的 `munmap` 拉长关键窗口，不需要修改 SQLite，也不需要 VFS
测试钩子。这并不否定原有测试，而是说明测试原先缺少 **可控调度 +
acknowledged-write oracle**。

从发现到修复也需分开看：

- Tailscale 2025-08 首次从备份流水线发现异常，六个月共处理 19 次事故。
- 2026-03-03 SQLite 定位并提交修复。
- 3.52.0 曾携带修复，但因无关的 text-to-float 变化造成 stale expression
  index 告警而撤回。
- 2026-03-13 发布 3.51.3。

因此，16 年主要是 **发现和可复现性延迟**；根因明确后，上游修复并不慢。

## 5. 独立复现

Demo 使用真实官方 amalgamation；不修改 SQLite，不注入测试 hook。大表
和 1 GiB mmap 用于扩大 `munmap` 窗口。每个成功 `sqlite3_step()` 都增加
进程外部的 acknowledged counter，最终 checkpoint 后再比较数据库行数。

### 5.1 本机 macOS

Apple M5 Pro，Darwin 25.6.0，Apple Clang 21：

| SQLite | 模式 | 轮次 | 已确认提交 | 结果 |
| --- | --- | ---: | ---: | --- |
| 3.51.2 | unsafe | 3 次运行，第 17/17/4 轮停止 | 618/632/106 | 3/3 丢失 |
| 3.51.3 | unsafe | 400 | 15,102 | 未观测到丢失 |
| 3.51.2 | serialized | 400 | 992 | 未观测到丢失 |
| 3.51.2 | rollback | 400 | 804 | 未观测到丢失 |

第三次 vulnerable run 丢了两笔，但数据库结构仍自洽：

```text
sqlite_version=3.51.2
mode=unsafe
committed=106
recovered=104
lost=2
integrity=ok
outcome=loss-detected
```

### 5.2 EPYC/Linux 目标机

Debian 12 / Linux 5.15，双路 AMD EPYC 7Y83，ext4/NVMe，GCC 12.2：

| SQLite | 模式 | 轮次 | 已确认提交 | 结果 |
| --- | --- | ---: | ---: | --- |
| 3.51.2 | unsafe | 3 次运行，第 6/7/4 轮停止 | 671/692/447 | 3/3 丢失 |
| 3.51.3 | unsafe | 400 | 68,200 | 未观测到丢失 |
| 3.51.2 | serialized | 400 | 6,106 | 未观测到丢失 |
| 3.51.2 | rollback | 400 | 800 | 未观测到丢失 |

这些是 correctness stress，不是 benchmark。有限次无故障不是安全证明；
结构性论据是后两种规避分别消除了“并发 write/checkpoint”和“WAL”前提。

## 6. 无法升级时怎么做

### 6.1 先确认真实运行时

不要看系统 `sqlite3` CLI 或依赖声明来猜版本。对每个实际写路径使用的
连接执行：

```sql
SELECT sqlite_version(), sqlite_source_id();
PRAGMA journal_mode;
PRAGMA wal_autocheckpoint;
```

还要盘点同一进程是否链接了多份 SQLite，以及是否有 daemon、CLI、worker、
sidecar 或旧依赖同时打开同一文件。版本矩阵必须来自实际连接。

### 6.2 能重编但不能升级：回移补丁

优先把官方 check-in `7168988acbec2d8d` 的 salt revalidation 回移到当前
vendor source。若处在 3.44 或 3.50 系列，优先采用官方 `3.44.6` 或
`3.50.7`，不要维护私有手改。

回移门禁：

1. 对照当前 `walCheckpoint()`，确认比较位于 read-lock 0 之后。
2. 移植 forced-interleaving regression test，而不是只编译成功。
3. 运行本 study 的自然 reproducer 做 vulnerable/patched A/B。
4. 查询生产二进制中的 `sqlite_source_id()`，确认实际加载的是 patched
   copy。

### 6.3 不能改二进制、必须保留 WAL：全局串行化

为每个数据库文件建立唯一写入/checkpoint owner：

```text
all write requests ----+
                       +---> one owner/gate ---> SQLite write connection
all checkpoint calls --+

read-only connections -----------------------> SQLite read connections
```

要求：

- 所有 write transaction 和所有 checkpoint 都经过同一 gate。
- 连接池只能有一个 write-capable connection；其余连接设为
  `PRAGMA query_only=ON` 并在应用层禁止升级为 writer。
- 多进程场景必须有跨进程 owner/IPC/锁；进程内 mutex 不够。
- 每个连接关闭自动 checkpoint：

```sql
PRAGMA wal_autocheckpoint=0;
```

- owner 只在停止接收并排空 writer 后执行 checkpoint；同时监控 WAL 大小、
  磁盘余量和读延迟，避免用“永不 checkpoint”交换成磁盘事故。

若不能证明所有 writer 和 checkpoint 入口都受控，这个方案就不成立。

### 6.4 可以牺牲 WAL 并发：切回 rollback journal

在维护窗口停止所有连接和写流量，备份并校验后执行：

```sql
PRAGMA wal_checkpoint(TRUNCATE);
PRAGMA journal_mode=DELETE;
```

必须检查返回值确实是 `delete`，再让应用重连。rollback journal 不经过
WAL-reset 状态机，因此该 bug 不适用；代价是 reader/writer 并发性下降。
若数据库已经损坏，切模式不会修复既有损坏，应先恢复或重建。

## 7. 哪些做法不够

| 做法 | 结论 |
| --- | --- |
| 降低 checkpoint 频率 | 只降低概率，不消除竞态 |
| `busy_timeout` | 处理锁等待，不修复过期 header |
| `synchronous=FULL` | 改变持久化屏障，不修复状态代际 |
| SQLite serialized threading mode | 保护单连接调用，不建立跨连接数据库级 gate |
| 只改成 `FULL`/`RESTART`/`TRUNCATE` | 获取 writer lock 失败时 API 可退化为 PASSIVE 并返回 `SQLITE_BUSY` |
| 只跑 `integrity_check` | 可漏掉结构一致的 committed-write loss |
| 直接复制在线 `.db` | 可能漏 WAL，本身也不是一致备份 |

如果没有能力升级/回移、无法保证全局串行化、又不能退出 WAL，则只能降低和
监测风险，不能声称已规避。

## 8. 检测与恢复

预防之外还应补齐：

1. 在 SQLite 外记录单调 transaction ID 或可重放操作日志，确认 ACK 后的
   写入仍可见。
2. 备份使用 `sqlite3_backup`、`VACUUM INTO` 或已验证的一致性工具，不复制
   在线主文件。
3. 对备份重新打开后执行 `integrity_check`，再检查业务不变量、计数和
   transaction watermark。
4. 监控 checkpoint 的 `pnLog`、`pnCkpt`、WAL 大小及异常关系；它们是告警
   线索，不是修复。
5. 怀疑命中时立即停写，保留 `.db`、`-wal`、`-shm` 和进程日志的同一时刻
   快照，再从已验证备份加事务日志恢复。

## 9. 交付物

- `source.md`：来源、版本边界与引用。
- `exploration.md`：源码路径、测试缺口、失败尝试与环境。
- `demo/`：C11 自然复现器和 CMake 工程。
- `evidence/`：FIL-C、macOS、EPYC/Linux、源码 diff 与 digest。
- `report.html`：浏览器可读的证据优先报告。

完整命令见 `demo/README.md`。
