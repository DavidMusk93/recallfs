# Learn from Tests: Consumer V2 Ring Slot Leak

## 暴露的 Bug

`MsgSlotRing` FIFO head-of-line blocking：rebalance revoke partition 后，in-flight slots（Dispatched 状态）和 buffered slots（Filled 状态）永远不释放，ring 打满后 `acquire()` 返回 nullptr，poll loop 停止，系统卡死。

## 为什么之前的测试没有发现

| 测试特征 | 之前的测试 | 暴露 bug 的测试 |
|----------|-----------|----------------|
| Worker 数量 | 1 | 8（含 1 个 slow worker） |
| Partition 数量 | 1-4 | 100 |
| Rebalance 注入 | 无 | 每 5s 一次，revoke 25% |
| 运行时长 | 瞬时 | 60s 持续 |
| Ring 容量压力 | 远未达到上限 | 高吞吐 + revoke 触发泄漏 |

**核心问题**：简单测试只验证了 happy path，没有验证「故障路径下的资源回收」。

## 开发工作流的问题

### 1. 测试覆盖的维度缺失

单测验证了 dispatch 语义正确性，但缺少：
- **资源生命周期验证**：slot 分配/回收的守恒断言
- **故障注入下的状态机完整性**：revoke 发生在不同阶段（Filled、Dispatched、Done）时的行为
- **长时间运行下的累积效应**：泄漏在短测试中不可见

### 2. 先功能后防御的顺序错误

正确顺序应该是：

```
功能实现 → 资源守恒断言 → 故障注入测试 → 性能压测
```

实际顺序是：

```
功能实现 → 简单 e2e → 性能压测 → 发现吞吐问题 → 批处理重构 → 再压测 → 终于发现资源泄漏
```

### 3. Ring buffer 的 FIFO reclaim 设计隐含了强假设

`reclaim()` 假设 head slot 总能及时变为 Done。这个假设在以下场景下被打破：
- Rebalance revoke 了 partition，但 slot 的所有权已经转移给 worker
- Worker 持有 batch 但 partition 已不存在，ack 失败，slot 卡在 Dispatched

**教训**：环形缓冲区如果使用严格 FIFO 回收策略，必须保证任何异常路径都能推进 head。

## 修复方案

Revoke 时立即清理所有相关 slots：

1. **In-flight slots**（已 dispatch 给 worker 的 batch）：在 `revokePartitionsLocked` 中调用 `cleanupDispatchItemSlotsLocked` 立即 markDone + reclaim
2. **Buffered slots**（在 partition buffer 中等待 dispatch 的）：`dispatchState_.revokePartition()` 返回被遗弃的 slot 列表，调用方执行 markDispatched → markDone → reclaim
3. **ABA 安全**：worker 后续 ack 已 revoked 的 batch 时，不再二次 cleanup

## 未来测试规范

新增任何涉及资源池（ring buffer、connection pool、lease map）的功能时，必须包含：

1. **守恒断言**：测试结束时 `liveCount == 0`（或已知的 in-flight 数量）
2. **故障路径覆盖**：在资源「借出」状态下触发 revoke/close/timeout，验证资源归还
3. **累积泄漏检测**：循环执行 assign → consume → revoke N 次，断言 liveCount 不单调递增
4. **慢路径模拟**：至少一个 worker 有延迟，放大 head-of-line blocking 窗口

## 性能对比

| 阶段 | 吞吐 | Ring 状态 |
|------|------|-----------|
| 修复前（有 rebalance） | 13,435 msg/s → 0（卡死） | 262144/262144（100%） |
| 修复后（有 rebalance） | 38,982 msg/s（稳定） | peak 14,763（5.6%） |
| 无 rebalance 基准 | ~96,211 msg/s | 正常波动 |
