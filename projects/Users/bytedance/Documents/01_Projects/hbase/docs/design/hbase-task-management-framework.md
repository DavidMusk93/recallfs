# HBase Task Management Framework

## 1. 结论

HBase 的任务管理框架不是单一实现，而是由三套职责不同的机制共同构成：

| 层次 | 主要模块 | 核心抽象 | 解决问题 |
| --- | --- | --- | --- |
| 可恢复任务框架 | `hbase-procedure` | `Procedure` / `ProcedureExecutor` / `ProcedureStore` / `ProcedureScheduler` | Master 侧多步骤、可持久化、可恢复、可回滚的分布式状态变更 |
| 事件执行框架 | `hbase-server` | `ExecutorService` / `EventHandler` / `EventType` / `ExecutorType` | Master / RegionServer 内部异步事件的线程池执行 |
| 运行时任务观测 | `hbase-server` | `TaskMonitor` / `MonitoredTask` / `MonitoredRPCHandler` | JVM 内长耗时任务和 RPC handler 的状态展示、泄漏清理和卡住告警 |

其中，Procedure Framework 是 HBase 当前最核心的任务管理模型，负责建表、删表、region assign、server crash、snapshot 等需要跨进程故障恢复的操作。ExecutorService 更像轻量异步事件队列，不提供 ProcedureStore 级别的持久化恢复语义。TaskMonitor 不是执行器，只是运行时诊断与可观测设施。

## 2. 目标与边界

### 2.1 目标

- 梳理 HBase 中“任务管理”相关框架的边界、职责和关键类。
- 说明 Procedure、ExecutorService、TaskMonitor 三者之间的关系。
- 整理 Procedure 状态机、数据持久化规则、多用户保护、资源管理和性能瓶颈，方便后续查阅。

### 2.2 非目标

- 不逐一展开每一种具体 Master Procedure 的业务 enum 和全部状态转换。
- 不给出基于线上指标或压测数据的量化性能结论。
- 不设计新的任务框架替代方案。

### 2.3 当前问题

HBase 代码中存在多个看起来都像“任务”的概念：

- `Procedure` 表示可恢复的持久化任务。
- `EventHandler` 表示异步事件处理单元。
- `MonitoredTask` 表示可观测的运行时状态对象。
- Shell 中的 `taskmonitor` 命令只查询 Web UI 暴露的运行时任务，不等价于 Procedure 列表。

如果不先区分这些概念，容易把“执行框架”和“监控框架”混在一起，导致排查入口错误。

## 3. 总体架构

```text
                       client / admin / master internal caller
                                      |
                                      v
                         submit Procedure or EventHandler
                                      |
             +------------------------+------------------------+
             |                                                 |
             v                                                 v
   +--------------------+                          +----------------------+
   | ProcedureExecutor  |                          | ExecutorService      |
   | hbase-procedure    |                          | hbase-server         |
   +--------------------+                          +----------------------+
             |                                                 |
             | uses                                            | runs
             v                                                 v
   +--------------------+      persists       +--------------------------+
   | ProcedureScheduler |<------------------->| ProcedureStore / WAL     |
   +--------------------+                     +--------------------------+
             |
             | executes business procedures
             v
   +-------------------------------+
   | Master Procedure implementations |
   | hbase-server/master/procedure |
   +-------------------------------+

   +-------------------------------------------------------------+
   | TaskMonitor                                                  |
   | observes long-running tasks and RPC handlers in current JVM   |
   +-------------------------------------------------------------+
```

## 4. Procedure Framework

Procedure Framework 位于 `hbase-procedure`，官方文档称为 Procedure Framework PV2。它的设计目标是把分散在代码中的分布式状态变更统一为可恢复的状态机。

### 4.1 核心职责

| 组件 | 路径 | 职责 |
| --- | --- | --- |
| `Procedure` | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/Procedure.java` | 任务基类，保存 framework 状态、proc id、父子关系、锁、超时、结果和异常 |
| `ProcedureExecutor` | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/ProcedureExecutor.java` | 线程池执行器，提交、调度、恢复、完成查询、失败回滚 |
| `ProcedureScheduler` | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/ProcedureScheduler.java` | 可运行 Procedure 队列和锁资源视图 |
| `ProcedureStore` | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/store/ProcedureStore.java` | Procedure 持久化接口 |
| `WALProcedureStore` | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/store/wal/WALProcedureStore.java` | 基于 WAL 的持久化实现 |
| `StateMachineProcedure` | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/StateMachineProcedure.java` | 状态机式 Procedure 基类 |
| `RemoteProcedureDispatcher` | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/RemoteProcedureDispatcher.java` | 向远端 server 批量分发 Procedure 动作 |

### 4.2 执行语义

`ProcedureExecutor` 持续调用 `Procedure.execute(env)`，直到任务完成。`execute` 的结果决定后续行为：

| `execute` 结果 | 含义 |
| --- | --- |
| `null` | 当前 Procedure 完成 |
| `this` | 当前 Procedure 还有后续步骤，需要持久化状态后再次执行 |
| `Procedure[]` | 创建子 Procedure，父 Procedure 等待子树完成后继续 |
| `ProcedureYieldException` / `InterruptedException` | 让出执行权，稍后重新调度 |
| `ProcedureSuspendedException` | 挂起并等待外部事件唤醒 |
| 其他异常 | 标记失败，框架尝试 rollback |

重要约束：

- `execute` 可能因为失败或重启被重复调用，业务逻辑必须幂等。
- `rollback` 也可能重复调用，也必须幂等。
- Procedure Framework 的状态和业务 Procedure 自己维护的状态要区分清楚。
- 每次执行后的关键状态需要序列化到 `ProcedureStore`，否则 Master 崩溃恢复后可能丢失进度。

### 4.3 完整状态机

Procedure Framework 的精髓是“两层状态机”：

| 层次 | 状态来源 | 作用 |
| --- | --- | --- |
| 框架状态机 | `ProcedureState` proto enum | 控制调度、持久化、等待、失败、回滚和终态 |
| 业务状态机 | `StateMachineProcedure` 子类的 enum | 把一个业务任务拆成多个可持久化、可重放、可回滚的步骤 |

框架状态来自 `hbase-protocol-shaded/src/main/protobuf/server/Procedure.proto` 的 `ProcedureState`：

| 状态 | 含义 | 是否终态 |
| --- | --- | --- |
| `INITIALIZING` | Procedure 构造中，尚未进入 executor | 否 |
| `RUNNABLE` | 已进入 executor，可调度执行 | 否 |
| `WAITING` | 等待子 Procedure 完成 | 否 |
| `WAITING_TIMEOUT` | 等待超时或外部事件 | 否 |
| `FAILED` | 执行失败，后续需要 rollback | 否 |
| `ROLLEDBACK` | 失败后已回滚 | 是 |
| `SUCCESS` | 执行成功 | 是 |

核心状态转换如下：

```text
+-------------+
| submit      |
+------+------+
       |
       v
+-------------+
| INITIALIZING|
+------+------+
       |
       v
+-------------+
| RUNNABLE    |
+------+------+
       |
       +-- execute returns null ------------------->+-------------+
       |                                            | SUCCESS     |
       |                                            +-------------+
       |
       +-- execute returns this / yield ------------+
       |                                            |
       |                                            v
       |                                    +-------------+
       |                                    | RUNNABLE    |
       |                                    +-------------+
       |
       +-- execute returns sub procedures ----------+
       |                                            |
       v                                            v
