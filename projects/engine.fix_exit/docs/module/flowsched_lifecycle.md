# FlowSched（本地调度）生命周期与排障手册

本文描述 `tide_worker` 内部 **FlowSched**（本地调度/服务发现）模块的生命周期、数据通路与常见问题定位方法。

约束：文中不引用任何“日志文件名:行号”的形式；仅引用代码位置与日志内容样例（日志内容可能随版本变化）。

## 1. 模块定位与职责

- FlowSched Client 初始化入口在 `src/runtime/taskmanager/task_manager.cpp:742`，通过 `control::flowsched::GrpcClient::Make()` 创建 client，并调用 `Start()` 拉起后台线程（`src/runtime/taskmanager/task_manager.cpp:755`）。
- 该模块对每个 task 暴露 `Channel` 抽象（`src/control/flowsched/client.h:284`），用于：
  - 周期性上报 `Heartbeat`（主要携带 `force_cutoff`）
  - 周期性上报 `Statsreport`（队列长度、CPU/内存、RPS）
  - 周期性拉取 `GetService`（调度结果/下游任务集合），并将结果 push 到 channel 队列
  - 可选：`GroupBy` 拉取窗口快照并查询结果（`GroupByBase`）

## 2. 生命周期（从进程启动到 task 运行）

### 2.1 TaskManager 初始化阶段

1. 读取/构造 `ClientOptions`（cluster、accessor、resourceid、getServiceIntervalSec）并创建 `GrpcClient`（`src/runtime/taskmanager/task_manager.cpp:744`）。
2. 调用 `GrpcClient::Start()`：
   - 创建 `localsched_grpc_event_logger`（日志内容 pattern 固定为 `time=... threadId=... level=...`）（`src/control/flowsched/grpc_client.cpp:137`）
   - 启动 3 个后台线程（`src/control/flowsched/grpc_client.cpp:147`）：
     - `heartbeat_reader`（线程名 `lflowHB`，`src/control/flowsched/grpc_client.cpp:157`）
     - `statsreport_reader`（`src/control/flowsched/grpc_client.cpp:268`）
     - `getservice_trigger`（`src/control/flowsched/grpc_client.cpp:322`）

### 2.2 Channel 创建与注册

- 上层通过 `GrpcClient::CreateChannel()` 创建具体 channel，并把它加入 `ClientBase` 的 channel 列表（`src/control/flowsched/grpc_client.cpp:96`，`src/control/flowsched/client_base.cpp:194`）。
- `Channel` 的最小标识是 `(jobId, taskId)`（`src/control/flowsched/client.h:79`）。
- `ClientBase::__AvailableChannels()` 会在内部做“自动清理”：当 `shared_ptr` 仅剩 `ClientBase` 自己持有（`use_count()==1`）时，会从列表中移除该 channel（`src/control/flowsched/client_base.cpp:364`）。
  - 这意味着：上层不再持有 channel 时，它会被后台线程自然淘汰（无需显式 Remove）。

### 2.3 Task 触发上报（Heartbeat / Statsreport）

`TaskBuilder` 把调度上报作为定时事件触发（`src/runtime/taskmanager/task_builder.h:88`）：

- `on_trigger_heartbeat_event`：
  - 若 `task->IsStopFlow()` 则直接返回，不再上报（`src/runtime/taskmanager/task_builder.h:90`）。
  - 每 10 次 tick 打一次 info 样例：
    - `name=flowschedHearbeat taskId=<...> loopNo=<...>`（`src/runtime/taskmanager/task_builder.h:95`）
  - 上报 payload：`force_cutoff = task->IsForceCutoff()`（`src/runtime/taskmanager/task_builder.h:98`）。

- `on_trigger_statsreport_event`：
  - 同样在 `IsStopFlow()` 时停止上报（`src/runtime/taskmanager/task_builder.h:104`）。
  - 计算队列长度（用多个输入队列的 capacity/available 的平均值避免遍历成本）（`src/runtime/taskmanager/task_builder.h:108`）。
  - 每 10 次 tick 打 info 样例：
    - `name=flowschedStatsReport taskId=<...> loopNo=<...> availQueueLen=<...> queueLenCapacity=<...>`（`src/runtime/taskmanager/task_builder.h:118`）
  - 上报指标：CPU/内存/RPS 等（`src/runtime/taskmanager/task_builder.h:124`），在 gRPC 侧映射为 kv：
    - `cpu-of-machine`/`used-cpu-of-machine`/`memory-of-machine`/`used-memory-of-machine`/`num-rows-per-second`（`src/control/flowsched/client.h:50`）。

