# Spark 任务管理框架设计梳理

> 本文梳理 Apache Spark Core 的任务管理（调度）框架，覆盖核心组件、状态机、
> 持久化与临时数据划分、多用户并发保护、以及性能瓶颈评估。
> 所有结论均带源码路径与行号，便于后续 agent 直接定位核查。
> 代码基线：本仓库当前 `master`（core 模块）。

## 1. 结论

| 结论 | 说明 |
|---|---|
| Spark 任务管理的设计核心是四层状态机 | Job -> Stage -> TaskSet -> Task，每层都有明确状态收敛点，避免状态散落 |
| 调度运行态大多是临时数据 | driver 内存保存主要调度状态；EventLog、可选 UI store、RDD checkpoint 才能跨进程保留 |
| 数据不丢依赖血缘重算与提交仲裁 | 机器异常下线后，FetchFailed 触发重算；OutputCommitCoordinator 保证同一 partition 只提交一次 |
| 性能瓶颈集中在 driver 单点 | DAGScheduler 事件循环、TaskSchedulerImpl 全局锁、RPC/ListenerBus 都是规模上限 |
| Pulse 可沿“轻中心 + 边缘自管理”演进 | 中心保留索引、配额、提交授权；task manager 管理本地 DAG 与资源闭环 |

## 2. 背景与范围

### 2.1 背景

本文梳理 Apache Spark Core 的任务管理（调度）框架，覆盖核心组件、状态机、持久化与临时数据划分、多用户并发保护、资源管理、故障恢复和演进取舍。所有关键结论尽量带源码路径，便于后续 agent 直接定位核查。

### 2.2 目标

- 讲清 Job -> Stage -> TaskSet -> Task 的分层调度与端到端数据流。
- 补全四层状态机（Job / Stage / TaskSet / Task），这是框架的设计精髓。
- 区分任务管理过程中的持久化数据与临时数据。
- 说明多用户 / 多并发作业的隔离与保护措施。
- 评估框架的性能瓶颈及现有缓解手段。
- 结合 Pulse 心跳平台，整理资源管理问题总结与演进建议。

### 2.3 非目标

- 不覆盖 RDD/算子语义、SQL/Catalyst、Shuffle 读写实现细节。
- 不涉及 Structured Streaming 的微批调度。
- 不深入 YARN/K8s/Standalone 的资源申请实现，只分析到 Spark `SchedulerBackend` 抽象层。

## 3. 当前问题

| 问题 | 当前表现 | 本文对应章节 |
|---|---|---|
| 章节混杂 | 架构、状态机、持久化、资源、演进建议交织，后续查阅成本高 | 第 4 章 |
| 持久化规则不易查 | 临时/半持久/持久数据分散描述 | 第 7.1 节 |
| 资源管理缺少整体视角 | 计算资源和存储资源原本分散在不同组件中 | 第 8 章 |
| 故障流程不够端到端 | 机器下线后的检测、清理、重算、提交仲裁需要串起来 | 第 9 章 |
| 去中心化 DAG 设计缺少取舍 | “中心只做索引”能降压，但会引入分布式一致性问题 | 第 11 章 |
| Pulse 演进方向需落地 | 当前心跳平台已有状态采集，但还缺资源画像、调度闭环和强一致仲裁 | 第 12 章 |
| Pulse 作为任务管理基座存在语义缺口 | 心跳协议只解决状态可见性，尚未定义 DAG、task ownership、commit、recovery 等任务管理核心语义 | 第 12.3 节 |

## 4. 文档结构

```text
+-----------------------------+
| 1. 结论                     |
+-------------+---------------+
              |
              v
+-----------------------------+
| 2-3. 背景、范围、当前问题   |
+-------------+---------------+
              |
              v
+-----------------------------+
| 5-9. Spark 调度方案与流程   |
| 架构 / 状态机 / 数据 / 资源 |
| / 机器异常下线              |
+-------------+---------------+
              |
              v
+-----------------------------+
| 10-12. 评估与演进取舍       |
| 性能瓶颈 / 去中心化 DAG     |
| / Pulse 资源管理            |
+-------------+---------------+
              |
              v
+-----------------------------+
| 13-14. 风险与验证           |
+-----------------------------+
```

> 章节编排遵循 `doc.md`：先给结论，再给背景和当前问题；方案细节按主题分组；流程使用 ASCII graph；对比、风险和决策优先使用表格。

---

## 5. 调度架构与分层

Spark 调度是一个自上而下逐级拆解的三层架构：把"作业(Job)"按 shuffle 边界拆成
"阶段(Stage)"的 DAG，再把每个 Stage 转成可在 Executor 上运行的"任务(Task)"集合(TaskSet)。

```text
  用户 action (count/collect/save ...)
            |
            v
  SparkContext.runJob
            |
            v
+-------------------------------------------------------------+
|  DAGScheduler                  (高层 / 面向 Stage)          |
|  - Job -> Stage(DAG) 拆分, 处理依赖与容错                   |
|  - 单线程事件循环串行处理所有状态变更                       |
+-------------------------------------------------------------+
            |  taskScheduler.submitTasks(TaskSet)
            v
+-------------------------------------------------------------+
|  TaskSchedulerImpl             (低层 / 面向 Task)           |
|  - 管理 TaskSetManager, 资源分配 resourceOffers            |
|  - rootPool(FIFO/FAIR) 决定优先级, 本地性 + 延迟调度       |
+-------------------------------------------------------------+
            |  backend.reviveOffers()  /  backend 回调 resourceOffers
            v
+-------------------------------------------------------------+
|  CoarseGrainedSchedulerBackend (面向 Executor)             |
|  - 管理 Executor 注册/资源, 序列化并 RPC 下发 Task        |
+-------------------------------------------------------------+
            |  LaunchTask (RPC)
            v
       Executor 执行  --- StatusUpdate (RPC) ---> 逐层上报
```

### 5.1 核心组件职责

| 组件 | 文件 | 职责 |
|---|---|---|
| DAGScheduler | [DAGScheduler.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala) | Job 拆 Stage、依赖管理、fetch 失败容错重试、Stage->TaskSet |
| TaskScheduler / TaskSchedulerImpl | [TaskSchedulerImpl.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSchedulerImpl.scala) | 接收 TaskSet、资源分配、失败重试、推测执行、状态回传 |
| TaskSetManager | [TaskSetManager.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSetManager.scala) | 单个 Stage attempt 内任务的本地性、重试、推测执行 |
| SchedulerBackend / CoarseGrained | [CoarseGrainedSchedulerBackend.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/cluster/CoarseGrainedSchedulerBackend.scala) | 管理 Executor、序列化下发 Task、回收资源 |
| SchedulableBuilder / Pool | [SchedulableBuilder.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/SchedulableBuilder.scala) / [Pool.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/Pool.scala) | FIFO / FAIR 调度池树，决定 TaskSet 优先级 |

### 5.2 端到端数据流