+-------------+                             +-------------+
| WAITING     |<---- children complete -----| child proc   |
+------+------+                             +-------------+
       |
       +-- child failed / current failed ---------->+-------------+
                                                   | FAILED      |
                                                   +------+------+
                                                          |
                                                          v
                                                   +-------------+
                                                   | rollback    |
                                                   +------+------+
                                                          |
                                                          v
                                                   +-------------+
                                                   | ROLLEDBACK  |
                                                   +-------------+

+-------------+
| RUNNABLE    |
+------+------+
       |
       +-- suspend / timeout wait ---------------->+-----------------+
                                                   | WAITING_TIMEOUT |
                                                   +--------+--------+
                                                            |
                                                            | external event / timeout
                                                            v
                                                   +-----------------+
                                                   | RUNNABLE        |
                                                   +-----------------+
```

复杂任务通常继承 `StateMachineProcedure<TEnvironment, TState>`，由子类定义业务 enum，并实现下列抽象方法：

| 抽象方法（子类必须实现） | 作用 |
| --- | --- |
| `getInitialState()` | 返回第一个业务状态 |
| `executeFromState(env, state)` | 执行一个业务状态步骤 |
| `rollbackState(env, state)` | 逆序回滚已执行状态 |
| `getState(int)` / `getStateId(TState)` | 在 enum 和持久化 ordinal 之间转换 |

此外，框架还提供以下方法供子类调用，子类不需要实现：

| 框架提供方法（子类调用） | 作用 |
| --- | --- |
| `setNextState(state)` | 在 `executeFromState` 中指定下一个业务状态 |

业务状态机的运行逻辑如下：

```text
+-------------------+
| ProcedureExecutor |
+---------+---------+
          |
          | calls Procedure.execute()
          v
+-----------------------+
| StateMachineProcedure |
+---------+-------------+
          |
          | get current / initial state
          v
+-----------------------+
| executeFromState      |
+---------+-------------+
          |
          +-- Flow.HAS_MORE_STATE ----->+----------------------+
          |                              | setNextState(next)   |
          |                              +----------+-----------+
          |                                         |
          |                                         v
          |                              +----------------------+
          |                              | persist state stack  |
          |                              +----------+-----------+
          |                                         |
          |                                         v
          |                              +----------------------+
          |                              | next step or yield   |
          |                              +----------------------+
          |
          +-- Flow.NO_MORE_STATE ------>+----------------------+
                                         | append EOF state     |
                                         +----------+-----------+
                                                    |
                                                    v
                                         +----------------------+
                                         | framework SUCCESS    |
                                         +----------------------+
```

回滚时，`StateMachineProcedure` 会按已执行业务状态栈逆序调用 `rollbackState`：

```text
+------------------------+
| states: S1, S2, S3, EOF|
+-----------+------------+
            |
            v
+------------------------+
| remove EOF if present  |
+-----------+------------+
            |
            v
+------------------------+
| rollbackState(S3)      |
+-----------+------------+
            |
            v
+------------------------+
| rollbackState(S2)      |
+-----------+------------+
            |
            v
+------------------------+
| rollbackState(S1)      |
+-----------+------------+
            |
            v
+------------------------+
| framework ROLLEDBACK   |
+------------------------+
```

这也是 Procedure 能支持 Master failover 的关键：业务步骤编号被序列化到 `StateMachineProcedureData.state`，Master 重启后可以从最后一个已持久化状态继续执行或逆序回滚。

### 4.4 恢复机制

Procedure 的恢复链路如下：

```text
+-------------------+
| ProcedureExecutor |
+---------+---------+
          |
          v
+-------------------+
| ProcedureStore    |
| load persisted    |
+---------+---------+
          |
          v
+-----------------------------+
| rebuild tree, nonce, queues |
+-------------+---------------+
              |
              v
+-----------------------------+
| resume unfinished Procedure |
+-------------+---------------+
              |
              v
+-----------------------------+
| continue failed rollback    |
+-----------------------------+
```

恢复能力依赖两个前提：

- Procedure 在每个关键步骤后持久化足够的业务状态。
- 每个步骤都能在重复执行时得到一致结果。

### 4.5 Master 集成

Master 侧具体业务 Procedure 位于 `hbase-server/src/main/java/org/apache/hadoop/hbase/master/procedure`。

`MasterProcedureEnv` 是通用 Procedure Framework 和 HMaster 运行环境之间的桥：

| 能力 | 来源 |
| --- | --- |
| 获取 Master 服务 | `getMasterServices()` |
| 获取 AssignmentManager | `getAssignmentManager()` |
| 获取 Master 专用调度器 | `getProcedureScheduler()` |
| 获取远端 RegionServer 分发器 | `getRemoteDispatcher()` |
| 获取异步任务线程池 | `getAsyncTaskExecutor()` |
| 等待 Master 初始化 | `waitInitialized(Procedure<?>)` |
| 唤醒或挂起 ProcedureEvent | `setEventReady(...)` |

典型 Master Procedure 包括：

| Procedure | 路径 | 任务类型 |
| --- | --- | --- |
| `ServerCrashProcedure` | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/procedure/ServerCrashProcedure.java` | RegionServer 崩溃恢复 |
| `SnapshotProcedure` | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/procedure/SnapshotProcedure.java` | Snapshot 操作 |
| `CloneSnapshotProcedure` | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/procedure/CloneSnapshotProcedure.java` | Clone snapshot |
| `RestoreSnapshotProcedure` | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/procedure/RestoreSnapshotProcedure.java` | Restore snapshot |

## 5. ExecutorService

ExecutorService 位于 `hbase-server/src/main/java/org/apache/hadoop/hbase/executor`，是 HBase 内部较传统的异步事件执行框架。

### 5.1 ExecutorService 核心职责

| 组件 | 路径 | 职责 |
| --- | --- | --- |
| `ExecutorService` | `hbase-server/src/main/java/org/apache/hadoop/hbase/executor/ExecutorService.java` | 管理多个命名 executor，每个 executor 底层是线程池和队列 |
| `EventHandler` | `hbase-server/src/main/java/org/apache/hadoop/hbase/executor/EventHandler.java` | 异步事件处理基类，实现 `Runnable` |
| `EventType` | `hbase-server/src/main/java/org/apache/hadoop/hbase/executor/EventType.java` | 定义事件类型，并映射 executor service type |
| `ExecutorType` | `hbase-server/src/main/java/org/apache/hadoop/hbase/executor/ExecutorType.java` | 定义 Master / RegionServer 侧 executor 类型 |
| `ExecutorStatus` | `hbase-server/src/main/java/org/apache/hadoop/hbase/executor/ExecutorStatus.java` | 输出 executor 当前线程和队列状态 |

### 5.2 ExecutorService 执行流程

```text
+-------------------+
| caller            |
| creates handler   |
+---------+---------+
          |
          | optional prepare()
          v
+-------------------+
| EventHandler      |
+---------+---------+
          |
          | submit()
          v
+-------------------+
| ExecutorService   |
+---------+---------+
          |
          | EventType -> ExecutorType
          v
+-------------------+
| thread pool       |
+---------+---------+
          |
          | run()
          v