上报写入路径：`ChannelBase::SendHeartbeat/Statsreport()` 把数据 push 进 `CRingArray`（`src/control/flowsched/client_base.cpp:17`、`src/control/flowsched/client_base.cpp:33`）。
若 ringarray push 失败，会返回错误：

- `flowsched: put the heartbeat data into the RingArray An error occurred, errcode: <code>`
- `flowsched: put the statsreport data into the RingArray An error occurred, errcode: <code>`

## 3. 后台线程的数据通路

### 3.1 Heartbeat：批量收敛 + gRPC 双向流

- 读端线程：`GrpcClient::heartbeat_reader()`（`src/control/flowsched/grpc_client.cpp:157`）
- 关键行为：
  - 从各 channel 的 ringarray 中“只取最新一条 Heartbeat”（`ChannelBase::PopLatestHeartbeat()`，`src/control/flowsched/client_base.cpp:118`）
  - 聚合成 `unordered_map<jobId, taskIds>`，并写入 `tide_flow_scheduler::HeartbeatReq`（`src/control/flowsched/grpc_client.cpp:170`）
  - `rw->Write(req)` 后 `rw->Read(resp)` 等待响应（`src/control/flowsched/grpc_client.cpp:183`）
  - 若流异常：调用 `rw->Finish()`，重建 `ClientContext + Stub + Stream`（`src/control/flowsched/grpc_client.cpp:209`）

与“流切换/强制 cutoff”相关的收敛逻辑在 `ClientBase::HandleHeartbeat()`（`src/control/flowsched/client_base.cpp:203`）：

- 从 `ChannelExtensionData` 取 `table_name` 和 `can_trigger_cutoff`（`src/control/flowsched/client.h:74`，`src/control/flowsched/client_base.cpp:227`）
- 如果某 table 触发 `force_cutoff`，会被记录到 `force_cutoffs_`，后续该表对应数据会被跳过（`src/control/flowsched/client_base.cpp:238`）。

### 3.2 Statsreport：批量发送（单次 RPC）

- 线程：`GrpcClient::statsreport_reader()`（`src/control/flowsched/grpc_client.cpp:268`）
- 行为：
  - 对每个 channel 取“最新一条 statsreport”（`ChannelBase::PopLatestStatsreport()`，`src/control/flowsched/client_base.cpp:144`）
  - 逐条调用 `GrpcService::Statsreport()`（`src/control/flowsched/grpc_client.cpp:270`）
  - 若失败，返回 `SimpleResult` 错误并记录日志内容样例：
    - `flowsched api: failed to get grpc client because <...> info <...> addr <...>`
    - `flowsched api: failed to send statsreport via grpc, errmsg=<...>, info <...> addr <...>`

### 3.3 GetService：周期触发 + 结果入队

- 线程：`GrpcClient::getservice_trigger()`（`src/control/flowsched/grpc_client.cpp:322`）
- 触发间隔：`m_options.getServiceIntervalSec`（在 TM 侧配置为 2 秒，`src/runtime/taskmanager/task_manager.cpp:752`）
- 行为：
  - 对所有可用 channel 调用 GetService，构造 `GetServiceReq(jobid, taskid)`（`src/control/flowsched/grpc_client.cpp:334`）
  - 若 `channel->GetBoolGetSchedResult()` 为 false 则跳过（`src/control/flowsched/grpc_client.cpp:325`，开关定义在 `src/control/flowsched/client.h:303`）
  - 解析响应为 `proto::TaskGroups` 并 `channel->PushSchedResult(...)`（`src/control/flowsched/grpc_client.cpp:366`、`src/control/flowsched/grpc_client.cpp:400`）
  - Push 失败会 warn（日志内容样例）：
    - `push to the scheduling result queue failed, err: <...>`（`src/control/flowsched/grpc_client.cpp:401`）

上层取结果：

- `ChannelBase::GetSchedResult()`：取队列 head（可能拿到“过期”的）并返回；没有则返回 `nullopt`（`src/control/flowsched/client_base.cpp:59`）
- `ChannelBase::GetLatestSchedResult()`：丢弃旧的，只保留最后一条（`src/control/flowsched/client_base.cpp:74`）

### 3.4 GroupBy：窗口快照拉取与查询

- `ClientBase::CreateGroupBy()` 创建 `GroupBy`（`src/control/flowsched/client_base.cpp:340`），内部会周期拉取 groupby window snapshot。
- 业务侧调用 `Channel::GetGroupByResult(timestamp)` 获取 `NextTaskSet`（接口定义在 `src/control/flowsched/client.h:300`）。

## 4. 如何观测（不依赖日志文件引用）

### 4.1 观测点清单

