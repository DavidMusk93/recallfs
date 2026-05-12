# api

api 应该尽可能简单。

核心思想不是一开始就提供一个“滚动升级”大接口，而是先定义少量稳定、语义清晰的原子接口，再通过组合这些接口实现复杂功能。

简单接口的价值

- 每个接口只做一件事，语义清晰，调用方容易理解。
- 接口边界稳定后，复杂流程可以在上层自由编排。
- 原子接口更容易复用、测试、审计和做失败恢复。
- 复杂能力应该来自组合，而不是来自一个超大接口塞满参数。

## job 管理原子接口

下面用 job 管理举例。先不要定义 `RollingUpgradeJob()` 这种“大而全”的接口，而是先定义原子动作。

```text
CreateJob(spec) -> job_id
GetJob(job_id) -> job
ListTasks(job_id) -> tasks

UpdateJobSpec(job_id, new_spec) -> ok
PauseJob(job_id) -> ok
ResumeJob(job_id) -> ok

DrainTask(task_id) -> ok
StartTask(job_id, task_spec) -> task_id
StopTask(task_id) -> ok
DeleteTask(task_id) -> ok

GetTask(task_id) -> task
WaitTaskState(task_id, target_state, timeout) -> ok | timeout
WaitJobStable(job_id, timeout) -> ok | timeout

ShiftTraffic(job_id, from_set, to_set, ratio) -> ok
RollbackJobSpec(job_id, old_spec) -> ok
```

这些接口的职责应该尽量单一：

- `CreateJob` / `GetJob`：负责 job 生命周期和查询。
- `ListTasks` / `GetTask`：负责观察当前实例分布和状态。
- `UpdateJobSpec`：负责声明目标版本、资源或配置，不直接承诺如何升级。
- `PauseJob` / `ResumeJob`：负责冻结和恢复调度。
- `DrainTask`：负责把某个实例从流量或消费集合里摘出来。
- `StartTask` / `StopTask` / `DeleteTask`：负责实例级别变更。
- `WaitTaskState` / `WaitJobStable`：负责把异步过程显式化，避免调用方猜状态。
- `ShiftTraffic`：负责流量切换，不和实例创建、删除绑死。
- `RollbackJobSpec`：负责恢复到旧版本目标状态。

接口设计要点

- 查询接口和变更接口分开，不要让 `GetJob` 隐式触发修复动作。
- 控制接口和等待接口分开，不要让 `StartTask` 内部偷偷阻塞很久。
- 流量切换和实例变更分开，这样同一组原子接口还能复用在灰度发布、扩缩容、机房迁移中。
- 每个接口都要尽量幂等，否则失败重试会把编排层搞复杂。

## 通过组合实现滚动升级

有了上面的原子接口后，滚动升级就只是一个上层 workflow，而不是底层必须内建的特殊能力。

目标：把 job 从 `v1` 升级到 `v2`，始终保持服务可用，并且每次只替换一小批实例。

```text
+-------------------------------------+
| 1. 读取当前 job 与 task 状态        |
+-------------------------------------+
                  |
                  v
+-------------------------------------+
| 2. 暂停调度，冻结实例集合           |
+-------------------------------------+
                  |
                  v
+-------------------------------------+
| 3. 更新 job 目标 spec 为 v2         |
+-------------------------------------+
                  |
                  v
+-------------------------------------+
| 4. 选择一批旧实例 old_batch         |
+-------------------------------------+
                  |
                  v
+-------------------------------------+
| 5. 对 old_batch 执行 drain          |
+-------------------------------------+
                  |
                  v
+-------------------------------------+
| 6. 启动同规模新实例 new_batch       |
+-------------------------------------+
                  |
                  v
+-------------------------------------+---- no ----->+----------------------------------+
| 7. 等待 new_batch ready?            |              | 失败: 回滚 spec / 恢复流量       |
+-------------------------------------+              +----------------------------------+
                  | yes                                              |
                  v                                                  v
+-------------------------------------+              +----------------------------------+
| 8. 切流到 new_batch                 |              | ResumeJob 并退出                |
+-------------------------------------+              +----------------------------------+
                  |
                  v
+-------------------------------------+
| 9. 停止并删除 old_batch             |
+-------------------------------------+
                  |
                  v
+-------------------------------------+---- no ----->+----------------------------------+
| 10. job 稳定?                       |              | 失败: 回滚并恢复旧实例集         |
+-------------------------------------+              +----------------------------------+
                  | yes
                  v
+-------------------------------------+
| 11. 处理下一批实例                  |
+-------------------------------------+
                  |
                  v
+-------------------------------------+
| 12. 全量完成后恢复调度              |
+-------------------------------------+
```

可以把它翻译成伪代码：

