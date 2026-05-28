# PartitionConsumer Queue 改造 Plan

> 历史说明
>
> 这份文档保留为历史提案入口。
>
> 当前围绕 `TableEventListener` / `PartitionTableEventConsumer` 的新提案，
> 已迁移到：
>
> - `docs/stream/table-event-listener.proposal.md`
>
> 新文档将“提案”和“开发计划”合并在同一份文档中，并补充了两项新的主线：
>
> - 以总行数而不是 table 数量限制 queue 大小
> - 为新 consumer 增加 latest window 预热能力

## 文档信息

| 字段 | 内容 |
| --- | --- |
| Topic | `partition-consumer` |
| Kind | `proposal` |
| Status | `draft` |
| 目标 | 供 review 使用，暂不落代码 |

## 1. 背景

当前 `PartitionConsumer` 使用 `folly::ProducerConsumerQueue<TableEvent>` 作为单生产者 / 单消费者的无锁队列。

相关位置：

- `src/fringedb/detail/iterator/stream.h`
- `src/fringedb/detail/iterator/stream.cc`

当前 `onTableEvent()` 的核心逻辑非常简单：

```cpp
void PartitionConsumer::onTableEvent(TableEvent event) noexcept {
  event_max_index_.store(event.max, std::memory_order_relaxed);
  event_queue_.write(std::move(event));
}
```

也就是说：

- producer 尝试把新事件写入队列
- 如果 `write()` 成功，则消费者未来可以从队列里命中这个新 window
- 如果 `write()` 失败，则直接放弃这次 push

这份 plan 只讨论这里的队列语义，不展开到更大的 L0 / TableLifeCycle 架构。

## 2. Queue 的设计初衷

### 2.1 初衷

这个 queue 的设计初衷是：

- 给 `StreamIterator` 提供一个低延迟的内存快路径
- 让消费者优先消费最近写入、且索引连续的 table
- 尽量降低读盘概率
- 通过无锁 SPSC 队列控制热路径开销

### 2.2 这个 queue 实际承担的角色

从语义上看，它不是一个“必须完整保存历史”的日志队列，而是一个：

- **供消费者命中最近连续 window 的机会性缓存**

它更像：

- recent window cache

而不是：

- durable history queue

### 2.3 消费侧对 queue 的依赖

`consume()` 的逻辑决定了这个 queue 的作用方式：

1. 如果头部事件已经落后于当前读位置，则 `popFront()`
2. 如果当前读位置早于头部事件起点，则直接 miss
3. 只有当 `current_read_index` 落在头部事件覆盖区间内时，才真正命中并消费

所以这个 queue 的价值，本质上取决于：

- 队列中是否保留了“最新且仍有命中概率”的 window

## 3. 当前面临的问题

### 3.1 问题本质

当 queue 满时，当前逻辑是：

- `write()` 失败
- 放弃这次 push
- 原队列内容保持不变

这会带来一个关键后果：

- 队列继续维持对一段**历史连续 table** 的引用
- 而不是向前滑动到**最新 window**

### 3.2 为什么这会放大内存问题

在消费 lag 容易发生、且 lagging consumer 数量较多时：

- 每个消费者各自维护一个本地 `event_queue_`
- 当新的 event 进不来时，本地 queue 保留的还是旧 window
- 消费者继续引用这段历史 table
- 多个 lagging consumer 就会同时维持多段历史 table 的引用

最终效果是：

- 不是“最近数据被缓存”
- 而是“历史连续段被保活”

这和原始设计目标相反。

### 3.3 问题可被简化为一句话

当前 queue full 语义是：

- **drop newest, keep oldest**

而更符合目标的语义应该是：

- **drop oldest, keep newest**

## 4. 改动契机

这次改动的契机很明确：

- 线上存在消费作业数量较多、并且 lag 容易发生的场景
- 当前 `write` 失败后的语义已经不再只是“少一次内存命中”
- 而是会把 queue 固化成“历史段引用保持器”

从收益角度看，这次改动有两个直接价值：

1. 让 queue 更符合最初设计目标
   - 保留最近 window，而不是保留历史段
2. 让内存中的 table 生命周期更短
   - 尽快淘汰旧引用，减轻堆积

## 5. 目标语义

### 5.1 我们真正想要的行为

当 queue 满时，希望 producer 仍然尽可能把**最新 event** 放进去。

也就是说，queue 需要始终尽量代表：

- 当前 partition 的**最新 window**

而不是：

- 当前消费者尚未消费掉的一段历史连续 window

### 5.2 目标效果

如果某个 consumer lag 太多：

- 它会更快 miss 掉内存 window
- 更早回退到其他路径
- 而不是继续拿住历史 queue 中的旧 table

这实际上是一种更健康的退化行为：

- **牺牲 lagging consumer 的 L0 命中率，换取整体内存可控**

## 6. 推荐改动方向

## 6.1 语义调整

把 queue full 时的策略，从：

- `push fail -> 放弃新 event`

调整为：

- `push fail -> 先淘汰最旧 event -> 再确保新 event 进入 queue`

这意味着队列整体会向前滑动，持续逼近最新 window。

### 6.2 语义上的预期结果

调整后：

- lagging consumer 不再长期引用历史连续段
- queue 中的 table 更接近 head
- 历史 table 更早失去引用

### 6.3 为什么这算“优雅改动”

因为它没有改变 queue 的根本职责：

- 仍然是 SPSC
- 仍然是 in-memory fast-path
- 仍然是机会性命中机制

改变的只是：

- queue 满时到底优先保护“旧数据”还是“新数据”

这属于**语义修正**，不是架构翻新。

## 7. 改动注意事项