- Heartbeat 是否持续触发：
  - 可通过周期性出现的日志内容样例 `name=flowschedHearbeat ...` 判断（`src/runtime/taskmanager/task_builder.h:95`）。
  - 若任务进入 `IsStopFlow()`，该上报会停止（`src/runtime/taskmanager/task_builder.h:90`）。

- Statsreport 是否持续触发：
  - 日志内容样例 `name=flowschedStatsReport ... availQueueLen=... queueLenCapacity=...`（`src/runtime/taskmanager/task_builder.h:118`）。
  - 指标含义：
    - `queueAvailableLen` 越小表示 backlog 越大（更拥塞）
    - `rps` 可用于判断任务吞吐是否下降（`src/runtime/taskmanager/task_builder.h:132`）

- GetService 是否持续更新：
  - 业务侧如果只关心最新调度结果，应优先使用 `GetLatestSchedResult()`（`src/control/flowsched/client_base.cpp:74`）。

### 4.2 推荐的“现场自检”顺序

1. 任务是否还在运行（是否进入 stopflow）：若 stopflow 为 true，心跳/上报会停，调度结果可能不再变化。
2. `GetService` 是否被主动禁用：检查 `channel->GetBoolGetSchedResult()`（`src/control/flowsched/client.h:304`）。
3. RingArray 是否积压：
   - `GetSchedResult()` 长期 `nullopt`：可能是上游没有 push（GetService 没成功），也可能是 channel 已被回收（use_count==1 被清理）。
4. gRPC 是否异常：关注错误日志内容是否出现类似：
   - `flowsched api: failed to get grpc client ...`
   - `flowsched api: failed to send getService via grpc ...`
   - `flowsched api: failed to send statsreport via grpc ...`

## 5. 常见问题与定位

### 5.1 “一直拿不到调度结果”（GetService 返回空）

可能原因与定位：

- `getservice_trigger` 正常运行但 channel 被回收：
  - `ClientBase::__AvailableChannels()` 会移除 `use_count()==1` 的 channel（`src/control/flowsched/client_base.cpp:376`）。
  - 现象：上层已经丢了 `shared_ptr<Channel>`，后台自然不再为它 getservice。

- gRPC 调用失败（无法拿到 stub / RPC 失败）：
  - 现象：出现日志内容样例 `flowsched api: failed to send getService via grpc ...`（`src/control/flowsched/grpc_client.cpp:351`）。
  - 处理：先确认 accessor 是否能解析地址（`GrpcClient::GetClient()`，`src/control/flowsched/grpc_client.cpp:106`），再排查网络/LB。

- 结果入队失败：
  - 现象：出现 `push to the scheduling result queue failed, err: ...`（`src/control/flowsched/grpc_client.cpp:401`）。
  - 处理：检查 `CRingArray` 容量/是否存在泄漏导致长期满。

### 5.2 “Heartbeat/Statsreport 看起来断断续续”

可能原因与定位：

- 上报线程只取最新一条：
  - Heartbeat 与 Statsreport 都是“只保留最新一条”策略（`src/control/flowsched/client_base.cpp:118`、`src/control/flowsched/client_base.cpp:144`）。
  - 当任务生成速度高于发送速度时，中间样本会被丢弃，这是设计行为。

- 任务 stopflow：
  - 一旦 `IsStopFlow()` 为 true，上报入口直接 return（`src/runtime/taskmanager/task_builder.h:90`、`src/runtime/taskmanager/task_builder.h:104`）。

### 5.3 “切流/force_cutoff 行为异常（一直切 or 不切）”

- `force_cutoff` 从 task 透传到 heartbeat（`src/runtime/taskmanager/task_builder.h:98`）。
- `ClientBase::HandleHeartbeat()` 会基于 `ChannelExtensionData.table_name` 和 `can_trigger_cutoff` 对表维度做 `force_cutoffs_` 维护（`src/control/flowsched/client_base.cpp:227`、`src/control/flowsched/client_base.cpp:259`）。
- 现象与处理：
  - 如果 table_name 缺失，`force_cutoff` 会被直接忽略（`src/control/flowsched/client_base.cpp:239`）。需要确认 channel 创建时是否填写了扩展字段。

## 6. 设计要点（便于理解“为什么是这样”）

- 观测侧“丢中间值”是预期：Heartbeat/Statsreport/GetLatestSchedResult 都有“取最新丢旧”策略，降低队列压力并避免慢消费者阻塞。
- Channel 生命周期与 `shared_ptr` 强相关：没有外部引用时会被清理，避免泄漏。
- GetService 线程固定轮询 interval：如果需要更快/更慢，需要调整 `options.getServiceIntervalSec`（`src/runtime/taskmanager/task_manager.cpp:752`）。