```text
job = GetJob(job_id)
tasks = ListTasks(job_id)
old_spec = job.spec

PauseJob(job_id)
UpdateJobSpec(job_id, spec_v2)

for old_batch in Batch(old_tasks, batch_size):
    for task in old_batch:
        DrainTask(task.id)

    new_batch = []
    for _ in old_batch:
        new_task_id = StartTask(job_id, task_spec_v2)
        new_batch.append(new_task_id)

    for task_id in new_batch:
        WaitTaskState(task_id, "Ready", timeout)

    ShiftTraffic(job_id, old_batch, new_batch, 100%)

    for task in old_batch:
        StopTask(task.id)
        DeleteTask(task.id)

    WaitJobStable(job_id, timeout)

ResumeJob(job_id)
```

## 为什么这种方式更好

- 滚动升级不是特例。灰度发布、回滚、扩容、缩容都可以复用同一套原子接口。
- 底层接口简单，编排层可以按不同业务目标选择批大小、等待策略、切流策略。
- 出问题时更容易定位，是 `DrainTask` 失败，还是 `WaitTaskState` 超时，而不是一个 `UpgradeJob()` 返回了模糊错误。
- 更容易做安全控制，比如只允许某些角色调用 `ShiftTraffic`，但允许更多角色调用 `GetJob`。

## L3: 生产级 API 组合

如果要到 L3 级别，目标就不再只是“能升级成功”，而是：

- 升级过程可审计。
- 失败后可自动回滚。
- 重试不会破坏系统状态。
- 支持人工接管和继续执行。
- 对调度器、流量系统、告警系统、权限系统都是一致的。

这时接口仍然应该保持原子化，但要补上生产环境真正需要的控制面能力。

### 生产级原子接口

在前面的基础上，再补一组保护性接口：

```text
AcquireJobLease(job_id, owner, ttl) -> lease_id
RenewJobLease(job_id, lease_id, ttl) -> ok
ReleaseJobLease(job_id, lease_id) -> ok

CreateOperation(job_id, type, params, idempotency_key) -> op_id
GetOperation(op_id) -> operation
AppendOperationEvent(op_id, event) -> ok

CreateChangeTicket(job_id, summary, risk) -> ticket_id
ApproveChange(ticket_id, approver) -> ok

SetJobAlertMode(job_id, mode) -> ok
CheckSLO(job_id, window) -> pass | fail
RunHealthCheck(task_set) -> pass | fail

CheckpointJob(job_id) -> checkpoint_id
RestoreJob(job_id, checkpoint_id) -> ok

MarkManualIntervention(op_id, reason) -> ok
AbortOperation(op_id) -> ok
```

这些接口分别解决的问题：

- `AcquireJobLease`：防止两个 orchestrator 同时升级同一个 job。
- `CreateOperation`：给整次升级一个稳定 operation id，便于断点续跑和审计。
- `AppendOperationEvent`：把关键步骤写成事件流，而不是靠日志 grep。
- `CreateChangeTicket` / `ApproveChange`：把变更审批显式化。
- `SetJobAlertMode`：升级期间可能需要切换告警策略，比如从 page 调整为 observe，但必须可追踪。
- `CheckSLO` / `RunHealthCheck`：把“业务健康”变成显式 gate，而不是只看进程活着。
- `CheckpointJob` / `RestoreJob`：用于有状态 job 的回滚。
- `MarkManualIntervention` / `AbortOperation`：让人工接管成为一等能力，而不是只能 SSH 上去手修。

### L3 级滚动升级流程

下面是一个生产环境的组合案例。

场景：

- job 当前运行版本为 `v1`
- 目标升级到 `v2`
- 服务必须保持可用
- 每批最多替换 `10%`
- 任意一步触发 `SLO fail`，自动停止并回滚
- 升级过程中允许人工介入，但必须留下操作痕迹

```text
+----------------------------------------+
| 1. 创建变更单并拿到审批                |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 2. 创建 operation                      |
|    绑定 idempotency_key                |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 3. 获取 job lease                      |
|    防止并发升级                        |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 4. 读取 job / tasks / traffic          |
|    建立基线状态                        |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 5. 创建 checkpoint                     |
|    保存 spec / task set / route        |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 6. PauseJob + 调整 alert mode          |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 7. UpdateJobSpec 到 v2                 |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 8. 选择 10% old_batch                  |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 9. Drain old_batch                     |
|    等待无 in-flight request/message    |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 10. Start new_batch                    |
+----------------------------------------+
                    |
                    v
+----------------------------------------+---- no ----->+----------------------------------+
| 11. Wait Ready + HealthCheck?          |              | 回滚到 checkpoint                |
+----------------------------------------+              | 恢复 route / spec / task set    |
                    | yes                               +----------------------------------+
                    v                                                  |
+----------------------------------------+                             v
| 12. ShiftTraffic 10% -> v2             |              +----------------------------------+
+----------------------------------------+              | 标记 operation failed             |
                    |                                   | ReleaseJobLease                   |
                    v                                   +----------------------------------+
+----------------------------------------+---- no ----->+----------------------------------+
| 13. CheckSLO?                           |              | 自动回滚 + 进入人工接管          |
+----------------------------------------+              +----------------------------------+
                    | yes
                    v
+----------------------------------------+
| 14. Stop / Delete old_batch            |
+----------------------------------------+
                    |
                    v
+----------------------------------------+---- no ----->+----------------------------------+
| 15. WaitJobStable?                     |              | 自动回滚 + 进入人工接管          |
+----------------------------------------+              +----------------------------------+
                    | yes
                    v
+----------------------------------------+
| 16. 下一批                             |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 17. 全量完成                           |
|    恢复 alert mode / ResumeJob         |
+----------------------------------------+
                    |
                    v
+----------------------------------------+
| 18. 标记 operation succeeded           |
|    ReleaseJobLease                     |
+----------------------------------------+
```