+-------------------+
| process()         |
+-------------------+
```

### 5.3 与 Procedure 的差异

| 维度 | Procedure Framework | ExecutorService |
| --- | --- | --- |
| 持久化 | 有，依赖 `ProcedureStore` | 无内建持久化 |
| 崩溃恢复 | 可从上次持久化状态恢复 | 进程崩溃后队列内事件丢失 |
| 回滚 | 支持 rollback 语义 | 无统一 rollback |
| 子任务 | 支持子 Procedure 树 | 无统一子任务模型 |
| 适用场景 | Master 侧关键状态变更 | 轻量异步事件、内部后台处理 |

## 6. TaskMonitor

TaskMonitor 位于 `hbase-server/src/main/java/org/apache/hadoop/hbase/monitoring`。它维护当前 JVM 内正在执行或近期完成的任务状态，用于诊断。

### 6.1 TaskMonitor 核心职责

| 组件 | 路径 | 职责 |
| --- | --- | --- |
| `TaskMonitor` | `hbase-server/src/main/java/org/apache/hadoop/hbase/monitoring/TaskMonitor.java` | JVM 内单例任务监控器 |
| `MonitoredTask` | `hbase-server/src/main/java/org/apache/hadoop/hbase/monitoring/MonitoredTask.java` | 普通任务状态接口 |
| `MonitoredRPCHandler` | `hbase-server/src/main/java/org/apache/hadoop/hbase/monitoring/MonitoredRPCHandler.java` | RPC handler 专用状态接口 |
| `TaskGroup` | `hbase-server/src/main/java/org/apache/hadoop/hbase/monitoring/TaskGroup.java` | 一组相关任务的组合 |

### 6.2 监控行为

| 行为 | 说明 |
| --- | --- |
| 创建普通任务 | `createStatus(description)` 返回 `MonitoredTask` |
| 创建 RPC 任务 | `createRPCStatus(description)` 返回 `MonitoredRPCHandler` |
| 过滤查询 | `getTasks(filter)` 支持 general、handler、rpc、operation 等过滤类型 |
| 过期清理 | 已完成任务超过 `hbase.taskmonitor.expiration.time` 后可被清理 |
| 泄漏清理 | 弱引用失效且任务仍在运行时，记录泄漏警告并 cleanup |
| 卡住告警 | RPC 任务运行超过 `hbase.taskmonitor.rpc.warn.time` 后输出 warn |

### 6.3 可观测入口

| 入口 | 路径 | 说明 |
| --- | --- | --- |
| Master dump | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/http/MasterDumpServlet.java` | Master Web dump 输出任务和线程诊断信息 |
| RegionServer dump | `hbase-server/src/main/java/org/apache/hadoop/hbase/regionserver/http/RSDumpServlet.java` | RegionServer Web dump 输出任务和线程诊断信息 |
| Shell taskmonitor | `hbase-shell/src/main/ruby/hbase/taskmonitor.rb` | 访问 RegionServer Web JSON 并格式化展示任务 |

## 7. 数据边界

ProcedureStore 持久化的是“恢复任务所需的最小状态”，不是执行现场的完整内存快照。判断是否持久化时，核心标准是：Master 崩溃后是否需要它来继续执行、回滚、去重或返回客户端结果。

### 7.1 持久化数据规则总表

| 数据/规则 | 典型字段或对象 | 是否持久化 | 持久化位置或重建来源 | 保留原因 | 重启后处理 |
| --- | --- | --- | --- | --- | --- |
| Procedure 类型与身份 | `class_name`、`proc_id`、`parent_id` | 是 | `Procedure.proto` | 反射恢复具体 Procedure，重建父子关系 | load 时实例化 Procedure，并放入 `procedures` map |
| 提交上下文 | `submitted_time`、`owner` | 是 | `Procedure.proto` | 保留提交时间和请求用户，支持异步任务归属和审计 | 恢复到 Procedure 实例 |
| 框架调度状态 | `state`、`last_update`、`timeout` | 是 | `Procedure.proto` | 判断任务处于 runnable、waiting、timeout、failed 还是终态 | 重建 runnable、waiting、timeout 列表 |
| 回滚栈索引 | `stack_id`、`executed` | 是 | `Procedure.proto` | 判断哪些步骤已执行，失败时按正确顺序回滚 | 重建 `rollbackStack` |
| 客户端结果 | `exception`、`result` | 是 | `Procedure.proto` | 客户端可通过 procId 查询异步操作结果或异常 | 放入完成结果或恢复失败状态 |
| 业务内部状态 | `state_message`、旧字段 `state_data` | 是 | Procedure 子类的 `serializeStateData` | 保存表名、region、server、snapshot、业务 enum 等恢复所需上下文 | 子类 `deserializeStateData` 反序列化 |
| 业务状态机步骤 | `StateMachineProcedureData.state` | 是 | `StateMachineProcedure` 默认序列化 | 正向恢复知道当前步骤，失败回滚知道逆序步骤 | 恢复 `states` 和 `stateCount` |
| 幂等提交键 | `nonce_group`、`nonce` | 是 | `Procedure.proto` | 防止客户端重试、Master failover 后重复提交同一任务 | 重建 `nonceKeysToProcIdsMap` |
| 持锁语义 | `locked` | 是 | `Procedure.proto` | Procedure 可能持有跨步骤锁，重启后必须恢复资源互斥关系 | 先标记 `lockedWhenLoading`，再恢复锁 |
| 管理控制位 | `bypass`、`is_crytical_system_table` | 是 | `Procedure.proto` | 保留旁路执行、关键系统表等控制语义 | 恢复到 Procedure 实例 |
| ProcedureStore 最大 id | 历史最大 `proc_id` | 是 | WAL 或 RegionProcedureStore 行记录 | 防止 proc id 回退导致 remote procedure nonce 判重错误 | store load 时返回历史最大值 |
| 当前 Procedure 实例索引 | `procedures` map | 否 | 从持久化 Procedure 重建 | 只是内存快速索引 | load 后按 procId 重建 |
| 调度队列 | `scheduler` queues、`FairQueue` | 否 | 从 `state`、锁和 Procedure 类型重建 | 只是当前进程调度结构 | load 后重新入队 |
| 回滚运行时结构 | `rollbackStack` | 否 | 从 `stack_id`、父子关系、执行状态重建 | 只是 executor 内部回滚索引 | load 时重新构造 |
| Nonce 内存索引 | `nonceKeysToProcIdsMap` | 否 | 从持久化 nonce 字段重建 | 只是内存判重 map | load 时重新填充 |
| 恢复锁临时标记 | `lockedWhenLoading` | 否 | 从持久化 `locked` 推导 | 避免恢复阶段重复 acquire 或破坏锁顺序 | 恢复锁完成后参与正常执行 |
| 子任务等待计数 | `childrenLatch` | 否 | 从 Procedure 父子关系和状态推导 | 当前进程内等待子 Procedure 的计数 | load 时重新计算 |
| 单次持久化控制 | `persist` | 否 | 每次执行前重置 | 控制某次执行是否跳过 store update，不是恢复语义 | 重启后恢复默认行为 |
| TaskMonitor 状态 | `MonitoredTask`、`MonitoredRPCHandler` | 否 | 无恢复来源 | 仅当前 JVM 诊断信息 | 进程退出后丢弃 |
| ExecutorService 队列 | `EventHandler` queue | 否 | 无恢复来源 | 轻量异步事件，不提供崩溃恢复 | 进程退出后丢弃 |

### 7.2 Store 保存方式

当前分支还存在 Region-based ProcedureStore：`RegionProcedureStore` 使用 Master local store 的 `proc:d` 列保存序列化 protobuf。删除时先写空值，再由 `cleanup()` 真正清理，用于保留历史最大 `proc_id`，避免恢复后 proc id 回退。

### 7.3 持久化判断

| 判断问题 | 如果答案是是 | 结论 |
| --- | --- | --- |
| Master 崩溃后继续执行是否需要它？ | 是 | 必须进入 ProcedureStore |
| 回滚已完成步骤是否需要它？ | 是 | 必须进入 ProcedureStore |
| 客户端查询异步结果是否需要它？ | 是 | 应持久化为 `result` 或 `exception` |
| 客户端重试去重是否需要它？ | 是 | 应持久化 nonce |
| 资源锁跨步骤或跨重启是否需要它？ | 是 | 应持久化锁语义，而不是持久化锁对象本身 |
| 只是为了当前进程调度、缓存、监控或加速？ | 是 | 作为临时数据，重启后重建或丢弃 |