### 7.1 最大注意点：不要直接破坏 SPSC 并发前提

这是这次改动里最需要审慎对待的点。

当前容器是：

- `folly::ProducerConsumerQueue`

它的前提是：

- 单 producer 写
- 单 consumer 读 / pop

而当前实现里：

- `consume()` 会 `popFront()`

如果改成 producer 在 `onTableEvent()` 中也直接 `popFront()`：

- 就会形成 producer 和 consumer 同时操作 head
- 这会破坏当前容器的并发使用前提

所以：

- **语义上应该“淘汰最旧 event 保留最新 window”**
- **实现上不能粗暴地让 producer 直接并发 `popFront()`**

### 7.2 需要明确的实现边界

review 时需要优先确认以下问题：

1. 是否继续坚持使用 `folly::ProducerConsumerQueue`
2. 如果继续使用，full-path 是否允许引入一个极轻量的慢路径保护
3. 如果不继续使用，是否接受替换为更适合“producer 侧淘汰头部”的 ring buffer

### 7.3 `event_max_index_` 的语义也要顺手校正

当前代码是先：

1. `store(event.max)`
2. 再 `write()`

这会导致：

- 即使新 event 没进 queue，watermark 也可能已经前移

如果这次调整要确保“新 window 尽量入队”，建议同时明确：

- `event_max_index_` 表示“最近观察到的 event”
- 还是“成功入队的最大 index”

建议 review 时一并定语义。

### 7.4 文档和注释要一起改

当前头文件里的注释明确写的是：

- queue 满时丢 newest，以保连续性

如果我们决定改语义，这些注释必须同步调整，否则会出现：

- 代码语义和注释相反

## 8. 建议的改动方案分层

### 方案 A：最小语义修正

目标：

- 不改大框架
- 只把 queue full 语义从“保旧丢新”改成“保新丢旧”

特点：

- 改动面小
- 能最快止住历史段堆积问题
- 需要谨慎处理 `SPSC` 并发边界

适合：

- 先做一轮控制风险的修正

### Plan B：weak_ptr 降级引用（默认实现）

目标：

- **不更改队列数据结构**（仍然使用 `folly::ProducerConsumerQueue`）
- 通过 `weak_ptr` 让消费者不再“强引用”历史 table，从根上避免 lag 场景下的 table 堆积
- 保持 `onTableEvent()/consume()` 的接口形态基本不变，降低改动风险

核心思路：

- `onTableEvent()` 收到 `TableEvent{shared_ptr<table>, min, max}` 后：
  - 仍然把 `event.max` 写入 `event_max_index_`（producer watermark）
  - 向队列写入的不是强引用 table，而是 `weak_ptr<table>` + `min/max`
- `consume()` 命中时：
  - 先 `weak_ptr.lock()` 尝试提升为 `shared_ptr`
  - 如果提升失败（table 已被系统其他路径释放），则：
    - 弹出该 event
    - 返回 nullptr（触发上层回退读盘/其他路径），不推进 `current_read_index`

效果与取舍：

- 优点：
  - 解决“多 lagging consumer 各自保活多段历史 table”的核心内存问题
  - 不需要让 producer 去 pop 旧事件，从而避免破坏 SPSC 的并发前提
  - 实现简单，工程风险低，适合作为默认实现
- 代价：
  - 对 lagging consumer 来说，内存命中率会下降（table 可能已过期），更早回退到慢路径
  - 需要明确：Plan B 解决的是“引用保活导致的内存堆积”，不是“尽量保连续命中”

适用场景：

- 消费任务数较多、lag 易发、内存风险优先于内存命中率的场景

### 方案 B：语义修正 + watermark 修正

在方案 A 基础上，再做：

- `event_max_index_` 语义校正
- 增加 drop / eviction 相关指标

特点：

- 可观测性更好
- 后续更容易分析效果

适合：

- 想在第一版改动里同时把行为看清楚

### 方案 C：队列实现一起调整

目标：

- 明确支持“producer 侧淘汰最旧 + consumer 侧读取最新窗口”的实现

特点：

- 语义最干净
- 实现成本更高
- 需要更仔细的并发验证

适合：

- 如果确认当前 `folly::ProducerConsumerQueue` 已经不适合承载这个目标语义

## 9. 我建议的 review 结论方向

如果只从当前问题出发，我建议 review 优先达成以下共识：

1. `event_queue_` 的设计目标是“维持最新 window”
2. queue 满时应该优先保新，不应该优先保旧
3. lagging consumer 更早 miss L0 是可接受、甚至更合理的退化
4. 实现时必须尊重当前 SPSC 队列的并发约束
5. `event_max_index_` 的语义要和新行为保持一致

## 10. 建议的下一步

等你 review 之后，下一步我建议按下面顺序推进：

1. 先确认目标语义
   - queue full 时是否明确切换为 `keep newest window`
2. 再确认实现策略
   - 保留当前队列，还是替换容器
3. 再落代码
   - `onTableEvent()`
   - 注释
   - 观测指标
   - 单元测试

## 11. Review Checklist

为了便于你 review，我把这次 plan 的关键判断收敛成几个问题：

1. 你是否同意 queue 的角色应定义为“最新 window cache”，而不是“历史连续段缓存”？
2. 你是否同意 queue full 时应该优先保最新 event？
3. 你是否接受 lagging consumer 更快 miss L0、转而读盘？
4. 你是否希望第一版只做最小语义修正，还是同步处理 `event_max_index_` 与 metrics？
5. 你是否希望保留 `folly::ProducerConsumerQueue`，还是愿意接受替换容器？

这几个点定下来后，代码修改会非常顺。