### L3 级组合伪代码

```text
ticket_id = CreateChangeTicket(job_id, "roll upgrade v1 -> v2", "medium")
ApproveChange(ticket_id, approver)

op_id = CreateOperation(
    job_id,
    "rolling_upgrade",
    {from: "v1", to: "v2", batch_ratio: "10%"},
    idempotency_key,
)

lease_id = AcquireJobLease(job_id, owner=op_id, ttl="10m")
AppendOperationEvent(op_id, "lease-acquired")

job = GetJob(job_id)
tasks = ListTasks(job_id)
checkpoint_id = CheckpointJob(job_id)
AppendOperationEvent(op_id, "checkpoint-created")

PauseJob(job_id)
SetJobAlertMode(job_id, "upgrade-observe")
UpdateJobSpec(job_id, spec_v2)
AppendOperationEvent(op_id, "upgrade-started")

for old_batch in Batch(ListTasks(job_id, version="v1"), ratio="10%"):
    RenewJobLease(job_id, lease_id, ttl="10m")
    AppendOperationEvent(op_id, {"step": "batch-selected", "batch": old_batch})

    for task in old_batch:
        DrainTask(task.id)

    new_batch = []
    for _ in old_batch:
        task_id = StartTask(job_id, task_spec_v2)
        new_batch.append(task_id)

    for task_id in new_batch:
        WaitTaskState(task_id, "Ready", timeout="15m")

    if RunHealthCheck(new_batch) == fail:
        AppendOperationEvent(op_id, "health-check-failed")
        RestoreJob(job_id, checkpoint_id)
        MarkManualIntervention(op_id, "new batch unhealthy")
        AbortOperation(op_id)
        ReleaseJobLease(job_id, lease_id)
        return

    ShiftTraffic(job_id, old_batch, new_batch, ratio="10%")

    if CheckSLO(job_id, window="5m") == fail:
        AppendOperationEvent(op_id, "slo-regression")
        RestoreJob(job_id, checkpoint_id)
        MarkManualIntervention(op_id, "slo regression after traffic shift")
        AbortOperation(op_id)
        ReleaseJobLease(job_id, lease_id)
        return

    for task in old_batch:
        StopTask(task.id)
        DeleteTask(task.id)

    WaitJobStable(job_id, timeout="10m")
    AppendOperationEvent(op_id, {"step": "batch-finished", "batch": old_batch})

SetJobAlertMode(job_id, "normal")
ResumeJob(job_id)
AppendOperationEvent(op_id, "upgrade-succeeded")
ReleaseJobLease(job_id, lease_id)
```

### 这个案例为什么算 L3

- 有审批：不是任何人都能直接升级。
- 有租约：不会并发修改同一个 job。
- 有 operation：升级是可追踪、可恢复的实体，不是一次无上下文脚本执行。
- 有 checkpoint：失败后不是“尽量修”，而是有明确恢复点。
- 有健康门禁：不仅看进程状态，还看流量后业务指标。
- 有人工接管：自动化失败后能停在清晰状态，交给人继续处理。
- 有审计事件：每个关键步骤都有结构化记录。

### 设计上的关键取舍

- 不要做一个 `UpgradeJob(job_id, to=v2, batch=10%, autoRollback=true, ...)` 超级接口。
- 这种超级接口在 demo 阶段看起来方便，但生产里通常会失控：
- 参数越来越多，语义越来越模糊。
- 出错时不知道卡在哪一步。
- 不同业务的灰度策略、检查项、回滚策略很难复用。
- 人工接管和断点恢复会非常痛苦。

更合理的分层是：

- 原子 API：提供稳定、可幂等、可审计的积木。
- workflow engine / orchestrator：组合这些积木，形成滚动升级、灰度、回滚、迁移等生产流程。
- policy 层：定义 batch 大小、SLO 门槛、审批规则、回滚阈值。

结论

复杂系统能力应该优先由“简单原子接口 + 上层编排”提供。到 L3 级别时，不是去设计一个更大的升级 API，而是给原子接口补齐生产环境真正需要的能力：租约、审批、审计、checkpoint、健康门禁、回滚和人工接管。