## 8. 多用户保护

HBase 任务管理过程中的多用户保护主要由四层构成：权限校验、owner 追踪、nonce 幂等、资源锁。

| 保护层 | 机制 | 解决问题 |
| --- | --- | --- |
| 权限校验 | Master coprocessor pre-hook，例如 `AccessController.preCreateTable` / `preDeleteTable` / `preModifyTable` | 防止无权限用户提交敏感任务 |
| owner 追踪 | Procedure 持久化 `owner` 字段，Master Procedure 构造时设置请求用户 | 记录任务归属，用于审计、结果归属和错误上下文 |
| nonce 幂等 | `nonce_group` + `nonce`，`ProcedureExecutor.registerNonce` | 防止客户端重试导致同一任务重复提交 |
| 资源锁 | namespace/table/region/server/peer/global/meta 锁 | 防止多个用户对同一 HBase 实体并发执行冲突操作 |

典型 DDL 请求在提交 Procedure 前会经过 Master coprocessor 的 pre-hook：

| 操作 | 保护点 |
| --- | --- |
| create table | `AccessController.preCreateTable` 要求 namespace/table 级 `ADMIN`、`CREATE` |
| delete table | `AccessController.preDeleteTable` 要求 table 级 `ADMIN`、`CREATE` |
| modify table | `AccessController.preModifyTable` 要求 table 级 `ADMIN`、`CREATE` |
| truncate table | `AccessController.preTruncateTable` 要求 table 级 `ADMIN`、`CREATE` |

权限校验失败时，Procedure 通常不会被提交到 executor。Procedure 的 `owner` 是持久化字段，但 owner 本身不是并发控制锁，也不替代权限系统；它的价值是把异步任务与提交用户关联起来，避免 Master failover 后丢失任务归属上下文。

Nonce 保护的流程如下：

```text
+--------------------------+
| client request           |
| nonce_group + nonce      |
+------------+-------------+
             |
             v
+--------------------------+
| submitProcedure          |
+------------+-------------+
             |
             v
+--------------------------+
| registerNonce(nonceKey)  |
+------------+-------------+
             |
             +-- existing procId ----->+--------------------------+
             |                         | return old procId       |
             |                         | skip duplicate submit   |
             |                         +--------------------------+
             |
             +-- no existing procId -->+--------------------------+
                                       | pre-hook + submit       |
                                       +------------+-------------+
                                                    |
                                                    v
                                       +--------------------------+
                                       | persist nonce           |
                                       +--------------------------+
```

恢复时，`ProcedureExecutor` 会从持久化 Procedure 中读取 nonce 并重建 `nonceKeysToProcIdsMap`。因此同一个请求即使跨 Master 重启重试，也能映射回原始 procId。

### 8.1 资源锁保护

Procedure 锁不是通用并发锁，而是对 HBase 实体的访问约束，例如 namespace、table、region、server。

| 特性 | 说明 |
| --- | --- |
| 资源粒度 | Master 侧常见资源包括 namespace、table、region、server、peer、global、meta |
| 锁生命周期 | 可只覆盖一次 `execute` 调用，也可通过 `holdLock` 覆盖整个 Procedure 生命周期 |
| 层级关系 | region 锁可能伴随 table / namespace 的读锁，防止上层实体被并发修改 |
| 恢复要求 | 锁状态会随 Procedure 持久化，重启恢复时需要重新建立锁语义 |

MasterProcedureScheduler 将 Procedure 按资源类型分队列：`global`、`meta`、`server`、`peer`、`table`。调度优先级是：

```text
+--------+    +------+    +--------+    +------+    +-------+
| global |--->| meta |--->| server |--->| peer |--->| table |
+--------+    +------+    +--------+    +------+    +-------+
```

锁就绪判断遵循：

| 锁需求 | 可运行条件 |
| --- | --- |
| 独占锁 | 当前资源没有任何共享锁或独占锁 |
| 共享锁 | 当前资源没有独占锁 |
| 已持有锁或恢复锁访问权 | 可继续运行 |

这意味着多个用户可以并发操作不同 table 或不同 server，但对同一个 table 的 create/modify/disable/enable 等独占型任务会被串行化。当前分支还为同一 table 的部分 DDL 增加了 `TableProcedureWaitingQueue`，防止多个需要 table exclusive lock 的任务同时进入 table queue。

## 9. 计算与存储资源管理

HBase 的资源管理不是一个集中式 ResourceManager，而是 Master 与 RegionServer 分层协作：

| 层次 | 资源对象 | 管理位置 | 核心机制 |
| --- | --- | --- | --- |
| Master 控制面 | region、table、server、peer、quota 元数据 | HMaster / Procedure / Balancer | assignment、move、balance、quota 观察、Procedure 锁 |
| RegionServer 执行面 | RPC handler、executor 线程、flush/compaction/split 线程、memstore、block cache、WAL | HRegionServer | 本地线程池、队列、内存水位、吞吐控制、quota enforcement |
| 存储数据面 | MemStore、HFile、StoreFile、Region、WAL | RegionServer + HDFS/object storage | write -> memstore -> flush -> HFile -> compaction -> split |

### 9.1 计算资源

| 资源 | 管理类 | 管理方式 | 风险点 |
| --- | --- | --- | --- |
| Procedure worker | `ProcedureExecutor` | core worker、max worker、timeout executor、worker monitor | worker 过少导致任务排队，过多导致 store/锁竞争 |
| HBase executor | `ExecutorService`、`ExecutorType` | 按事件类型启动独立线程池，如 open/close region、snapshot、flush operation | 队列隔离不足或线程数配置不合理会造成局部拥塞 |
| RPC 处理资源 | `RpcScheduler`、`SimpleRpcScheduler` | handler 线程、call queue、priority/replication/meta 队列隔离 | 普通请求、meta 请求、复制请求相互影响时会放大尾延迟 |
| Region open/close | `HRegionServer` executor 启动逻辑 | `RS_OPEN_REGION`、`RS_CLOSE_REGION`、`RS_OPEN_META` 等线程池 | region 迁移或恢复时线程池成为恢复速度上限 |
| Flush 线程 | `MemStoreFlusher` | flush queue、regions-in-queue、flush handlers、memstore 水位触发 | flush 不及时会阻塞写入，flush 过多会制造小文件 |
| Compaction/split 线程 | `CompactSplit` | large/small compaction 线程池、split 线程池、吞吐控制器 | compaction 积压影响读放大，过度 compaction 争抢 IO |
| Chore 后台任务 | `ChoreService` | 周期性执行 balancer、quota refresh、cleaner 等 | 周期任务同时触发可能造成控制面抖动 |

### 9.2 存储资源

