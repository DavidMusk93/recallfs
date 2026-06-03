# Tmux

目标：让 agent 用 `tmux` 管理长周期命令，减少 sandbox terminal 混乱，避免忘记回收和交互中断。

## 1. 使用场景

| 场景 | 是否使用 tmux | 说明 |
| --- | --- | --- |
| 长时间构建、压测、同步 | 是 | 避免终端断开导致任务丢失 |
| 需要持续观察日志 | 是 | 便于 attach、capture-pane、detach |
| 多进程协同任务 | 是 | 用 session/window 区分角色 |
| 一次性短命令 | 否 | 直接执行更简单 |
| 需要用户即时交互 | 谨慎 | 优先改成参数化或非交互命令 |

## 2. 会话守则

- 先查已有 session，再决定是否新建。
- session 名必须表达任务，例如 `sync-docs`、`bench-fringedb`。
- 一个任务优先一个 session，不要无限制创建。
- 长周期任务必须把输出写到日志文件，便于后续复盘。
- 完成任务后，记录结果，再决定是否保留 session。
- 不依赖“写文件判断是否结束”，优先使用 `tmux` 的状态和 pane 输出。

## 3. 基本流程

```text
+-----------------------+
| 需要长周期命令?        |
+-----------+-----------+
            |
            v
+-----------------------+---- no ---->[ 直接运行命令 ]
| 需要 tmux?             |
+-----------+-----------+
            | yes
            v
+-----------------------+
| 查询已有 session       |
+-----------+-----------+
            |
            v
+-----------------------+---- yes ---->+-----------------------+
| session 已存在?        |              | 复用已有 session      |
+-----------+-----------+              +-----------+-----------+
            | no                                  |
            v                                     |
+-----------------------+                         |
| 创建命名 session       |<------------------------+
+-----------+-----------+
            |
            v
+-----------------------+
| 发送命令并写日志       |
+-----------+-----------+
            |
            v
+-----------------------+
| capture-pane 观察结果  |
+-----------+-----------+
            |
            v
+-----------------------+
| 记录状态并交付结果     |
+-----------------------+
```

## 4. 常用命令

| 目标 | 命令 |
| --- | --- |
| 查看 session | `tmux ls` |
| 新建 session | `tmux new-session -d -s <name>` |
| 发送命令 | `tmux send-keys -t <name> '<cmd>' C-m` |
| 查看 pane 内容 | `tmux capture-pane -pt <name>` |
| 进入 session | `tmux attach -t <name>` |
| 关闭 session | `tmux kill-session -t <name>` |

## 5. 推荐模板

```bash
session="sync-docs"
log=".tmp/logs/${session}.log"
mkdir -p .tmp/logs

tmux has-session -t "$session" 2>/dev/null || tmux new-session -d -s "$session"
tmux send-keys -t "$session" "bash ops.sh -p stream_engine 2>&1 | tee '$log'" C-m
tmux capture-pane -pt "$session"
```

## 6. 结束判定

- 优先通过 `tmux capture-pane -pt <name>` 查看最新输出。
- 如果命令会返回 shell prompt，观察是否已经回到 prompt。
- 如果命令写日志，结合日志尾部判断是否完成。
- 如果任务是服务进程，确认监听端口、健康检查或日志 ready 标志。
- 不要只凭“文件存在”判断任务完成。

## 7. 清理规则

- 不要批量 kill 不认识的 session。
- 只清理当前任务明确创建的 session。
- 长周期服务类 session 可保留，但要在结果中说明名称和用途。
- 临时任务完成后，可关闭对应 session，避免后台堆积。

## 8. Agent 提醒

- `tmux` 是为了让任务持续运行，不是为了制造更多后台状态。
- session 名、日志路径、关键命令都要在结果中说明。
- 能参数化的交互不要放进 tmux，先改成非交互命令。
- 如果 session 状态不确定，先观察再操作，不要直接清理。