1. `SparkContext.runJob` -> `DAGScheduler.runJob` -> `submitJob`（[DAGScheduler.scala#L984-L1027](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala#L984-L1027)），分配 jobId 后 `eventProcessLoop.post(JobSubmitted)`。
2. 事件循环线程 `doOnReceive` -> `handleJobSubmitted`（[L1400-L1485](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala#L1400-L1485)）。
3. 拆 Stage：`createResultStage` 沿 shuffle 依赖递归创建父 `ShuffleMapStage`（`getOrCreateShuffleMapStage`，[L528-L550](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala#L528-L550)）。
4. 提交 Stage：`submitStage` 递归提交缺失父 Stage，无缺失则 `submitMissingTasks`（[L1635](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala#L1635)）。
5. Stage->TaskSet：`submitMissingTasks` 计算 preferred locations、广播任务二进制、生成 `ShuffleMapTask`/`ResultTask`，调 `taskScheduler.submitTasks(TaskSet)`（[L1821](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala#L1821)）。
6. `TaskSchedulerImpl.submitTasks` 创建 TaskSetManager 入调度池，调 `backend.reviveOffers()`（[L243-L285](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSchedulerImpl.scala#L243-L285)）。
7. Backend `makeOffers` 构建 `WorkerOffer`，回调 `scheduler.resourceOffers`（[CoarseGrainedSchedulerBackend.scala#L372-L385](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/cluster/CoarseGrainedSchedulerBackend.scala#L372-L385)）。
8. `resourceOffers` 经 `rootPool.getSortedTaskSetQueue` 排序取 TaskSet，按本地性级别产出 `TaskDescription`（[TaskSchedulerImpl.scala#L512-L797](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSchedulerImpl.scala#L512-L797)）。
9. `launchTasks` 序列化后 `executorEndpoint.send(LaunchTask)` 发到 Executor（[L426-L456](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/cluster/CoarseGrainedSchedulerBackend.scala#L426-L456)）。
10. Executor 完成后 `StatusUpdate` -> `scheduler.statusUpdate` -> `TaskResultGetter` 异步反序列化 -> `TaskSetManager.handleSuccessfulTask/handleFailedTask` -> `dagScheduler.taskEnded` post `CompletionEvent` -> 事件循环 `handleTaskCompletion`。

---

## 6. 状态机（设计精髓）

任务管理框架的核心是四层嵌套状态机：Task 的状态汇聚成 TaskSet 的进度，
TaskSet 的成败汇聚成 Stage 的状态，Stage 的成败汇聚成 Job 的最终结果。
关键设计是：**每一层都有唯一的状态收敛点**，避免状态被多处分散修改。

| 层级 | 唯一收敛点 |
|---|---|
| Task | `TaskSetManager.handleSuccessfulTask` / `handleFailedTask` |
| Stage | `DAGScheduler.markStageAsFinished` |
| Job | result task 全完成分支（成功）/ `failJobAndIndependentStages`（失败） |

### 6.1 Task 状态机（最底层）

`TaskState` 是底层枚举（[TaskState.scala#L20-L31](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/TaskState.scala#L20-L31)）：

```text
                  +----------- KILLED  (被主动 kill / 被其它 attempt 成功后清理)
                  |
  LAUNCHING ---> RUNNING -----> FINISHED (正常完成)
     |            |
     |            +----------- FAILED  (执行异常)
     |
     +----------------------- LOST    (executor 丢失)

  终态(isFinished): FINISHED, FAILED, KILLED, LOST     (L24/L30)
  失败态(isFailed):                  FAILED, LOST       (L28)
```

`Enumeration` 本身不强制转换，转换语义体现在消费方代码中。Driver 侧用
`TaskInfo` 的可变标志位跟踪单次 attempt 的细粒度状态
（[TaskInfo.scala#L98-L146](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskInfo.scala#L98-L146)）：

| 派生状态 | 计算方式 | 说明 |
|---|---|---|
| running | `!finished` | finishTime==0 |
| gettingResult | `gettingResultTime != 0` | 正在拉取大结果 |
| finished | `finishTime != 0` | `markFinished` 设置 |
| successful | `finished && !failed && !killed` | |
| status(字符串) | RUNNING/GET RESULT/FAILED/KILLED/SUCCESS/UNKNOWN | 对应 REST `TaskStatus` |

### 6.2 TaskSet 状态机（一个 Stage attempt 内的任务集）

TaskSetManager 用一组数组与计数器表达 TaskSet 的聚合进度，没有显式枚举
（[TaskSetManager.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSetManager.scala)）：

| 字段 | 含义 |
|---|---|
| `copiesRunning[]`（L78） | 每个 task 正在运行的副本数（含推测执行） |
| `successful[]`（L125） | 每个 task 是否已有一个副本成功 |
| `numFailures[]`（L126） | 每个 task 累计失败次数 |
| `tasksSuccessful`（L134） | 成功的 task 总数 |
| `isZombie`（L175） | 僵尸态：全部成功或被 abort 后不再启动新任务 |

```text
            resourceOffer (copiesRunning++)
                  |
                  v
  [ pending ] --------> [ running ] --+-- handleSuccessfulTask --> successful[i]=true
      ^                               |        tasksSuccessful++
      |                               |        (全部成功 => isZombie=true)
      | addPendingTask                |
      | (可重试)                      +-- handleFailedTask --+-- countTowardsTaskFailures
      |                               |                       |   numFailures[i]++
      |                               |                       |   >= maxTaskFailures => abort()
      |                               |                       +-- 否则重新入队 pending
      |                               |
      +-------------------------------+-- FetchFailed: successful[i]=true, isZombie=true
                                          (不重试本 task, 交 DAGScheduler 重算上游)

  收尾: maybeFinishTaskSet()  当 isZombie && runningTasks==0 (L606-L616)
        -> sched.taskSetFinished(this)
```

关键转换：
- 成功：`handleSuccessfulTask`（[L812-L948](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSetManager.scala#L812-L948)），并 kill 同一 task 其它在跑 attempt，记入 `killedByOtherAttempt`。
- 失败：`handleFailedTask`（[L951-L1096](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSetManager.scala#L951-L1096)）。`FetchFailed` 不计入失败次数并置 zombie；`ExceptionFailure`（不可序列化结果 / 输出文件已存在）直接 abort 不重试。
- 中止：`abort`（[L1098-L1102](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSetManager.scala#L1098-L1102)）通知 `dagScheduler.taskSetFailed`，置 zombie。
- Executor 丢失：可能回滚 `successful[i]=false; tasksSuccessful--` 后重新入队（[L1192-L1220](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSetManager.scala#L1150-L1226)）。

### 6.3 Stage 状态机

Stage 无显式枚举，状态 = DAGScheduler 持有的"集合归属" + `StageInfo` 时间戳字段。

DAGScheduler 用三个 HashSet 作为状态桶（[DAGScheduler.scala#L164-L172](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala#L164-L172)）：

```text
                getMissingParentStages 有缺失父 Stage
   submitStage -------------------------------------> [ waitingStages ]
        |                                                    |
        | 无缺失父 Stage                                     | 父 Stage 完成
        | submitMissingTasks                                 | submitWaitingChildStages
        v                                                    v
   [ runningStages ] <-------------------------------------- +
        |
        +-- markStageAsFinished(err=None)  成功: completionTime 设置, clearFailures()
        |        -> runningStages -= stage  (L3284)
        |
        +-- markStageAsFinished(err=Some) 失败: stageFailed(reason)
        |
        +-- FetchFailed: failedStages += (failedStage, mapStage)  (L2486-2487)
                 |
                 v
           [ failedStages ] --resubmitFailedStages--> 重新 submitStage (L1245-1252)
```

`StageInfo` 对外状态由 `getStatusString` 计算（[StageInfo.scala#L67-L77](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/StageInfo.scala#L67-L77)）：

| completionTime | failureReason | 状态 |
|---|---|---|
| 未定义 | - | running |
| 已定义 | 已定义 | failed |
| 已定义 | 无 | succeeded |

- 唯一收尾点 `markStageAsFinished`（[L3256-L3285](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala#L3256-L3285)）。
- 失败重试上限由 `Stage.failedAttemptIds` + `maxStageAttempts` 保护（防无限重试，SPARK-5945）。
- 两种子类：`ShuffleMapStage`（输出 shuffle 数据，`isAvailable` 判可用）与 `ResultStage`（执行 action `func`）。
- 对外 REST 枚举 `StageStatus`：ACTIVE / COMPLETE / FAILED / PENDING / SKIPPED。

### 6.4 Job 状态机

Job 也无显式枚举，由 `ActiveJob.finished[]` / `numFinished` 跟踪进度
（[ActiveJob.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/ActiveJob.scala)），
结果用 `JobResult`（`JobSucceeded` / `JobFailed`）表达，
通过 `JobWaiter.jobPromise` 向调用方反馈（[JobWaiter.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/JobWaiter.scala)）。

```text
   handleJobSubmitted: activeJobs += job        (L1472)
            |
            v
       [ RUNNING ]  (numFinished < numPartitions)
            |
            +-- 每个 ResultTask 完成: finished[outputId]=true; numFinished++  (L2289-2290)
            |        当 numFinished == numPartitions (L2292):
            |          markStageAsFinished -> cleanupState -> post JobEnd(JobSucceeded)
            |          -> JobWaiter.taskSucceeded -> jobPromise.success  => [ SUCCEEDED ]
            |
            +-- failJobAndIndependentStages (L3380-3390):
                     cancelRunningIndependentStages -> cleanupState
                     -> JobWaiter.jobFailed -> jobPromise.tryFailure
                     -> post JobEnd(JobFailed)            => [ FAILED ]

   取消: handleJobCancellation / handleJobGroupCancelled -> failJobAndIndependentStages
   清理: cleanupStateForJobAndIndependentStages 末尾 activeJobs -= job  (L961)
```

对外 REST 枚举 `JobExecutionStatus`：RUNNING / SUCCEEDED / FAILED / UNKNOWN。

### 6.5 Executor 状态机（部署层，辅助）

`ExecutorState`（[ExecutorState.scala#L20-L31](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/deploy/ExecutorState.scala#L20-L31)）：
LAUNCHING / RUNNING / KILLED / FAILED / LOST / EXITED / DECOMMISSIONED。
终态为 KILLED/FAILED/LOST/EXITED；`DECOMMISSIONED` 故意不算终态（executor 仍存在但不再接新任务）。

---

## 7. 调度数据与并发保护

这是理解"driver 重启会丢什么"的关键。结论：**任务调度的运行时状态几乎全部是临时（driver 内存）数据，
driver 重启即全部丢失；只有事件日志、（可选的）UI store 磁盘后端、RDD checkpoint 才是持久化的。**

```text
  driver 进程内存 (TRANSIENT, 重启即丢)
  +-----------------------------------------------------------+
  | DAGScheduler:  jobIdToStageIds / stageIdToStage /         |
  |   waitingStages / runningStages / failedStages / activeJobs|
  | TaskSchedulerImpl: taskSetsByStageIdAndAttempt /          |
  |   taskIdToTaskSetManager / rootPool ...                   |
  | MapOutputTrackerMaster: shuffleStatuses (位置元数据)      |
  | LiveListenerBus / AsyncEventQueue: 内存事件队列           |
  | AppStatusStore(默认 InMemoryStore): Live UI 数据          |
  +-----------------------------------------------------------+
            | 事件                          | 监听
            v                               v
  +----------------------+      +----------------------------------+
  | EventLoggingListener |      | AppStatusStore(LevelDB/RocksDB)  |
  | -> eventLog.dir/HDFS |      | 仅当 spark.ui.store.path / SHS   |
  | JSON, PERSISTENT     |      | PERSISTENT(可选)                 |
  +----------------------+      +----------------------------------+

  executor 本地磁盘 / external shuffle service
  +-----------------------------------------------------------+
  | Shuffle 输出文件 (半持久, 非可靠存储, executor 丢即丢)    |
  +-----------------------------------------------------------+

  可靠存储 (HDFS 等)
  +-----------------------------------------------------------+
  | RDD Checkpoint 数据 (PERSISTENT)                          |
  +-----------------------------------------------------------+
```

### 7.1 持久化规则对比表

下表是本文持久化规则的统一速查口径。判定维度：**存储介质**、**driver 重启是否丢失**、
**executor 丢失是否丢失**、**写入时机**、**恢复方式**。一句话总览口径见每行"分类"列。

| # | 子系统 / 数据 | 文件 | 分类 | 存储介质 | driver 重启 | executor 丢失 | 写入时机 | 恢复方式 |
|---|---|---|---|---|---|---|---|---|
| 1 | DAGScheduler 状态桶（jobId/stage/waiting/running/failed） | [DAGScheduler.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala) L148-198 | 临时 | driver 内存 | 全丢 | 不受影响 | 运行中实时变更 | 无，整 app 重跑 |
| 2 | TaskSchedulerImpl 调度态（taskSetsByStage/taskIdToTSM/rootPool） | [TaskSchedulerImpl.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSchedulerImpl.scala) L122-173 | 临时 | driver 内存 | 全丢 | 部分清理 | 运行中实时变更 | 无 |
| 3 | MapOutputTracker 位置元数据 | [MapOutputTracker.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/MapOutputTracker.scala) | 临时 | driver 内存 | 全丢 | 对应条目失效 | map task 完成时注册 | 重算上游 ShuffleMapStage |
| 4 | Shuffle 输出文件 | executor 本地磁盘 / ESS | 半持久 | executor 本地磁盘 | 不受影响 | 丢失（除非 ESS 托管） | map task 写出 | FetchFailed 触发上游重算 |
| 5 | 缓存的 RDD 块（StorageLevel.MEMORY/DISK） | [BlockManager.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/storage/BlockManager.scala) | 半持久 | executor 内存/本地磁盘 | 不受影响 | 丢失（多副本可救） | `persist` 后首次计算 | lineage 血缘重算 |
| 6 | LiveListenerBus / AsyncEventQueue | [AsyncEventQueue.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/AsyncEventQueue.scala) | 临时 | driver 内存 | 全丢 | 不受影响 | 事件产生即入队 | 无（满则丢事件） |
| 7 | AppStatusStore（默认 InMemoryStore） | [AppStatusStore.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/status/AppStatusStore.scala) | 临时 | driver 内存 | 全丢 | 不受影响 | 监听事件实时更新 | 无 |
| 8 | AppStatusStore（LevelDB/RocksDB 后端） | [KVUtils.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/status/KVUtils.scala) | 持久（可选） | 本地磁盘 KV | 保留 | 不受影响 | 实时更新 | 重启后可读旧库 |
| 9 | EventLog 事件日志 | [EventLoggingListener.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/EventLoggingListener.scala) | 持久 | HDFS/可靠存储 | 保留 | 不受影响 | job/stage/app 边界 flush | History Server 重建 UI |
| 10 | RDD Checkpoint | [ReliableRDDCheckpointData.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/rdd/ReliableRDDCheckpointData.scala) | 持久 | HDFS/可靠存储 | 保留（数据） | 不受影响 | 显式 `checkpoint` 后触发 | 截断 lineage，直接读 |

判定口径速记：
- **临时（行 1/2/3/6/7）**：只活在 driver 内存，driver 重启即全丢，**整个 application 必须重跑**，无热恢复。
- **半持久（行 4/5）**：分布在 executor 上，单点丢失不致命，靠 **FetchFailed 重算 / lineage 血缘 / 多副本** 兜底。
- **持久（行 8/9/10）**：落到可靠/本地磁盘，可跨 driver 重启读取，但**只用于审计/UI/数据复用，不会自动恢复调度状态**。

要点：
- **EventLog 是任务管理唯一的"持久审计记录"**：任务/Stage/Job 的 start/end 事件以 JSON 落盘（[EventLoggingListener.scala#L105-L182](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/EventLoggingListener.scala#L105-L182)）。task start/end 不立即 flush，job/stage/application 边界才 flush。
- **AppStatusStore 默认临时**：`createLiveStore` 无 `spark.ui.store.path` 时回退 `InMemoryStore`（[KVUtils.scala#L167](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/status/KVUtils.scala#L167)）。
- **MapOutputTracker 与 shuffle 文件分离**：driver 内存只存位置元数据，真实文件在 executor 本地磁盘 / ESS。两者任一丢失都会触发 `FetchFailed` -> Stage 重算。

---

### 7.2 多用户 / 并发保护

Spark 单 driver 内可同时跑多个来自不同线程/用户的 Job。保护措施分两个维度：
**共享可变状态的并发安全** 与 **多 Job/多用户的逻辑隔离**。

#### 7.2.1 并发安全：三种不同策略

```text
+-- DAGScheduler ----------------------------------------------+
|  策略: 单线程事件循环串行化 (无显式锁)                       |
|  EventLoop: 1 线程 + LinkedBlockingDeque, eventQueue.take()  |
|  所有状态变更只在 dag-scheduler-event-loop 线程内发生        |
|  外部线程一律 eventProcessLoop.post(event) 入队              |
+--------------------------------------------------------------+

+-- TaskSchedulerImpl ----------------------------------------+
|  策略: 单一 `this` monitor 锁                               |
|  TaskSetManager 非线程安全, 所有访问 synchronized(this)     |
|  + taskIdToTaskSetManager 用 ConcurrentHashMap (锁外读)     |
|  + nextTaskId 用 AtomicLong                                 |
+--------------------------------------------------------------+

+-- CoarseGrainedSchedulerBackend ----------------------------+
|  策略: IsolatedThreadSafeRpcEndpoint + this 锁 + 固定锁序   |
|  executorDataMap 在 endpoint 方法内修改(消息串行)          |
|  endpoint 外访问用 CoarseGrainedSchedulerBackend.this       |
|  withLock: 先 scheduler.synchronized 再 backend.synchronized|
|           (统一加锁顺序避免死锁, SPARK-27112)               |
+--------------------------------------------------------------+
```

| 机制 | 位置 | 保护对象 |
|---|---|---|
| 单线程事件循环 | [EventLoop.scala#L35-L48](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/util/EventLoop.scala#L35-L48) + DAGScheduler.scala L3506 | DAGScheduler 全部 HashMap，无需加锁 |
| `synchronized(this)` | TaskSchedulerImpl.scala L122-123 注释 + L248/L514/L799 等 | TaskSetManager、rootPool、各类 Map |
| `@GuardedBy("CoarseGrainedSchedulerBackend.this")` | CoarseGrainedSchedulerBackend.scala L77-124 | executorDataMap 等 |
| ConcurrentHashMap | taskIdToTaskSetManager(L133)、shuffleStatuses、activeQueryToJobs | 跨线程只读/原子写 |
| Atomic* | nextJobId/nextStageId、nextTaskId、totalCoreCount | 无锁 ID/计数分配 |
| 固定锁序 `withLock` | CoarseGrainedSchedulerBackend.scala L1053-1054 | 避免与 scheduler 锁死锁 |

DAGScheduler 中少数需被其它线程访问的方法明确标注线程安全，只走 thread-safe 访问器，
如 `getPreferredLocs`（[L3417-L3441](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala#L3417-L3441)），
跨线程共享的 `activeQueryToJobs` / `jobIdToQueryExecutionId` 用 `ConcurrentHashMap`。

#### 7.2.2 多 Job / 多用户隔离

```text
  线程 A (用户/查询 A)            线程 B (用户/查询 B)
  setLocalProperty(pool=etl)     setLocalProperty(pool=adhoc)
  setJobGroup(gid=A)             setJobGroup(gid=B)
        |                              |
        +---------- 共享 SparkContext --+
                       |
                       v
            FairSchedulableBuilder.addTaskSetManager
            按 local property "spark.scheduler.pool" 路由
                       |
        rootPool (FAIR)
        +-- pool "etl"   (minShare/weight)  --> TSM ...
        +-- pool "adhoc" (minShare/weight)  --> TSM ...
        +-- pool "default"                  --> TSM ...
```

| 隔离手段 | 位置 | 说明 |
|---|---|---|
| Local property（线程本地，可继承） | `setLocalProperty` / InheritableThreadLocal | 不同线程/用户的属性互不干扰 |
| Job Group | `SparkContext.setJobGroup`（[L873-L882](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/SparkContext.scala#L873-L882)） | 设置 group id / description / interruptOnCancel，支持按组取消 |
| FAIR 调度池 | [SchedulableBuilder.scala#L60-L251](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/SchedulableBuilder.scala#L60-L251) | 按 `spark.scheduler.pool` 路由到独立 pool，每池有 minShare/weight |
| 调度算法 | [SchedulingAlgorithm.scala](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/SchedulingAlgorithm.scala) | FIFO 比 priority；FAIR 保证每池 minShare 后按 weight 公平分配 |

调度算法对比：

| 算法 | 比较规则 | 效果 |
|---|---|---|
| FIFOSchedulingAlgorithm | 先比 priority(jobId)，再比 stageId | 先提交先得，扁平 rootPool->TSM |
| FairSchedulingAlgorithm | needy(runningTasks<minShare) 优先；都 needy 比 minShareRatio；都不 needy 比 runningTasks/weight | 每池至少拿 minShare，余量按 weight 公平 |

#### 7.2.3 取消的线程安全

任意用户线程发起的取消都被转成事件，统一在 DAG 单线程事件循环内串行处理，
因此取消与提交、任务完成天然不并发冲突：
- `JobCancelled -> handleJobCancellation`、`JobGroupCancelled -> handleJobGroupCancelled`、`JobTagCancelled`、`AllJobsCancelled`（DAGScheduler.scala L3538-L3547）。
- `cancelledJobGroups`（`LimitedSizeFIFOSet`）记录"取消未来 job"的 group，新 job 命中即取消；该集合非线程安全但只在事件线程内访问。
- TaskSchedulerImpl 侧 `cancelTasks` / `killTaskAttempt` 在 `synchronized` 内执行，保证与调度状态一致。

---

## 8. 计算与存储资源管理

调度的前提是"有资源可用"。Spark 的资源管理分两条线：**计算资源**（executor / CPU / GPU 槽位，决定能并行跑多少 task）与**存储资源**（统一内存 + 块管理，决定数据放哪、放不下时怎么办）。两条线都在 driver 侧汇聚，但治理机制完全不同。

```text
   计算资源
   +---------------------------------------------------------------+
   | ExecutorAllocationManager  动态分配: 积压则指数扩容, 空闲则回收 |
   | ResourceProfile            每 stage 的 CPU/GPU/内存画像        |
   | CPUS_PER_TASK + WorkerOffer 决定单 executor 的并行槽位         |
   | SchedulerBackend           向 cluster manager 申请/杀 executor |
   +---------------------------------------------------------------+

   存储资源
   +---------------------------------------------------------------+
   | UnifiedMemoryManager  execution/storage 统一区域, 软边界借用   |
   | TaskMemoryManager     task 级执行内存配额 + spill 触发          |
   | BlockManager          块的存放/读取/复制 (内存<->磁盘<->远程)   |
   | MemoryStore/DiskStore 具体落地与驱逐                           |
   +---------------------------------------------------------------+
```

### 8.1 计算资源：动态分配与资源画像

| 机制 | 文件 | 作用 |
|---|---|---|
| 动态分配主循环 | [ExecutorAllocationManager.scala#L341-L352](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/ExecutorAllocationManager.scala#L341-L352) | 每 100ms `schedule()` 计算每个 ResourceProfile 的目标 executor 数 |
| 指数扩容 | [ExecutorAllocationManager.scala#L457-L459](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/ExecutorAllocationManager.scala#L457-L459) | 有 pending task 积压时目标数翻倍增长，快速追上负载 |
| 需求计算 | [ExecutorAllocationManager.scala#L297-L330](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/ExecutorAllocationManager.scala#L297-L330) | `maxNumExecutorsNeededPerResourceProfile` 按待运行/运行中 task 与每 executor 槽位估算 |
| 空闲回收 | [ExecutorMonitor.scala#L109-L138](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/dynalloc/ExecutorMonitor.scala#L109-L138) | `timedOutExecutors` 找出空闲超 `executorIdleTimeout` 的 executor 待回收 |
| 资源画像 | [ResourceProfile.scala#L195-L251](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/resource/ResourceProfile.scala#L195-L251) | `calculateTasksAndLimitingResource` 按 CPU/GPU/内存算出每 executor 可跑 task 数与限制资源 |
| 槽位计算 | [TaskSchedulerImpl.scala#L1239-L1283](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSchedulerImpl.scala#L1239-L1283) | `calculateAvailableSlots` 按 `CPUS_PER_TASK` 和自定义资源算可用槽 |
| 申请/杀 executor | [CoarseGrainedSchedulerBackend.scala#L896-L992](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/cluster/CoarseGrainedSchedulerBackend.scala#L896-L992) | `doRequestTotalExecutors` / `doKillExecutors` 是面向 cluster manager 的抽象点 |

要点：

- **资源诉求按 stage 画像计算**：`ResourceProfile` 允许不同 stage 声明不同 CPU/GPU/内存，`DEFAULT_RESOURCE_PROFILE_ID=0` 是默认画像。
- **扩容快、缩容慢**：积压触发指数扩容；回收等待 `executorIdleTimeout`，`cachedExecutorIdleTimeout` 默认近似无限，用于保护缓存块。
- **一个 task 占多少由 `spark.task.cpus` 决定**：`WorkerOffer.freeCores` 是 offer 的资源面，槽位用尽即停止派发。

关键配置：

| 配置 | 默认 | 作用 |
|---|---|---|
| `spark.dynamicAllocation.enabled` | false | 开启动态分配 |
| `spark.dynamicAllocation.minExecutors` | 0 | executor 下限 |
| `spark.dynamicAllocation.maxExecutors` | Int.MaxValue | executor 上限 |
| `spark.dynamicAllocation.schedulerBacklogTimeout` | 1s | 积压多久触发扩容 |
| `spark.dynamicAllocation.executorIdleTimeout` | 60s | 空闲多久回收 |
| `spark.task.cpus` | 1 | 单 task 占用核数 |

### 8.2 存储资源：统一内存与块管理

```text
  executor JVM 堆 (spark.memory.fraction=0.6 之内)
  +-------------------------------------------------------+
  | Execution 区  <----软边界, 可互相借用---->  Storage 区 |
  | shuffle/join/sort/agg 临时缓冲          cache/broadcast |
  | 执行可驱逐存储借走的内存, 存储不能驱逐执行内存          |
  +-------------------------------------------------------+
          | 放不下
          v
  spill 到本地磁盘 / LRU 驱逐 / 按 StorageLevel 复制副本
```

| 机制 | 文件 | 作用 |
|---|---|---|
| 统一内存管理 | [UnifiedMemoryManager.scala#L134-L248](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/memory/UnifiedMemoryManager.scala#L134-L248) | execution/storage 共享一池，软边界互借；执行内存不足时可驱逐存储借走的内存 |
| 预留内存 | [UnifiedMemoryManager.scala#L264](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/memory/UnifiedMemoryManager.scala#L264) | `RESERVED_SYSTEM_MEMORY_BYTES=300MB` 防止小堆 OOM |
| task 级配额 + spill | [TaskMemoryManager.java#L159-L238](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/java/org/apache/spark/memory/TaskMemoryManager.java#L159-L238) | `acquireExecutionMemory` 不足时调用 `MemoryConsumer.spill` 把数据落盘换内存 |
| 块管理 | [BlockManager.scala#L1676-L1759](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/storage/BlockManager.scala#L1676-L1759) | `doPutIterator` 写块，内存放不下转磁盘；`getOrElseUpdate` 读不到则计算并缓存 |
| 块复制 | [BlockManager.scala#L1910-L2003](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/storage/BlockManager.scala#L1910-L2003) | `replicate` 按 StorageLevel 副本数复制到其它节点 |
| LRU 驱逐 | [MemoryStore.scala#L472-L567](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/storage/memory/MemoryStore.scala#L472-L567) | `evictBlocksToFreeSpace` 按 LRU 驱逐冷块 |
| 块位置全局视图 | [BlockManagerMasterEndpoint.scala#L795-L863](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/storage/BlockManagerMasterEndpoint.scala#L795-L863) | driver 侧 `blockLocations` 维护"哪个块在哪个 executor" |

要点：

- **执行内存优先级高于存储内存**：执行可以驱逐存储借走的内存，存储不能反向驱逐执行，保证 shuffle/sort 不因缓存而 OOM。
- **放不下不等于失败**：执行内存不足触发 spill；存储内存不足触发 LRU 驱逐，`MEMORY_AND_DISK` 可落盘，`MEMORY_ONLY` 丢弃后待重算。
- **块位置是临时元数据**：executor 丢失会触发位置失效，缓存块靠 lineage 重算或副本恢复。

---

## 9. 机器异常下线的调度与数据不丢流程

本章回答："集群里一台机器突然宕机（executor 进程随之消失），Spark 怎么发现、怎么调度补救、怎么保证正在写出的数据不重复/不丢、查询结果不缺失。"核心是**检测 -> 级联清理 -> 重算 -> 提交仲裁**四步。

### 9.1 端到端流程

```text
[1] 检测: 心跳超时 / RPC 断连
    HeartbeatReceiver.expireDeadHosts (L210-249)
    CoarseGrainedSchedulerBackend.onDisconnected (L398-405)
        |
        v
[2] 级联清理
    CoarseGrainedSchedulerBackend.removeExecutor (L459-503)
        -> TaskSchedulerImpl.executorLost / removeExecutor (L1006-1119)
        -> TaskSetManager.executorLost (L1150-1226)
        -> DAGScheduler.handleExecutorLost (L3079-3092)
        -> removeExecutorAndUnregisterOutputs (L3108-3161)
        |
        v
[3] 重算
    下游 fetch 不到数据 -> FetchFailed (L2395-2555)
        -> failedStages += (failedStage, mapStage)
        -> resubmitFailedStages (L1244-1256)
        -> HealthTracker.updateExcludedForFetchFailure (L220-262)
        |
        v
[4] 提交仲裁
    SparkHadoopMapRedUtil.commitTask (L41-100)
        -> OutputCommitCoordinator.canCommit (L95-110)
        -> first-committer-wins, 同一 partition 只允许一个 attempt 提交
```

### 9.2 系统核心操作点

| # | 核心操作点 | 文件 | 保证什么 |
|---|---|---|---|
| 1 | 心跳超时检测 | [HeartbeatReceiver.scala#L210-L249](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/HeartbeatReceiver.scala#L210-L249) | 超 `executorTimeoutMs` 主动判死并发 `RemoveExecutor`，不依赖 RPC 一定断开 |
| 2 | 丢失级联清理 | [TaskSchedulerImpl.scala#L1087-L1119](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSchedulerImpl.scala#L1087-L1119) | 在 driver 锁/事件线程内串行清理，避免半更新的脏状态 |
| 3 | map 输出失效 | [DAGScheduler.scala#L3108-L3161](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala#L3108-L3161) | 删除该 host/executor 的 shuffle 位置，使下游不会去拉已经不存在的数据 |
| 4 | 运行中 task 重排 | [TaskSetManager.scala#L1150-L1226](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/TaskSetManager.scala#L1150-L1226) | 把丢失节点上未完成 task 标记 `Resubmitted` 重新入队，不算入失败次数 |
| 5 | FetchFailed 重算 | [DAGScheduler.scala#L2395-L2555](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/DAGScheduler.scala#L2395-L2555) | 下游拉不到数据时重新提交上游 Stage 重算丢失分区 |
| 6 | 坏节点排除 | [HealthTracker.scala#L220-L262](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/HealthTracker.scala#L220-L262) | 防止反复调度到同一坏机器 |
| 7 | 提交授权（写不重） | [OutputCommitCoordinator.scala#L95-L205](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/scheduler/OutputCommitCoordinator.scala#L95-L205) | 同一 partition 只有一个 attempt 能 commit，重算/推测副本被拒，避免重复写 |
| 8 | 提交前先问 | [SparkHadoopMapRedUtil.scala#L41-L100](file:///Users/bytedance/Documents/01_Projects/spark/core/src/main/scala/org/apache/spark/mapred/SparkHadoopMapRedUtil.scala#L41-L100) | task 提交输出前先申请授权，未授权不落地，保证输出文件不冲突 |

### 9.3 写入与查询为什么不丢

| 场景 | 机器下线后会发生什么 | 不丢/不重的保证 |
|---|---|---|
| Shuffle map 输出丢失 | 位置元数据被删，下游 FetchFailed | 重新提交上游 ShuffleMapStage 重算 |
| 缓存 RDD 块丢失 | BlockManagerMaster 位置失效 | lineage 血缘重算；若 StorageLevel 有副本则直接读副本 |
| 正在写 HDFS 的 task | 该 attempt 失败，被重排 | OutputCommitCoordinator 仲裁，只有先提交者落地，重排副本被拒 |
| 查询 ResultStage 丢分区 | 对应 ResultTask 重排 | 重算该分区，`numFinished` 直到全分区完成才标记 Job 成功 |
| 推测执行/重试的并发副本 | 多个 attempt 同时算同一分区 | 成功一个即 kill 其余 + 提交仲裁，输出唯一 |

要点：

- **不丢的本质是血缘可重算 + 元数据可失效**：只要 driver 未崩溃且 lineage 在，丢失的中间数据都能重算。
- **不重的本质是提交仲裁**：OutputCommitCoordinator 把"谁能写"收敛到 driver 单点裁决，消除重算/推测带来的重复写。
- **优雅退役是主动版**：计划内下线可通过 `spark.decommission.enabled` / `spark.storage.decommission.enabled` 迁移缓存块或 shuffle 数据，减少 FetchFailed 重算。

---

## 10. 性能瓶颈评估

根本约束是**两个串行单点**：DAGScheduler 单线程事件循环 + TaskSchedulerImpl 全局锁下的 resourceOffers。
其余瓶颈多为围绕"driver 单点"的衍生问题，Spark 已用一系列 SPARK JIRA 优化缓解。

| # | 瓶颈 | 位置 | 为何是瓶颈 | 现有缓解 |
|---|---|---|---|---|
| 1 | DAGScheduler 单线程事件循环 | DAGScheduler.scala L3506 + EventLoop.scala L42 | 所有 job/stage/task 完成事件串行处理，大作业/多并发时积压 | SPARK-23626 预计算 partition；SPARK-46383 清空 accumulables；TaskResultGetter 异步反序列化；`messageProcessingTimer` 可观测 |
| 2 | resourceOffers 全局锁 + 三重循环 | TaskSchedulerImpl.scala L514/L567/L590 | 持 `this` 锁，复杂度 O(offers x tasksets x localityLevels)，每轮还全量排序 | task 完成后只做单 executor offer `makeOffers(executorId)`；`shuffledOffers` 均衡 |
| 3 | 任务序列化 + 广播(taskBinary) | DAGScheduler.scala L1729-L1760 | 在事件循环线程内序列化宽 stage closure，阻塞整个循环；stage 重试重复序列化(TODO) | broadcast 分发避免随 TaskDescription 重发；超 `TASK_SIZE_TO_WARN_KIB` 告警 |
| 4 | 结果获取 / driver 内存 | TaskResultGetter.scala + TaskSetManager.scala L793 | 反序列化线程池默认 4；大结果走 BlockManager 远程拉取集中在 driver；受 `maxResultSize` 硬限 | 异步反序列化不占调度锁；`enqueuePartitionCompletionNotification` 异步化(避免 synchronized 阻塞)；超限提前 kill |
| 5 | 本地性计算 / 延迟调度 | TaskSetManager.scala L621-L681 / L1361 | 每个 offer 触发 `getAllowedLocalityLevel`，扫描/清理本地性桶；与三重循环叠加 | SPARK-4939 等待机制；SPARK-31837 只在更优级别 shift；`PercentileHeap` 仅 speculation 时用 |
| 6 | ListenerBus 丢事件 | AsyncEventQueue.scala L61/L158-L195 | 每队列单 dispatch 线程，慢 listener(如 AppStatusListener 写 KVStore)使有界队列(默认 10000)填满后**直接丢事件** | 多队列隔离慢 listener；`LIVE_ENTITY_UPDATE_PERIOD` 限流；`MAX_RETAINED_TASKS_PER_STAGE` + 异步 cleanup |
| 7 | partition 预计算(缓解 #1) | DAGScheduler.scala L875-L886 / L999-L1002 | `RDD.getPartitions()` 慢，原本会阻塞事件循环 | SPARK-23626：在提交者线程预先计算整条 DAG 的 partitions |
| 8 | Driver 单点 RpcEndpoint | CoarseGrainedSchedulerBackend.scala L168/L538 | 全集群 StatusUpdate/心跳/注册经单一 endpoint 串行，再触发 #2 抢锁 | 局部 offer；结果异步(#4)；SPARK-27112 锁序 |
| 9 | 大量 task 的 driver 内存 | TaskSetManager.scala L78/L125/L201 | 每 TSM 为 numTasks 分配多个数组/List/HashMap；accumulables 长期驻留 | SPARK-46383 清空 accumulables；反向索引 `executorIdToTaskIds` 避免 O(N) 扫描 |
| 10 | getSortedTaskSetQueue 全量排序 | Pool.scala L105-L113 | 每次 offer 都 O(n log n) 排序整棵池树，FAIR 比较器更贵 | 区分全量/局部 offer 减少调用频率 |

关键配置旋钮：

| 配置 | 默认 | 缓解 |
|---|---|---|
| `spark.scheduler.listenerbus.eventqueue.capacity` | 10000 | #6 丢事件 |
| `spark.driver.maxResultSize` | 1g | #4 driver 内存 |
| `spark.resultGetter.threads` | 4 | #4 结果反序列化并发 |
| `spark.locality.wait[.process/node/rack]` | - | #5 延迟调度 |
| `spark.ui.store.path` | - | 持久化 UI store / 减内存 |
| `spark.scheduler.mode` | FIFO | FIFO/FAIR 切换 |

---

## 11. 去中心化 DAG 管理设计探讨

> 本章是面向未来的**设计探讨**，不是对当前 Spark 实现的描述。结论列在前，推导在后。

**命题**：大多数任务管理平台是"以 DAG 为核心的中心化平台"，中心节点既存全量 DAG、又做调度决策，
因此必然引入各种锁（Spark 即是典型：DAGScheduler 单线程事件循环 + TaskSchedulerImpl 全局锁，见第 10 章）。
设想反过来——**让每个 task manager 自己管理它那部分 DAG，中心只做索引**。

### 11.1 两种形态对比

| 维度 | 中心化 DAG（Spark 现状） | 去中心化 DAG + 中心索引（探讨） |
|---|---|---|
| DAG 存放 | 全量在 driver 内存 | 切分到各 task manager 本地 |
| 调度决策 | 中心串行（事件循环 + 全局锁） | 各 task manager 本地决策，无需抢全局锁 |
| 中心节点职责 | 调度 + 状态 + 容错全包 | 只存"谁负责哪段 DAG / shuffle 在哪"的索引 |
| 锁 | 重度依赖（见第 4、7 章） | 中心几乎无锁（索引读多写少），锁下沉到本地 |
| 中心压力 | 高（百万 task 时是瓶颈 #1/#2） | 低（只转发索引查询） |
| 任务重启 | driver 挂则整 app 重跑（见 [3.1](#31-持久化规则对比表)） | 单个 task manager 挂只影响其子图，可局部重启 |
| 全局视图 | 强一致、即时 | 最终一致，需额外协调 |

### 11.2 设计草图

```text
              +-----------------------------+
              |  中心索引服务 (轻量, 读多写少) |
              |  - subgraph owner 映射        |
              |  - shuffle/block 位置索引     |
              |  - 仅做查询/注册, 不做调度决策 |
              +--------------+--------------+
                 ^ 注册/查询  | 查询
        +--------+-----+ +----+---------+ +--------------+
        | TaskManager A| | TaskManager B| | TaskManager C|
        | 本地子 DAG    | | 本地子 DAG    | | 本地子 DAG    |
        | 本地调度+重试 | | 本地调度+重试 | | 本地调度+重试 |
        +------+-------+ +------+-------+ +------+-------+
               |   跨子图依赖按索引点对点拉取        |
               +------------------<---------------+
```

### 11.3 优点（与命题一致）

- **中心压力小**：中心退化为索引，不再是调度热点；Spark 当前的瓶颈 #1（事件循环）、#2（全局锁）、#8（单点 RpcEndpoint）天然消解。
- **任务重启更轻松**：DAG 按 owner 分片，单个 task manager 故障只需重建它那段子图并向索引重新注册，不必像 driver 崩溃那样整 app 重跑。
- **锁下沉、粒度变细**：全局锁拆成每个 task manager 的本地锁，并发冲突面变小。
- **天然水平扩展**：加 task manager 即扩调度吞吐，不受单 driver 上限约束。

### 11.4 代价与必须回答的问题（探讨需诚实）

| 难点 | 中心化下如何被"免费"解决 | 去中心化下需要新机制 |
|---|---|---|
| 跨子图依赖一致性 | driver 持全局 DAG，依赖即指针 | 需分布式 DAG 切分 + 跨节点依赖协议 |
| 提交 exactly-once | OutputCommitCoordinator 单点仲裁（见 6.2） | 失去单点仲裁，需分布式提交协议（2PC/租约/幂等写） |
| 全局状态视图 | driver 内存即真相 | 索引最终一致，存在窗口期不一致 |
| 死锁/环检测 | 中心可全局判环 | 子图各自只见局部，需全局环检测协议 |
| 调度公平性 | rootPool（FAIR/FIFO）全局裁决（见第 4 章） | 跨 task manager 公平性需额外协调，易退化为局部最优 |
| 索引一致性 | 不涉及 | 索引本身成为新的关键路径，需考虑其可用性与一致性级别 |

### 11.5 评估结论（待确认）

- **方向合理，但"锁"不会消失，只是搬家**：中心化的全局锁被替换为"分布式协调"，复杂度从单点串行转移到跨节点协议。对**调度吞吐**是净收益，对**强一致语义（尤其提交 exactly-once）**是净成本。
- **关键风险点是提交仲裁**：第 9 章已说明 Spark 不丢/不重的核心是 OutputCommitCoordinator 单点裁决；去中心化后必须用分布式提交协议补齐，否则会牺牲"写不重"这条硬保证。
- **建议的折中**：保留**中心做提交仲裁与全局环检测**这类"必须强一致"的小职责，把**调度决策与子图状态**下沉到 task manager。即"中心只做索引 + 少量强一致仲裁"，而非"中心纯索引"。这与本文档第 12 章的 Pulse 演进思路一致——固定一个轻量中心会话层，把复杂能力放到边缘节点。

---

## 12. 结合 Pulse 心跳平台的资源管理问题总结与演进建议

用户提供的 Pulse 心跳平台设计已经具备资源管理雏形：agent 周期性上报 `state.heartbeat`，payload 中包含 host/load/zone/role 与 `tide_workers[]` 的 pid、CPU、内存、端口和版本；coordinator 维护 `NodeState` 与 `MessageLedger`，用 `epoch + seq` 做幂等合并，Alive/Warming/Expired 由 TTL 与确认窗口判断。这个模型适合做**状态采集与轻量控制面**。

如果 Pulse 要成为任务管理基座，结论是：**当前最大设计缺陷不是心跳不够强，而是任务管理语义缺失**。Spark 的经验表明，任务管理至少需要 DAG 状态机、资源供需模型、任务唯一归属、失败重排、输出提交仲裁、调度状态审计六个闭环；Pulse 当前只覆盖了其中的"节点状态可见"和"指令消息幂等"两部分。

### 12.1 Pulse 与 Spark 资源管理对照

| 维度 | Spark 当前机制 | Pulse 当前设计 | 差距 / 启示 |
|---|---|---|---|
| 存活检测 | HeartbeatReceiver 超时 + RPC 断连 | `ttl_ms` + 20s 内 3 个不同 epoch/seq 确认 | Pulse 已有基础，但还缺与调度摘除/恢复的强绑定 |
| 计算资源画像 | ResourceProfile + CPUS_PER_TASK + WorkerOffer | `state.heartbeat.payload` 上报 load 与 tide_workers | 需要把指标升级为可调度的 capacity/slots/resource profile |
| 动态伸缩 | ExecutorAllocationManager 按 backlog 扩缩 executor | coordinator 可通过 `cmd.*` 下发指令 | 需要定义 backlog、目标副本数、冷却时间和幂等扩缩容命令 |
| 存储资源 | UnifiedMemoryManager + BlockManager + StorageLevel | 当前主要是进程/主机指标 | 需要补充磁盘、数据分片、副本、热数据位置与迁移状态 |
| 节点下线 | removeExecutor -> map 输出失效 -> FetchFailed 重算 | Expired 状态 + peer 最终一致传播 | 需要定义下线后的任务迁移、数据重建、提交仲裁 |
| 写入一致性 | OutputCommitCoordinator first-committer-wins | MessageLedger 去重命令 | 需要为业务写入增加 partition/attempt 级提交仲裁或幂等写协议 |

### 12.2 问题总结

| 问题 | 现象 | 风险 |
|---|---|---|
| 指标还不是资源模型 | load/cpu/mem 是观测值，不等于可分配槽位 | 调度无法精确判断某节点还能接多少 task |
| 缺少需求侧信号 | 只有 agent 状态，没有 pending task/backlog | 只能被动看负载，无法像 Spark 动态分配一样主动扩缩 |
| 存储语义不足 | 未描述数据块、分片、副本、落盘位置 | 节点下线后无法判断哪些数据要迁移、哪些可重算 |
| 最终一致与调度决策冲突 | peer 间状态异步传播 | 多 coordinator 可能短暂给同一资源下发冲突指令 |
| 指令账本粒度偏粗 | MessageLedger 能去重 command，但不表达 task attempt / partition ownership | 无法自然保证写入 exactly-once 或任务唯一归属 |
| group 不是状态权威 | group 只聚合转发 | group 下节点异常时，coordinator 需要更明确的降级与重连策略 |

### 12.3 作为任务管理基座的设计缺陷与整改建议

| # | 设计缺陷 | 对比 Spark | 影响 | 整改建议 |
|---|---|---|---|---|
| 1 | 缺少 DAG / Stage / Task 层级模型 | Spark 明确 Job -> Stage -> TaskSet -> Task，并有状态收敛点 | 只能管理节点和命令，无法表达任务依赖、重试范围和局部恢复边界 | 增加 `DagSpec`、`StageSpec`、`TaskSpec`、`TaskAttempt` 四层模型，先支持 DAG 分片和子图摘要 |
| 2 | 缺少任务唯一归属（ownership） | Spark task attempt 由 driver 分配 taskId，TaskSetManager 收敛状态 | 多 coordinator / 多 task manager 下可能重复派发同一 task | 为每个 task 引入 `owner_epoch`、`lease_id`、`attempt_id`，中心只做 lease/attempt 分配与过期回收 |
| 3 | MessageLedger 粒度太粗 | Spark 区分 task start/end、stage end、job end、executor lost | `reply.command_result` 只能说明命令成功，不说明任务最终语义 | 扩展 `TaskLedger`：`pending/running/succeeded/failed/killed/lost/committed`，reply 只作为状态输入 |
| 4 | 资源上报不是资源供给模型 | Spark 用 ResourceProfile + WorkerOffer 做供需匹配 | `load/cpu_percent/mem_percent` 是观测指标，不等于可调度 slot | 增加 `ResourceOffer{slots,cpu,memory,disk,labels,version,health}`，并和 `TaskResourceProfile` 匹配 |
| 5 | 缺少 backlog / demand 信号 | Spark 动态分配按 pending task/backlog 扩缩 executor | coordinator 只能被动看机器状态，无法主动扩缩容 | task manager 上报 `pending_tasks/running_tasks/blocked_tasks`，coordinator 计算目标容量 |
| 6 | 最终一致 peer 状态不能直接用于强调度 | Spark driver 内调度状态强一致；只有 UI/事件可异步 | 多 coordinator 可能基于不同视图重复调度或重复下发写入 | 将资源可见性保持最终一致，但把 ownership、quota、commit token 设为单 writer 或强一致小服务 |
| 7 | 缺少输出提交仲裁 | Spark OutputCommitCoordinator 保证 first-committer-wins | 重试/推测/故障恢复时可能重复写或覆盖输出 | 增加 `CommitCoordinator`：按 `job_id/stage_id/partition_id` 发放一次性 commit token |
| 8 | 缺少 shuffle / block / 分片位置索引 | Spark 有 MapOutputTracker 与 BlockManagerMaster | 节点下线后不知道哪些中间数据丢失、哪些可复用 | 增加 `DataLocationIndex`：记录 block/shard owner、replicas、epoch、rebuild 状态 |
| 9 | 节点状态缺少调度态 | Spark executor 有 running/lost/decommissioned，HealthTracker 可排除坏节点 | Alive 节点不一定可调度，Expired 节点也可能仍有待迁移数据 | 将节点状态拆成 `liveness` 与 `schedulability`：`alive/warming/expired` + `active/draining/excluded/decommissioning` |
| 10 | Group 降压层边界容易被误用 | Spark 没有让 executor 代理 driver 决策；关键裁决仍在 driver | 如果 group 参与调度裁决，会引入不一致和排障困难 | 明确 group 只转发/聚合，不做 owner 分配、commit、quota 和 DAG 决策 |
| 11 | 缺少调度审计和回放 | Spark EventLog 可重建 History UI | 故障后难以解释“谁调度了什么、为什么重复/丢失” | 增加 `TaskEventLog`：记录 dag submitted、task launched、attempt ended、commit granted、node lost |
| 12 | 缺少核心状态机文档 | Spark 的设计精髓是状态机和唯一收敛点 | 后续能力会堆在消息类型里，语义漂移 | 为 Node、TaskAttempt、Stage、DAG、Command、CommitToken 分别定义状态机 |

整改原则：

- **不要把心跳变成调度本身**：心跳只负责采样、回报和承载轻量指令；调度需要独立的 DAG/Task/Lease/Commit 模型。
- **最终一致用于观测，强一致用于裁决**：节点指标、UI、拓扑可以最终一致；task ownership、quota、commit token 必须有单调裁决点。
- **中心只保留小而硬的职责**：中心不管理每个 task 的运行细节，但必须管理全局唯一性、配额、索引和提交授权。
- **边缘节点负责大而频繁的职责**：task manager 管理本地 DAG 分片、重试、缓存、spill、局部恢复和批量状态上报。

### 12.4 整改后的任务管理基座形态

```text
+-----------------------------+
| Coordinator / Index Plane   |
| - Node/Resource Index       |
| - Task Lease / Ownership    |
| - Quota / Fairness          |
| - Commit Token              |
| - EventLog / Audit          |
+-------------+---------------+
              |
              | heartbeat response: cmd.*, lease, throttle
              v
+-----------------------------+
| Task Manager / Agent        |
| - Local DAG Fragment        |
| - TaskAttempt State Machine |
| - Local Retry / Backoff     |
| - Resource Snapshot         |
| - Data/Block Report         |
+-------------+---------------+
              |
              | heartbeat: state.*, reply.*, task events, offers
              v
+-----------------------------+
| Pulse Group (optional)      |
| - Batch / Forward only      |
| - No ownership decision     |
| - No commit decision        |
+-----------------------------+
```

核心对象建议：

| 对象 | 作用 | 最小字段 |
|---|---|---|
| `DagSpec` | 描述任务图和跨 stage 依赖 | `dag_id`、`version`、`stages[]`、`edges[]`、`submitter` |
| `TaskSpec` | 描述一个可调度任务 | `task_id`、`stage_id`、`partition_id`、`resource_profile_id`、`inputs[]` |
| `TaskAttempt` | 描述一次执行尝试 | `attempt_id`、`task_id`、`owner_agent_id`、`lease_id`、`state`、`epoch` |
| `ResourceOffer` | 描述节点可用资源 | `agent_id`、`free_slots`、`cpu`、`memory`、`disk`、`labels`、`health` |
| `TaskLease` | 保证任务唯一归属 | `lease_id`、`task_id`、`owner`、`expire_at_ms`、`fencing_token` |
| `CommitToken` | 保证输出只提交一次 | `job_id`、`stage_id`、`partition_id`、`attempt_id`、`granted_at_ms` |
| `DataLocation` | 描述中间数据位置 | `data_id`、`owner_agent_id`、`replicas[]`、`epoch`、`state` |
| `TaskEvent` | 审计和回放 | `event_id`、`type`、`task_id`、`attempt_id`、`ts_ms`、`payload` |

### 12.5 整改优先级

| 优先级 | 必做项 | 原因 | 验收口径 |
|---|---|---|---|
| P0 | 定义 Task/DAG/Attempt/Lease 状态机 | 没有状态机就无法做可靠任务管理 | 文档中能画出状态转换，代码中有唯一收敛点 |
| P0 | 区分 `liveness` 与 `schedulability` | 活着不代表可调度 | Expired、Draining、Excluded 节点不会接新 task |
| P0 | 增加 ResourceOffer / TaskResourceProfile | 解决资源供需匹配 | 调度决策不再依赖 load 字符串 |
| P1 | 增加 TaskLease 和 fencing token | 解决重复派发和脑裂 | 同一 task 同一时间只有一个有效 owner |
| P1 | 增加 CommitToken | 解决重复写 | 同一 partition 只有一个 attempt 能 commit |
| P1 | 增加 DataLocationIndex | 支持机器下线后的数据恢复 | 节点丢失后能列出需重算/迁移的数据 |
| P2 | 增加 TaskEventLog | 支持审计、回放和问题定位 | 能回答“谁在什么时候把哪个 attempt 调度到哪里” |
| P2 | 引入 HealthTracker 类机制 | 避免坏节点反复接任务 | 连续失败节点会被临时 exclude |
| P3 | 支持去中心化 DAG 分片 | 降低中心压力 | task manager 可独立重启本地子图，中心只修正索引 |

### 12.6 演进建议

| 阶段 | 建议 | 核心产物 |
|---|---|---|
| P0：资源可观测 | 扩展 `state.metrics` / `state.capability`，区分 total/used/free/reserved | `ResourceSnapshot{cpu,memory,disk,network,slots,labels}` |
| P1：资源可调度 | 将 `tide_workers[]` 升级为 worker slot 模型，支持 `role/zone/version/capability` 过滤 | `WorkerOffer` 类似结构，明确 `free_slots` 与限制资源 |
| P2：调度闭环 | 引入 pending/backlog 上报与目标副本数，coordinator 通过 `cmd.scale_out/scale_in` 下发幂等命令 | 类似 Spark ExecutorAllocationManager 的扩缩容状态机 |
| P3：存储资源 | 上报本地数据块、磁盘水位、副本数、热数据位置 | `BlockLocationIndex` / `ShardReplicaState` |
| P4：故障恢复 | Expired 节点触发资源摘除、任务重排、数据重建或副本提升 | `NodeLost` 事件流 + reschedule/rebuild plan |
| P5：一致性边界 | 对写入类任务增加 commit token 或 lease，中心只仲裁最终提交 | 类似 OutputCommitCoordinator 的轻量提交仲裁 |

### 12.7 推荐架构取舍

```text
Pulse Coordinator
  - 保存资源索引、节点存活、命令账本、提交仲裁
  - 不承担每个 task 的细粒度执行状态

Task Manager / Agent
  - 本地管理 worker slot、子 DAG、重试、采样与本地限流
  - 通过 heartbeat 上报资源快照和子图摘要

Storage/Block Index
  - 可先放在 coordinator 内，后续独立
  - 记录数据块位置、副本、重建状态
```

结论：Pulse 最适合沿着"**中心轻量索引 + 边缘自管理 + 少量强一致仲裁**"演进。与 Spark 相比，Spark 的 driver 把调度、资源、提交仲裁都收敛到单点，正确性强但扩展性受限；Pulse 可以保留心跳/消息层的简洁性，把 task manager 做厚，让中心只承担资源索引、故障摘除和 commit token 这类必须统一裁决的职责。

---

## 13. 风险与未解决问题

- **driver 单点失效**：调度运行时状态全在 driver 内存，driver 崩溃则整个 application 重跑，无法热恢复（仅 EventLog 可事后复盘）。
- **事件循环/全局锁的天花板**：超大规模（百万级 task、大量并发 Job）下，#1 与 #2 仍是硬约束，现有优化只是减负而非消除串行。
- **ListenerBus 丢事件导致 UI/监控失真**：高 task 速率下 AppStatusListener 可能跟不上，UI 数据不准（仅告警，不阻塞调度）。
- **Barrier 调度可扩展性**：资源不足时无法在提交时直接失败，会反复空转 offer（SPARK-24819 待办）。
- `待确认`：本文基于当前 `master`，行号可能随后续提交漂移；引用前建议用文件名+符号名二次定位。

## 14. 验证 / 复核方式

- 组件与行号：按表中 file 链接逐项打开核对。
- 状态机收敛点：搜索 `markStageAsFinished`、`handleSuccessfulTask`、`failJobAndIndependentStages` 确认唯一性。
- 并发策略：在 TaskSchedulerImpl.scala 搜索 `synchronized`、在 CoarseGrainedSchedulerBackend.scala 搜索 `GuardedBy` / `withLock`。
- 持久化：搜索 `spark.eventLog`、`InMemoryStore`、`createLiveStore`、`ReliableCheckpoint`。
- 性能瓶颈 JIRA：在源码注释中搜索对应 `SPARK-23626 / SPARK-46383 / SPARK-27112 / SPARK-4939 / SPARK-31837` 佐证。
