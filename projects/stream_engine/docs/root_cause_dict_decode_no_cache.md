# `dict_decode no-cache` 根因分析（`tide.query.range.end` 上下文缺失）

## 1. 现象日志

本次分析直接基于如下日志：

```text
I0420 11:56:01.196583 3396330 dict_execution.cc:98] dict_execution exec-cache hit name=bmq_ies_live/dwd_live_fcdn_nss_monitor_vqos_v1/is_last_tag version=-1 epoch=1775692800 expr_len=308
I0420 11:56:01.196604 3396330 dict_execution.cc:83] ExecutionFunction::create took 34 us
I0420 11:56:01.196807 3396330 dict_execution.cc:98] dict_execution exec-cache hit name=bmq_ies_live/dwd_live_fcdn_nss_monitor_vqos_v1/local_node_name version=-1 epoch=1775692800 expr_len=88857
I0420 11:56:01.196815 3396330 dict_execution.cc:83] ExecutionFunction::create took 53 us
I0420 11:56:01.196975 3396330 dict_execution.cc:98] dict_execution exec-cache hit name=bmq_ies_live/dwd_live_fcdn_nss_monitor_vqos_v1/local_node_name version=-1 epoch=1775692800 expr_len=88897
I0420 11:56:01.196981 3396330 dict_execution.cc:83] ExecutionFunction::create took 45 us
I0420 11:56:01.197034 3396330 dict_execution.cc:98] dict_execution exec-cache hit name=bmq_ies_live/dwd_live_fcdn_nss_monitor_vqos_v1/useful version=-1 epoch=1775692800 expr_len=873
I0420 11:56:01.197038 3396330 dict_execution.cc:83] ExecutionFunction::create took 7 us
I0420 11:56:01.197055 3396330 dict_execution.cc:98] dict_execution exec-cache hit name=bmq_ies_live/dwd_live_fcdn_nss_monitor_vqos_v1/online version=-1 epoch=1775692800 expr_len=833
I0420 11:56:01.197060 3396330 dict_execution.cc:83] ExecutionFunction::create took 10 us
I0420 11:56:01.197072 3396330 dict_decode.cc:75] dict_decode cache hit name=bmq_ies_live/dwd_live_fcdn_nss_monitor_vqos_v1/local_node_name version=-1 epoch=1775692800
I0420 11:56:01.197077 3396330 dict_decode.cc:65] DecodeFuntion::create took 7 us
I0420 11:56:01.197129 3396330 dict_decode.cc:75] dict_decode cache hit name=bmq_ies_live/dwd_live_fcdn_nss_monitor_vqos_v1/remote_node_name version=-1 epoch=1775692800
I0420 11:56:01.197135 3396330 dict_decode.cc:65] DecodeFuntion::create took 6 us
I0420 11:56:01.197264 3396330 dict_execution.cc:98] dict_execution exec-cache hit name=bmq_ies_live/dwd_live_fcdn_nss_monitor_vqos_v1/is_client_first_frame_success version=-1 epoch=1775692800 expr_len=322
I0420 11:56:01.197269 3396330 dict_execution.cc:83] ExecutionFunction::create took 7 us
I0420 11:56:01.199999 3396379 manager.cc:401] [dict] Up-to-date: server X-Next-Version=3137 for column="bmq_ies_live/dwd_live_fcdn_nss_monitor_vqos_v1/local_node_name"
I0420 11:56:01.200114 3396379 dict_decode.cc:85] dict_decode no-cache name=bmq_ies_live/dwd_live_fcdn_nss_monitor_vqos_v1/local_node_name version=-1 rows=4037
I0420 11:56:01.200122 3396379 dict_decode.cc:65] DecodeFuntion::create took 6395 us
```

从日志可以确认两件事：

1. 同一个查询里同时出现了 `dict_decode cache hit` 和 `dict_decode no-cache`
2. `cache hit` 与 `no-cache` 发生在不同线程

结合 `local_dict` 的实现，这说明：

- `cache hit` 对应的执行上下文里存在有效的 `tide.query.range.end`
- `no-cache` 对应的执行上下文里没有这个值，或者值 `<= 0`

## 2. `dict_decode` 的判定规则

`dict_decode` 是否使用缓存，只取决于 `QueryConfig` 里是否带了 `tide.query.range.end`。

逻辑可以概括为：