| 资源 | 管理类 | 管理方式 | 风险点 |
| --- | --- | --- | --- |
| Region 分布 | `AssignmentManager`、`RegionStates` | Master 维护 region 状态和位置，Procedure 驱动 assign/unassign/move | 单 server region 过多或热点 region 集中导致负载倾斜 |
| 负载均衡 | `LoadBalancer`、`StochasticLoadBalancer` | 基于 region load、table load、locality、memstore size、storefile size 等成本函数生成迁移计划 | 成本函数滞后或指标不全时，balance 可能无法消除热点 |
| 资源池隔离 | `RSGroupBasedLoadBalancer` | 将 table/region 放置约束到 RegionServer group | group 容量不足时会限制调度空间 |
| MemStore 内存 | `RegionServerAccounting`、`MemStoreFlusher` | 全局 memstore 高/低水位，超过压力后选择大 region flush | 内存压力会转化为 flush 压力和写入阻塞 |
| Block cache / heap | `HeapMemoryManager`、`CacheConfig`、`BlockCacheFactory` | 动态或配置化管理 memstore 与 block cache 占比 | cache 过小增加读 IO，memstore 过小增加 flush 频率 |
| StoreFile 数量 | `CompactionPolicy`、`CompactSplit` | 通过 compaction 合并 HFile，降低读放大 | compaction 积压导致读放大和磁盘空间放大 |
| 空间 quota | `RegionServerSpaceQuotaManager`、`QuotaObserverChore` | Master 观察空间使用，RegionServer 刷新并执行 violation policy | 统计滞后导致策略生效有延迟 |
| RPC / 请求 quota | `RegionServerRpcQuotaManager`、`QuotaCache`、`OperationQuota` | RegionServer 对 put/get/scan 检查 user/table quota | quota cache 刷新滞后时，短时间内可能偏离目标 |

### 9.3 资源管理闭环

HBase 的资源管理闭环可以抽象为：

```text
+-------------------------+
| observe metrics/state   |
+-----------+-------------+
            |
            v
+-------------------------+
| Master / RS decision    |
+-----------+-------------+
            |
            v
+-------------------------+
| Procedure/task/quota    |
+-----------+-------------+
            |
            v
+-------------------------+
| RegionServer executes   |
+-----------+-------------+
            |
            v
+-------------------------+
| metrics feed decisions  |
+-------------------------+
```

这个闭环的特点是“控制面最终一致 + 执行面本地自治”：

| 特点 | HBase 做法 | 启示 |
| --- | --- | --- |
| 控制面不直接执行所有动作 | Master 生成 assignment/balance/quota 决策，RegionServer 执行 open/close/flush/compaction | 控制面应下发意图，执行面根据本地资源状态落地 |
| 本地资源需要快速保护 | RegionServer 本地 memstore 水位、flush queue、compaction queue 直接保护写入和读放大 | 资源保护不能完全依赖远端 coordinator |
| 决策必须可恢复 | 关键状态变更走 ProcedureStore | 跨节点资源调度需要持久化意图、状态和结果 |
| 指标有滞后 | balancer 和 quota 都依赖周期性统计 | 资源调度需要接受滞后，并设计限速、回滚和再平衡 |

## 10. 机器异常下线的调度流程

本章节以“集群中一台 RegionServer 异常下线”为例，串联前述 Procedure、资源锁、持久化、客户端重试等机制，说明 HBase 如何在节点故障时保证已写入数据不丢、读写最终可用。这是任务管理框架在真实故障场景下的端到端体现。

### 10.1 不丢数据的核心前提

| 前提 | 机制 | 作用 |
| --- | --- | --- |
| 写入先落 WAL | 写请求先 append 到 RegionServer 的 WAL，再写 memstore | 即使 memstore 未 flush 就崩溃，数据仍保存在 WAL |
| WAL 在共享存储 | WAL 写在 HDFS / object storage，不依赖崩溃节点本地盘 | 节点彻底下线后，WAL 仍可被其它节点读取和切分 |
| 已 flush 数据已落 HFile | flush 把 memstore 持久化为 HFile | 已 flush 的数据不依赖 WAL 恢复 |
| seqId 单调 | region 记录已持久化最大 seqId | WAL 回放时跳过已 flush 的 edit，避免重复或回退 |

只要写入返回成功（WAL 已持久化），即使紧接着 RegionServer 崩溃，数据也能通过 WAL 切分和回放恢复。

### 10.2 故障检测

| 步骤 | 关键类/方法 | 说明 |
| --- | --- | --- |
| RS 注册临时节点 | ZooKeeper `/hbase/rs` 下的 ephemeral znode | RS 进程崩溃或 ZK session 超时后，znode 自动删除 |
| Master 感知节点消失 | `RegionServerTracker.nodeChildrenChanged` -> `refresh` -> `processAsActiveMaster` | 只有 active master 处理，计算消失的 server 集合 |
| 触发下线处理 | `ServerManager.expireServer` | 去重后将 server 从 online 移到 dead，提交故障恢复 |
| 提交恢复任务 | `AssignmentManager.submitServerCrash` | 将 `ServerState` 置为 `CRASHED` 做 fencing，提交 `ServerCrashProcedure` |

核心操作点：`submitServerCrash` 会先把 server 状态标记为 `CRASHED`，此后不再接受该 RS 的 `reportRegionStateTransition`，保证 SCP 拿到的 region 列表不再变化。

### 10.3 ServerCrashProcedure 状态机

机器下线的恢复是一个不可回滚、只能向前重试的 `StateMachineProcedure`（`ServerCrashProcedure`，状态枚举 `ServerCrashState`）。其 `rollbackState` 直接抛 `UnsupportedOperationException`，因为故障恢复没有“撤销”语义，只能持续推进直到完成。

```text
+--------------------+
| SERVER_CRASH_START |
+---------+----------+
          |
          +-- carrying meta -->+-----------------------------+
          |                    | SPLIT_META_LOGS             |
          |                    +-------------+---------------+
          |                                  |
          |                                  v
          |                    +-----------------------------+
          |                    | DELETE_SPLIT_META_WALS_DIR  |
          |                    +-------------+---------------+
          |                                  |
          |                                  v
          |                    +-----------------------------+
          |                    | ASSIGN_META                 |
          |                    +-------------+---------------+
          |                                  |
          +-- no meta ----------------------+
          |
          v
+--------------------+
| GET_REGIONS        |
+---------+----------+
          |
          +-- split wal -------->+--------------------+
          |                      | SPLIT_LOGS         |
          |                      +---------+----------+
          |                                |
          |                                v
          |                      +--------------------+
          |                      | DELETE_SPLIT_WALS  |
          |                      +---------+----------+
          |                                |
          +-- no split wal ----------------+
          |
          v
+--------------------+
| ASSIGN             |
+---------+----------+
          |
          +-- has peer --------->+--------------------+
          |                      | CLAIM_REPL_QUEUES  |
          |                      +---------+----------+
          |                                |
          +-- no peer ---------------------+
          |
          v
+--------------------+
| FINISH             |
| removeServer       |
+--------------------+
```

| 状态 | 核心操作点 |
| --- | --- |
| `SERVER_CRASH_START` | 判断是否承载 meta，承载则优先进入 meta 分支 |
| `SERVER_CRASH_SPLIT_META_LOGS` | 切分 meta 的 WAL |
| `SERVER_CRASH_DELETE_SPLIT_META_WALS_DIR` | 确认 meta WAL 切分完成，否则回退重试 |
| `SERVER_CRASH_ASSIGN_META` | 重新分配 meta region |
| `SERVER_CRASH_GET_REGIONS` | 获取崩溃 server 上的全部 region 并标记 crashed |
| `SERVER_CRASH_SPLIT_LOGS` | 切分用户 region 的 WAL，生成 `recovered.edits` |
| `SERVER_CRASH_DELETE_SPLIT_WALS_DIR` | 确认切分完成并清理切分目录 |
| `SERVER_CRASH_ASSIGN` | 为每个用户 region 提交 `TransitRegionStateProcedure` 重分配 |
| `SERVER_CRASH_CLAIM_REPLICATION_QUEUES` | 接管崩溃节点的复制队列 |
| `SERVER_CRASH_FINISH` | 从 `regionStates` 移除 server，结束 |

### 10.4 meta 优先处理