1. `epoch > 0`
   - 走 `decodeCache()->get(epoch)`
   - 命中则打印 `cache hit`
   - 未命中则 `snapshotKeys()` 后写回 cache
2. `epoch <= 0`
   - 直接 `snapshotKeys()`
   - 打印 `dict_decode no-cache`

因此这里不是 decode cache 自身损坏，而是某条执行路径没有把 `tide.query.range.end` 带进去。

## 3. 仓库内已确认的代码事实

### 3.1 `VeloxTask` 是按“当前 stage 的局部 plan”提取 `tide.query.range.end`

`VeloxTask::Init()` 在创建主 `QueryCtx` 时，会扫描当前 task 持有的 `plan_node_ptr_`，查找该 plan 里的 `FringeDBNode`，然后从 `FringeDBNode::options()` 中读取 `fringedb.partition.max`，映射成 `dict::Repo::queryRangeEndKey()`。

对应代码：

- [VeloxTask.cpp](file:///root/Documents/stream_engine/src/runtime/taskmanager/velox/VeloxTask.cpp#L106-L124)

关键片段：

```cpp
std::unordered_map<std::string, std::string> configurationValues = {
    {velox::core::QueryConfig::kSessionTimezone, localTimeZone()},
    {velox::core::QueryConfig::kAdjustTimestampToTimezone, "true"}};
core::PlanNode::findFirstNode(plan_node_ptr_.get(), [&](auto* planNode) {
    if (auto* sourceNode = dynamic_cast<const plan::source::FringeDBNode*>(planNode)) {
        const auto& opts = sourceNode->options();
        if (auto it = opts.find("fringedb.partition.max"s); it != opts.end()) {
            configurationValues.insert({dict::Repo::queryRangeEndKey(), it->second});
        }
        return true;
    }
    return false;
});
```

这里有一个关键限制：

- `VeloxTask` 只能看到“当前 stage 的物理计划”
- 如果当前 stage 的 plan 里没有 `FringeDBNode`，这里就取不到 `fringedb.partition.max`
- 一旦取不到，就不会给 `QueryCtx` 注入 `tide.query.range.end`

这条路径与日志中的 `dict_decode cache hit ... epoch=1775692800` 是一致的，但它只对“plan 中仍然存在 `FringeDBNode` 的 stage”成立。

### 3.2 当前仓库中，显式注入 `tide.query.range.end` 的代码只有这一处

全仓库检索结果表明：

- `dict::Repo::queryRangeEndKey()` 的显式注入只出现在 `VeloxTask.cpp`
- `tide.query.range.end` 在本仓库内没有其他明确传播点

这意味着：

- 只要某个 stage 的 plan 内存在 `FringeDBNode`，该 stage 就有机会拿到该上下文
- 如果后续 stage 的 plan 不再包含 `FringeDBNode`，该 stage 就天然拿不到这个值

### 3.3 这是一个“多阶段物理计划”问题，不只是 `tide_sink` 单点问题

你 review 代码后的判断是对的：这里的关键不在于单个函数是否忘了传参，而在于整个查询会被拆成多个物理 stage。

从代码上能对上的证据是：

1. `JobRuntimeState` 会反序列化出多个 `planNodes_`
2. `QueryTimeoutConfig::parseFrom(...)` 只对 `planNodes_.front()` 执行一次
3. 然后把解析出的结果广播给所有本地 task

对应代码：

- [local_job_manager.cpp](file:///root/Documents/stream_engine/src/runtime/taskmanager/local_job_manager.cpp#L415-L439)

关键片段：

```cpp
const int64_t queryTimeoutMs =
    !planNodes_.empty() ? tide::runtime::QueryTimeoutConfig::parseFrom(planNodes_.front())
                        : tide::runtime::QueryTimeoutConfig::kDefaultQueryTimeoutMs;
setQueryTimeoutMs(queryTimeoutMs);

for (auto&& [groupIndex, localTaskId] : localTaskPairs) {
    auto task = std::make_shared<velox::VeloxTask>(planNodes_[groupIndex], errorEventEndpoint);
    task->setQueryTimeoutMs(queryTimeoutMs);
    ...
}
```

这说明仓库里已经存在一个“跨 stage 提取配置，再广播给所有 task”的公共模式。

问题在于：

- `query timeout` 走的是公共提取/广播模式
- `tide.query.range.end` 走的却是每个 `VeloxTask` 自己从局部 plan 中扫描 `FringeDBNode`

一旦物理计划拆阶段：

1. 第一阶段 plan 中存在 `FringeDBNode`
2. 后续阶段 plan 中不存在 `FringeDBNode`
3. 后续阶段的 `VeloxTask::Init()` 就无法再通过局部 plan 提取 `fringedb.partition.max`
4. 于是这些后续阶段创建的 `QueryCtx` 不带 `tide.query.range.end`

这正好能解释你观察到的现象：

- 前面的 decode 可能在第一阶段，能够 `cache hit`
- 最后的 decode 发生在后续阶段，例如 rpc sink 所在阶段
- 该阶段不含 `FringeDBNode`，于是 `dict_decode no-cache`

## 4. 为什么 `tide_sink` 仍然是最贴近现象的落点

### 4.1 `tide_sink` 是结果链路的最后物化点

在 `TideSinkOperator::addInput()` 中，Velox `RowVector` 会依次执行：

1. `flattenVector(flatten)`
2. `exportToArrow(...)`
3. 转成 `arrow::Table`
4. 再交给 `encoderFunction_` / `sinkFunction_`

对应代码：

- [tide_sink.cpp](file:///root/Documents/stream_engine/src/runtime/taskmanager/velox/plan/sink/tide_sink.cpp#L240-L280)

关键片段：

```cpp
auto flatten = std::dynamic_pointer_cast<facebook::velox::BaseVector>(input);
facebook::velox::BaseVector::flattenVector(flatten);
auto resultBatch = facebook::velox::exportToArrow(t, v, flatten, true, pool());
```

这条路径非常符合“通常最后一个 `dict_decode` 才 `no-cache`”这个现象，因为：

- 它位于查询结果真正落出 Velox 执行引擎之前
- 这里很像 lazy / deferred 结果的最终物化点
- 如果最后一个 decode 是在结果出栈时发生，那么 `tide_sink` 比 `source/fringedb.cpp` 更贴近症状

### 4.2 `tide_sink` 仍然会额外创建 encoder / sink 函数对象

`TideSinkOperator` 在构造时会根据 sink node options 创建：

- `encoderFunction_`
- `sinkFunction_`

对应代码：

- [tide_sink.h](file:///root/Documents/stream_engine/src/runtime/taskmanager/velox/plan/sink/tide_sink.h#L230-L280)

创建方式本质上是：

```cpp
std::unordered_map<std::string, std::string> decoderOptions = sinkNode->options();
...
NewOperatorFunction(..., "encoder", decoderOptions);
```

以及：

```cpp
std::unordered_map<std::string, std::string> rpcOptions = sinkNode->options();
...
NewOperatorFunction(..., functionName, rpcOptions);
```

这里的问题在于：

- `sinkNode->options()` 是 sink 节点自己的 options
- 不是 source 节点的 options
- 不会自动携带 `fringedb.partition.max`
- 也没有任何代码主动补 `tide.query.range.end`

### 4.3 `operator_func_factory.cpp` 也没有补这类上下文

`encoder` 与 `rpc` 等函数对象最终通过 `NewOperatorFunction(...)` 创建。

对应代码：

- [operator_func_factory.cpp](file:///root/Documents/stream_engine/src/runtime/taskmanager/operator_func_factory.cpp#L1600-L1620)
- [operator_func_factory.cpp](file:///root/Documents/stream_engine/src/runtime/taskmanager/operator_func_factory.cpp#L1767-L1780)

但这条工厂链路里：

- 没有 `fringedb.partition.max`
- 没有 `dict::Repo::queryRangeEndKey()`
- 没有 `tide.query.range.end`

因此，只要最后阶段的附加处理还会触发 `dict_decode`，`tide_sink` 这条路径就天然缺少必要上下文。

### 4.4 新增线程级证据：`no-cache` 线程与 `tide_sink` 链路直接对齐

新增 engine 日志里，`dict_decode no-cache` 所在线程是 `3396379`。同一个线程上还出现了如下日志：

1. `VeloxTask.cpp:224`
   - `cancel time count: 1`
2. `operator_func_factory.cpp:2252`
   - `new func by unique name : encoder`
3. `operator_func_factory.cpp:2252`
   - `new func by unique name : sink.rpc`
4. `datatable.h:286`
   - `encoder fully mode`
5. `rpc_sink.cpp:198`
   - `send rows: [0,10000)`
6. `rpc_sink.cpp:277`
   - `collect ... bytes stop false/true`

这组日志的重要性在于：

1. `dict_decode no-cache` 线程号与 `encoder` / `sink.rpc` 初始化线程号一致
2. 同一线程后续继续执行了 `rpc_sink` 的发送逻辑
3. 这说明 `3396379` 并不是一个偶然的后台线程，而是实际承担了 `tide_sink` 下沉链路的执行线程

也就是说，当前证据已经不只是“`tide_sink` 在代码结构上可疑”，而是：

```text
产生 dict_decode no-cache 的线程，和 tide_sink 的 encoder / sink.rpc / rpc_sink
执行线程已经直接对齐。
```

这会明显削弱 `source/fringedb.cpp` 作为主嫌疑点的可能性，因为：

- 如果问题主要来自 source 侧 pushdown evaluator，更合理的线程证据应当落在 source/filter 相关链路
- 而现在实际对齐到的是 sink 侧线程与 sink 侧日志

## 5. 对上一版结论的进一步修正

上一版把 `src/source/fringedb.cpp` 中的临时 evaluator 写成了主因，这个结论过早。上一轮又把问题更多归结为 `tide_sink` 单点缺参，现在看也不够完整。

重新定位后，更合理的结论是：

1. `src/source/fringedb.cpp` 仍然是一个真实存在的缺参点
2. 但它不符合“通常是最后一个 `dict_decode no-cache`”这个症状特征
3. 真正的根因类别是“多阶段物理计划下，后续 stage 无法再从局部 plan 中找到 `FringeDBNode`”
4. `tide_sink` 不是根因抽象本身，但它是当前症状最集中的落点
5. 结合新增线程日志，`dict_decode no-cache` 已经和 `tide_sink` 线程直接对齐

也就是说：

- `source/fringedb.cpp`：确认存在缺参问题，但不像主症状对应点
- `tide_sink.cpp`：是症状的主要触发点
- `VeloxTask.cpp` 按局部 plan 扫 `FringeDBNode`：才是导致后续 stage 缺参的核心设计问题

## 6. 当前更稳妥的根因表述

当前更稳妥的表述应为：

```text
dict_decode no-cache 的直接原因仍然是执行路径中缺失了
tide.query.range.end。

更深一层的设计根因是：
当前代码把 tide.query.range.end 的提取绑定在“当前 VeloxTask 所持有的局部物理 plan
里是否存在 FringeDBNode”这个条件上。

而实际查询会拆成多个物理 stage：
第一阶段可能有 FringeDBNode，后续阶段没有。
后续阶段又同样需要做 dict_decode，因此它们创建的 QueryCtx 天然缺少
tide.query.range.end。
```

进一步拆开说：

1. 第一阶段 plan 里通常有 `FringeDBNode`
2. 后续 stage 的 plan 里通常已经没有 `FringeDBNode`
3. `VeloxTask::Init()` 仍然试图从“当前 stage 的 plan”中提取 `fringedb.partition.max`
4. 所以后续 stage 的 `QueryCtx` 无法得到 `tide.query.range.end`
5. 最后一个 `dict_decode no-cache` 恰好发生在 rpc sink 所在 stage，因此线程证据对齐到了 `tide_sink`

## 7. 修复计划

### Plan 1：不要再从每个 stage 的局部 plan 临时提取 `tide.query.range.end`

目标：

- 把 `tide.query.range.end` 的来源从“局部 plan 上是否存在 `FringeDBNode`”改为“跨 stage 的公共查询配置”

建议方案：

1. 在 job 级别只解析一次 `fringedb.partition.max`
2. 将其作为跨 stage 共享配置广播给所有 `VeloxTask`
3. `VeloxTask::Init()` 不再依赖当前 stage 的 plan 中是否存在 `FringeDBNode`
4. 统一从 task 已持有的公共配置中构造 `QueryCtx`

### Plan 2：复用 `QueryTimeoutConfig` 这类公共配置模式，但需要改语义命名

当前问题的本质是：

- 仓库里已经有一个公共配置提取/广播模式：`QueryTimeoutConfig`
- 它通过 `planNodes_.front()` 提取一次，然后广播到所有 task
- 这正是 `tide.query.range.end` 需要的传播方式

建议：

1. 让 `tide.query.range.end` 也走类似 `QueryTimeoutConfig` 的公共类
2. 但 `QueryTimeoutConfig` 这个名字已经不适合继续承载更多语义
3. 应该把它升级或重命名为更通用的公共查询配置类，例如：
   - `QueryExecutionConfig`
   - `QueryRuntimeConfig`
   - `QuerySharedConfig`
4. 新类中同时承载：
   - `query timeout`
   - `tide.query.range.end`
   - 后续其他需要跨 stage 传播的查询级配置

### Plan 3：改造落点

建议修改顺序：

1. `query_timeout.h/.cpp`
   - 扩展为更通用的公共配置类
   - 或重命名成表达“查询共享配置”的名字
2. `local_job_manager.cpp`
   - 仍然基于 `planNodes_.front()` 做一次统一解析
   - 但解析结果不再只有 timeout，还包括 `range.end`
3. `TaskContext`
   - 继承或持有新的公共配置
4. `VeloxTask`
   - 初始化 `QueryCtx` 时从 task 已持有的公共配置里注入 `dict::Repo::queryRangeEndKey()`
   - 不再从 `plan_node_ptr_` 二次扫描 `FringeDBNode`

### Plan 4：`tide_sink` 相关埋点仍然保留，但角色变成验证

在新的根因判断下，`tide_sink` 埋点仍然有价值，但目的变成：

- 验证 rpc sink 所在 stage 在修复后是否已经拿到 `tide.query.range.end`
- 而不是继续把它当成唯一修复入口

建议在 `tide_sink` 增加最小埋点：

1. `TideSinkOperator::addInput()` 进入时打印：
   - thread id
   - task id
   - batch 信息
2. 创建 `encoderFunction_` / `sinkFunction_` 时打印：
   - 是否携带 `fringedb.partition.max`
   - 是否携带 `tide.query.range.end`
3. 给 sink 路径增加固定标识，例如：
   - `tide_sink_export`
   - `tide_sink_encoder`
   - `tide_sink_rpc`

这样可以直接验证修复后，rpc sink 所在 stage 是否已经拥有正确的 query-range-end。

### Plan 5：保留 `source/fringedb.cpp` 的修复，但下调优先级

`src/source/fringedb.cpp` 中默认 `QueryCtx::create()` 的问题仍然应该修，但在当前这个问题里更像：

- 明确存在的代码缺陷
- 次要修复项
- 不是最贴合“最后一个 dict decode no-cache”现象的主嫌疑点

建议顺序：

1. 先把公共配置类抽象好
2. 再让所有 stage 统一复用
3. 然后补 `source/fringedb.cpp`

## 8. 验证计划

### 8.1 代码级验证

确认以下路径都能拿到相同的 query-range-end：

1. 第一阶段 `VeloxTask`
2. 后续 rpc sink 所在阶段 `VeloxTask`
3. `tide_sink` 的后置 encoder / sink 链路
4. `source/fringedb.cpp` 的临时 evaluator

### 8.2 运行时验证

重点观察日志是否从：

1. 前几个 `dict_decode cache hit`
2. 最后一个 `dict_decode no-cache`

变成：

- 统一带 epoch 的 `cache hit` / `cache miss`
- 不再出现“最后一个是 `no-cache`”的特征

如果修复后，这个现象消失，就可以把根因基本锁定为：

- 多阶段物理计划下，后续 stage 没有继承 `tide.query.range.end`

## 9. 结论

这次重新定位后，更合理的判断是：

1. 根因类型仍然是 `tide.query.range.end` 缺失
2. 更深一层的设计根因是：按 stage 局部 plan 提取该配置
3. 多阶段物理计划下，后续 stage 不再含 `FringeDBNode`
4. 所以后续 stage 创建的 `QueryCtx` 无法得到 `tide.query.range.end`
5. rpc sink 所在阶段正是这个缺参问题的主要症状落点

理由有三点：

1. `VeloxTask.cpp` 当前按局部 plan 扫 `FringeDBNode`
2. `local_job_manager.cpp` 已经证明仓库里存在“跨 stage 公共配置”的成熟模式
3. rpc sink 所在线程与 `dict_decode no-cache` 线程已直接对齐
4. 这说明问题不是单个 sink 函数忘了传参，而是后续 stage 本身没有继承公共配置

因此，正确修复方向不应只围绕 `tide_sink` 打补丁，而应把 `tide.query.range.end` 升级为跨 stage 的公共查询配置，并通过类似 `QueryTimeoutConfig` 的公共类统一传播。若继续使用 `QueryTimeoutConfig` 这个类，则必须修改其语义命名，使其能准确表达“共享查询配置”而不只是 timeout。