meta 必须先于用户 region 恢复，因为用户 region 重分配需要写 meta。

| 保障点 | 机制 |
| --- | --- |
| meta 优先切分和分配 | `SERVER_CRASH_START` 检测到 carrying meta 时先走 meta 分支 |
| 用户 region 等待 meta 就绪 | 非 meta 状态执行前调用 `waitMetaLoaded`，未就绪则 `ProcedureSuspendedException` 挂起 |
| 避免重复处理 meta | `SERVER_CRASH_ASSIGN` 中 `filterDefaultMetaRegions` 把 meta 从用户 region 列表剔除 |

### 10.5 WAL 切分与回放保证不丢

这是“数据不丢”的关键链路。崩溃节点 memstore 中未 flush 的数据全部记录在 WAL 中。

```text
+-------------------------+
| crashed RS WAL          |
| shared storage          |
+-----------+-------------+
            |
            v
+-------------------------+
| SplitWALManager         |
| splitWALs               |
+-----------+-------------+
            |
            | per-file SplitWALProcedure
            v
+-------------------------+
| live RS split worker    |
+-----------+-------------+
            |
            v
+-------------------------+
| WALSplitter             |
| group edits by region   |
+-----------+-------------+
            |
            +-- LastSequenceId skips flushed edits
            |
            v
+-------------------------+
| recovered.edits         |
| per region              |
+-----------+-------------+
            |
            v
+-------------------------+
| new RS opens region     |
+-----------+-------------+
            |
            v
+-------------------------+
| replay recovered edits  |
+-----------+-------------+
            |
            v
+-------------------------+
| writes recovered        |
+-------------------------+
```

核心操作点：

- WAL 切分必须在 region 重新分配之前完成。`SERVER_CRASH_SPLIT_LOGS` 一定排在 `SERVER_CRASH_ASSIGN` 之前。
- 切分时用 region 已持久化的最大 seqId（`LastSequenceId`）跳过已 flush 的 edit，避免重复回放或数据回退。
- 新 RS 打开 region 时强制回放 `recovered.edits`，回放完成后才对外提供服务，因此恢复期间不会读到不完整数据。

### 10.6 Region 重新分配

| 步骤 | 关键类/方法 | 说明 |
| --- | --- | --- |
| 逐个处理 region | `ServerCrashProcedure.assignRegions` | 持有 `RegionStateNode` 锁，避免并发冲突 |
| 已在 RIT 的 region | `regionNode.getProcedure().serverCrashed(...)` | 让正在运行的 region procedure 在新 server 重试 |
| 普通 region | `TransitRegionStateProcedure.assign(...)` | 作为子 Procedure 提交，选目标 RS 并 open |
| 防止重复分配 | `isMatchingRegionLocation` | region 已被其它 TRSP 移走时不再重复分配 |

`TransitRegionStateProcedure` 自身也是状态机，经历 `GET_ASSIGN_CANDIDATE` -> `OPEN` -> `CONFIRM_OPENED` 完成在新 RS 上的打开。默认 `hbase.master.scp.retain.assignment=false`，即不强制保留原位置，故障转移更快。

### 10.7 客户端读写不丢

服务端恢复期间，客户端的写入和查询通过异常识别 + 缓存更新 + 重试保证最终成功。

```text
+-------------------------+
| client RPC              |
| old RS location         |
+-----------+-------------+
            |
            v
+-------------------------+
| server exception        |
| moved / not serving     |
+-----------+-------------+
            |
            v
+-------------------------+
| update cache on error   |
+-----------+-------------+
            |
            +-- RegionMovedException updates new ServerName + seqNum
            |
            +-- NotServingRegionException removes cache and re-queries meta
            |
            v
+-------------------------+
| retrying caller         |
| backoff and retry       |
+-----------+-------------+
            |
            v
+-------------------------+
| request reaches new RS  |
| after region opened     |
+-------------------------+
```

| 客户端机制 | 作用 |
| --- | --- |
| `RegionMovedException` 携带新位置 | 客户端可直接把缓存更新到新 RS，无需重查 meta |
| `NotServingRegionException` | region 关闭/迁移中，客户端清缓存并稍后重查 meta |
| `canUpdateOnError` 用 seqNum 比较 | 只在缓存不比错误信息新时更新，避免回退到旧位置 |
| `clearCache(ServerName)` | 整台 server 不可用时，清掉该 server 上所有 region 缓存 |
| 退避重试 | 重试调用者在 region 恢复期间退避等待，直到 open 完成 |

核心结论：只要写入已返回成功（WAL 已持久化），节点崩溃后数据通过 WAL 切分和回放恢复；客户端在恢复窗口内收到的是“可重试异常”，重试后请求被路由到新 RS，因此读写不会丢失，只会在恢复窗口内出现短暂延迟。

### 10.8 端到端核心操作点小结

| 阶段 | 核心操作点 | 不丢保证 |
| --- | --- | --- |
| 检测 | ZK ephemeral znode + `expireServer` + `CRASHED` fencing | 停止接受崩溃节点的状态汇报，冻结待恢复 region 列表 |
| 恢复编排 | `ServerCrashProcedure`（持久化、不可回滚、只向前重试） | Master failover 后仍能从持久化状态继续恢复 |
| 数据恢复 | WAL split + `recovered.edits` + 回放，且切分先于分配 | 未 flush 写入被完整回放 |
| 服务恢复 | meta 优先 + `TransitRegionStateProcedure` 重分配 | region 在新 RS 重新对外提供服务 |
| 客户端 | 移动/不可用异常 + 缓存更新/失效 + 退避重试 | 恢复窗口内请求可重试，最终路由到新 RS |

## 11. Pulse 演进建议

`/Users/bytedance/Documents/01_Projects/pulse/docs/design/distributed-heartbeat-management.md` 中的 Pulse 已经具备资源管理的雏形：

| Pulse 能力 | 当前设计 | 对应资源管理意义 |
| --- | --- | --- |
| 状态上报 | `state.heartbeat`、`state.metrics`、`state.capability` | 建立 agent 资源视图 |
| 指令下发 | `cmd.*` 通过 `/heartbeat` 响应返回 | coordinator 可以下发资源调整动作 |
| 回复账本 | `reply.*`、`MessageLedger` | 跟踪资源动作是否 accepted/running/ok/failed |
| 聚合层 | `Pulse Group` 聚合 `agents[]` | 降低 coordinator 心跳压力，形成区域级采样入口 |
| 控制消息 | `control.redirect`、`control.throttle` | 可做心跳路径切换、上报频率控制和轻量限流 |
| 最终一致 | `epoch + seq` 合并状态 | 适合资源状态传播，但不适合强一致资源分配 |

### 11.1 问题总结

| 问题 | 当前 Pulse 风险 | HBase 对照 |
| --- | --- | --- |
| 资源模型还偏状态采样 | `state.metrics` 可以上报 load/memory，但缺少统一的 CPU、内存、磁盘、端口、进程、容量、租约模型 | HBase 明确区分 region、server、memstore、storefile、quota、线程池 |
| 缺少资源分配语义 | 目前有 `cmd.*`，但没有 `Allocation`、`Lease`、`DesiredState` 这类资源意图对象 | HBase 关键资源变更通过 Procedure 持久化意图和状态 |
| 控制动作缺少持久化恢复边界 | `MessageLedger` 记录消息状态，但资源动作的目标状态、当前状态、回滚策略还不完整 | ProcedureStore 保存 class、state、owner、nonce、result、业务状态 |
| 本地保护能力不足 | agent 可执行命令，但尚未定义本地 admission control、资源水位、拒绝策略 | RegionServer 本地 memstore/flush/compaction/quota 可立即保护自身 |
| 调度决策缺少成本函数 | coordinator 能看到状态，但缺少类似 balancer 的 placement/cost model | StochasticLoadBalancer 用 region load、locality、memstore、storefile 等成本函数 |
| 最终一致与互斥关系未分层 | `epoch + seq` 适合状态覆盖，但资源独占、迁移、配额变更需要 fencing 或租约 | HBase 用 Procedure 锁、nonce、owner、quota 和 region state 组合保护 |
| Group 不是状态权威 | group 聚合状态但不裁决，如果未来参与资源调度，需要明确边界 | HBase 的 RegionServer 执行本地保护，但 Master 是 assignment/quota 控制面 |

### 11.2 演进建议

| 阶段 | 建议 | 产出 |
| --- | --- | --- |
| 1. 资源画像标准化 | 扩展 `state.resource`，统一上报 CPU、memory、disk、network、process、port、version、capacity、labels | Coordinator 形成 `NodeResourceState` |
| 2. 区分观测状态和期望状态 | 增加 `desired_state` / `allocation` / `lease` 模型，不把命令消息本身当作资源状态 | 可以解释“系统希望怎样”和“agent 当前怎样”的差异 |
| 3. 引入资源动作账本 | 将 `MessageLedger` 扩展为 `ActionLedger`，记录 action id、resource id、target state、owner、deadline、nonce、status、result | 支持失败重试、幂等、审计和恢复 |
| 4. 下沉本地资源保护 | agent 支持本地水位和 admission control，例如进程数、CPU、内存、磁盘、端口占用超限时拒绝或降级执行 | 避免 coordinator 滞后导致单机过载 |
| 5. 建立调度成本函数 | coordinator 根据负载、容量、zone、labels、历史成功率、心跳新鲜度计算 placement score | 支持资源选择、迁移、扩容、缩容 |
| 6. 引入租约和 fencing | 对独占资源分配使用 `lease_id`、`epoch`、`ttl`、`owner`，agent 执行动作时校验 lease | 防止重复指令、旧 coordinator、旧 agent 进程误操作 |
| 7. 将 group 定位为区域聚合器 | group 继续不做最终裁决，但可做采样压缩、局部预警、上报节流和转发缓存 | 保持安全边界清晰，避免多权威冲突 |
| 8. 资源动作 Procedure 化 | 对跨节点、可失败、需回滚的动作引入类似 Procedure 的状态机：prepare、dispatch、wait、verify、commit/rollback | 提高复杂资源调度的可恢复性 |

### 11.3 建议的资源管理抽象

可以在 Pulse 中逐步引入下面的对象：

| 对象 | 作用 | 是否持久化 |
| --- | --- | --- |
| `NodeResourceState` | agent 上报的实时资源视图，包括容量、使用量、标签、进程、心跳新鲜度 | 是，保留最新版本和短历史 |
| `ResourceIntent` | coordinator 计算出的期望状态，如将某组件放到某 agent、调整心跳频率、迁移工作负载 | 是 |
| `ResourceLease` | 对独占或有副作用资源的临时占用凭证 | 是 |
| `ResourceAction` | 一次具体执行动作，如 start/stop/reload/migrate/throttle | 是 |
| `ActionResult` | agent 对 action 的 accepted/running/ok/failed 回复 | 是 |
| `LocalGuardrail` | agent 本地水位和拒绝策略 | 是，作为配置；实时计数可临时 |

推荐的控制闭环：

```text
+-------------------------+
| agent reports           |
| NodeResourceState       |
+-----------+-------------+
            |
            v
+-------------------------+
| coordinator computes    |
| ResourceIntent          |
+-----------+-------------+
            |
            v
+-------------------------+
| create lease/action     |
+-----------+-------------+
            |
            | heartbeat response
            v
+-------------------------+
| agent checks lease      |
| guardrail + idempotency |
+-----------+-------------+
            |
            v
+-------------------------+
| agent executes          |
| replies ActionResult    |
+-----------+-------------+
            |
            v
+-------------------------+
| verify convergence      |
| roll forward/back       |
+-------------------------+
```

### 11.4 与 HBase 的关键差异

| 维度 | HBase | Pulse 建议 |
| --- | --- | --- |
| 状态权威 | Master 负责 region assignment/quota 控制面，RegionServer 负责本地执行面 | Coordinator 负责全局意图，agent 负责本地保护，group 只聚合 |
| 恢复模型 | ProcedureStore 持久化关键任务状态 | ActionLedger + ResourceIntent + Lease 持久化 |
| 资源互斥 | Procedure 锁和 region/table/server 状态 | lease/fencing + resource id 互斥 |
| 本地限流 | memstore 水位、flush/compaction、RPC quota | local guardrail + control.throttle + admission control |
| 调度模型 | balancer 成本函数和 assignment procedure | placement score + action state machine |

## 12. 性能瓶颈评估

### 12.1 主要热点

| 热点 | 位置 | 原因 | 影响 |
| --- | --- | --- | --- |
| ProcedureStore 写入 | `execProcedure` 每步执行后 `updateStoreOnExec` | 每个关键步骤都要持久化，append/store update 可能耗时 | 高并发 Procedure 下吞吐受 store latency 影响 |
| Master 重启加载 | `store.recoverLease()` + `store.load()` | 需要恢复 lease、扫描持久化 Procedure、重建内存索引和队列 | 未清理或长期未更新的老 Procedure 会拖慢 Master restart |
| 同 root rollback stack 同步 | `procStack.addRollbackStep` | 同一 root 下子 Procedure 共享 root state | 子 Procedure 持久化可能被串行化，代码已尝试把部分 store update 移到锁外 |
| 资源锁冲突 | `MasterProcedureScheduler` | 同一 table/region/server 的独占锁会串行化 | 热点表 DDL、批量 region 操作可能排队 |
| Worker 数配置 | `ProcedureExecutor.init(numThreads)` | core worker 固定，max worker 为 `10 * numThreads` | worker 过少会排队，过多会增加锁和 store 竞争 |
| Timeout / monitor 线程 | `TimeoutExecutorThread` | 超时、周期 chore、worker monitor 集中处理 | 大量 timeout/waiting Procedure 会增加调度压力 |
| TaskMonitor 锁 | `TaskMonitor` synchronized 方法 | 创建、清理、查询任务列表需要同步 | 监控任务特别多时影响诊断查询，不是主执行瓶颈 |
| ExecutorService 队列 | 每个 executor 的线程池和 queue | 事件处理速度低于提交速度时积压 | 非 Procedure 异步事件延迟 |

### 12.2 当前优化

| 优化 | 说明 |
| --- | --- |
| `yield` / `isYieldAfterExecutionStep` | 允许长 Procedure 在步骤间让出 worker，提升公平性 |
| `isYieldBeforeExecuteFromState` | 业务状态机可在指定状态前主动让出执行机会 |
| `skipPersistence()` | 某些非关键重试信息可跳过一次持久化，减少 store 压力 |
| `forceUpdate` | ProcedureStore 可提示上层刷新老 Procedure，帮助删除旧 WAL，改善重启加载 |
| `FairQueue` | Master 调度器按资源队列公平选择，避免单一资源长期霸占 worker |
| `TableProcedureWaitingQueue` | 对同一 table 的独占 DDL 做入队前等待，减少队列内冲突和无效轮询 |
| store update 锁外执行 | 同 root 子 Procedure 在不需要维护 rollback step 时，可在 root state 锁外持久化 |

### 12.3 性能风险判断

| 场景 | 性能风险 | 建议观察指标 |
| --- | --- | --- |
| 大量 table DDL | table exclusive lock 串行化，等待队列增长 | Procedure 队列长度、table lock waiting procedures |
| 大量 region assign/unassign | region/server 锁冲突，远端 RPC 等待 | `WAITING_TIMEOUT` 数量、AssignmentManager 日志 |
| Master 重启慢 | ProcedureStore 中历史 Procedure 或旧 WAL 过多 | store load 耗时、forceUpdate 日志、procedure store 文件数 |
| Procedure 步骤过粗 | 单个 worker 长时间被占用，公平性下降 | 单 Procedure elapsed time、worker thread dump |
| Procedure 步骤过细 | 持久化次数过多，store 成为瓶颈 | store update latency、ProcedureStore sync 耗时 |
| TaskMonitor 数据过多 | synchronized 查询和清理成本升高 | `hbase.taskmonitor.max.tasks`、dump servlet 响应时间 |

## 13. 关键取舍

| 取舍 | 原因 | 代价 |
| --- | --- | --- |
| 用 Procedure 管理关键状态变更 | 需要 Master failover 后继续执行 | Procedure 实现必须严格幂等，状态序列化复杂 |
| 保留 ExecutorService 处理轻量异步事件 | 简单、低成本、适合非持久化内部事件 | 不保证崩溃恢复，不适合关键状态变更 |
| TaskMonitor 只做观测不做调度 | 降低与执行框架耦合，适合跨组件诊断 | 查询到的任务不代表所有 Procedure 或 executor 队列 |
| Procedure 锁绑定 HBase 实体 | 避免 table、region、server 状态被并发破坏 | 锁层级和恢复语义增加实现复杂度 |
| 每个关键步骤持久化 | 任意时刻 Master 崩溃都可恢复 | ProcedureStore 写入成为核心性能路径 |
| owner/nonce 随 Procedure 持久化 | 跨重启保留用户归属和幂等提交 | 提交流程必须正确传递 request user 和 nonce |
| Master 控制面和 RegionServer 本地自治结合 | 全局放置与本地快速保护分离 | 指标滞后和策略冲突需要额外协调 |

## 14. 排查入口

| 问题 | 优先入口 |
| --- | --- |
| Master 操作卡住、建表删表不结束、region assign 异常 | `ProcedureExecutor`、`MasterProcedureScheduler`、具体 `master/procedure` 类 |
| Master failover 后任务是否能恢复 | `ProcedureStore`、Procedure 的 `serializeStateData` / `deserializeStateData` |
| 内部异步事件队列堆积 | `ExecutorService.getAllExecutorStatuses()`、`ExecutorStatus`、`EventType` |
| RPC handler 或长耗时任务卡住 | `TaskMonitor.getTasks(filter)`、Master / RegionServer dump servlet |
| Shell `taskmonitor` 看不到某个操作 | 确认该操作是否注册了 `MonitoredTask`，以及查询的是 RegionServer Web JSON 而不是 Procedure 列表 |

## 15. 风险

- Procedure 与 EventHandler 在历史代码中可能共存，同一业务路径需要确认实际入口。
- Procedure 的执行状态与业务状态机状态不同，排查日志时不能混用。
- TaskMonitor 只记录主动注册的任务，不是全量任务目录。
- Procedure 恢复语义依赖具体实现是否正确持久化业务状态，框架不能自动弥补遗漏字段。
- owner 追踪不是权限控制，真正的权限保护依赖 RPC/Master coprocessor hook 在提交前完成校验。
- nonce 只能防止同一 nonce key 的重复提交，不能替代资源锁和权限校验。

## 16. 验证

本梳理依据以下可复查来源：

| 证据 | 路径 |
| --- | --- |
| PV2 官方说明 | `hbase-website/app/pages/_docs/docs/_mdx/(multi-page)/pv2.mdx` |
| Procedure 基类注释与执行契约 | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/Procedure.java` |
| ProcedureExecutor 类注释与常量 | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/ProcedureExecutor.java` |
| ProcedureScheduler 队列接口 | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/ProcedureScheduler.java` |
| Procedure proto 持久化字段 | `hbase-protocol-shaded/src/main/protobuf/server/Procedure.proto` |
| StateMachineProcedure 业务状态栈 | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/StateMachineProcedure.java` |
| ProcedureStore 持久化与 forceUpdate 语义 | `hbase-procedure/src/main/java/org/apache/hadoop/hbase/procedure2/store/ProcedureStore.java` |
| MasterProcedureEnv 集成点 | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/procedure/MasterProcedureEnv.java` |
| MasterProcedureScheduler 锁与调度 | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/procedure/MasterProcedureScheduler.java` |
| SchemaLocking 资源锁集合 | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/procedure/SchemaLocking.java` |
| MasterProcedureUtil nonce 提交流程 | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/procedure/MasterProcedureUtil.java` |
| AccessController DDL 权限 hook | `hbase-server/src/main/java/org/apache/hadoop/hbase/security/access/AccessController.java` |
| RegionProcedureStore 保存格式 | `hbase-server/src/main/java/org/apache/hadoop/hbase/procedure2/store/region/RegionProcedureStore.java` |
| ExecutorService 类注释与提交逻辑 | `hbase-server/src/main/java/org/apache/hadoop/hbase/executor/ExecutorService.java` |
| EventHandler 执行契约 | `hbase-server/src/main/java/org/apache/hadoop/hbase/executor/EventHandler.java` |
| TaskMonitor 任务创建、过滤和清理逻辑 | `hbase-server/src/main/java/org/apache/hadoop/hbase/monitoring/TaskMonitor.java` |
| RegionServer executor 启动逻辑 | `hbase-server/src/main/java/org/apache/hadoop/hbase/regionserver/HRegionServer.java` |
| MemStore flush 队列和水位 | `hbase-server/src/main/java/org/apache/hadoop/hbase/regionserver/MemStoreFlusher.java` |
| Compaction/split 线程池 | `hbase-server/src/main/java/org/apache/hadoop/hbase/regionserver/CompactSplit.java` |
| StochasticLoadBalancer 成本函数 | `hbase-balancer/src/main/java/org/apache/hadoop/hbase/master/balancer/StochasticLoadBalancer.java` |
| RegionServer RPC quota | `hbase-server/src/main/java/org/apache/hadoop/hbase/quotas/RegionServerRpcQuotaManager.java` |
| RegionServer 下线检测 | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/RegionServerTracker.java` |
| 下线处理入口 | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/ServerManager.java` |
| ServerCrashProcedure 状态机 | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/procedure/ServerCrashProcedure.java` |
| WAL 切分编排 | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/SplitWALManager.java` |
| WAL 切分与 seqId 跳过 | `hbase-server/src/main/java/org/apache/hadoop/hbase/wal/WALSplitter.java` |
| recovered.edits 回放 | `hbase-server/src/main/java/org/apache/hadoop/hbase/regionserver/HRegion.java` |
| Region 状态迁移 | `hbase-server/src/main/java/org/apache/hadoop/hbase/master/assignment/TransitRegionStateProcedure.java` |
| 客户端位置缓存更新 | `hbase-client/src/main/java/org/apache/hadoop/hbase/client/AsyncRegionLocatorHelper.java` |
| RegionMovedException | `hbase-client/src/main/java/org/apache/hadoop/hbase/exceptions/RegionMovedException.java` |
| Pulse 心跳平台设计 | `/Users/bytedance/Documents/01_Projects/pulse/docs/design/distributed-heartbeat-management.md` |

## 17. 待确认

- RegionServer 侧是否还有不经过上述三类机制的长期后台任务，需要按具体问题继续检索。
- 性能瓶颈评估基于静态代码路径，尚未结合线上指标或压测数据量化。
