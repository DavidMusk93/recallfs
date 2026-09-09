# 分布式 Data Pipeline Completeness 研究

> 原文：[How we measure data completeness at scale](https://www.datadoghq.com/blog/engineering/data-pipeline-completeness/)
>
> 研究与复现日期：2026-09-09。本文严格区分 Datadog 的公开陈述、数学推导与本地可执行证据。

## 1. 结论先行

这篇文章最值得沉淀的架构思想是：

> Completeness 不是某个 counter 的属性，而是特定 topology version 下，一个
> cohort 的显式 delivery obligations 是否已经履行。

Datadog 的 segment model、create/ack evidence、failure-independent
observer、dynamic topology 与 decision gating，共同构成了一套适用于大型
distributed pipeline 的 correctness control plane。本研究通过本地模型复现了
核心 idempotency 机制，并证明了文章未展开的若干工程合同为什么不可缺失。

但这篇文章不能直接作为完整设计照搬。公开内容没有定义 statistical
estimator、confidence bounds、late bucket finalization、transformation
cardinality 和 measurement-health protocol。缺少这些合同，一个看似合理的
completeness percentage 可能在数学上无效，在操作上也不安全。

## 2. 证据摘要

| 结论 | 状态 | 证据 |
| --- | --- | --- |
| create/ack 在重复与乱序下可以收敛 | 已复现 | 穷举长度不超过 8 的全部 event sequence |
| root ingress bucket 必须沿链路继承 | 已复现 | 跨 bucket 的 create 与 ack 不会错误匹配 |
| sequential ratio 相乘需要 cohort conservation | 已推导并复现 | 合法链路 $100 \to 90 \to 81 \to 72$；不守恒链路被拒绝 |
| volume weighting 会掩盖低流量 required branch 故障 | 已复现 | delivery mass 为 $0.99$，required floor 为 $0$ |
| create 与 ack 必须共享 sampling decision | 已复现 | consistent sampling 得到 $1$；independent sampling 得到 $0.128483$ |
| automation 必须检查 measurement health | 已复现 | invalid、stale、undersampled 与 unknown-topology signal 均 fail closed |
| custom storage 在 Datadog 规模下成本更低 | 仅为原文陈述 | 没有公开实现或 benchmark |
| 故障通常在一分钟内被发现 | 仅为原文陈述 | 没有公开 incident-level measurements |

## 3. 值得沉淀的架构

文章将一个无边界的 end-to-end 问题拆成四个有边界的层次：

```text
payload context
      |
      v
segment evidence -----> time-bucketed evidence store
      |                            |
      v                            v
topology graph --------> path completeness query
                                   |
                                   v
                         human or automation gate
```

### 3.1 Local evidence

每个 segment 都为稳定的 payload ID 产生 create 与 acknowledgment evidence。
最小 evidence identity 为
`(root_bucket, payload_id, segment_id)`。

`root_bucket` 由可信的 intake clock 生成，并沿整条 pipeline 继承。如果每个
hop 都按照本地 arrival time 重新分桶，延迟到达的 create 与 acknowledgment
会被放入不同 bucket，从而制造并不存在的 loss。

紧凑状态机如下：

| Stored state | Create seen | Ack seen | 含义 |
| --- | --- | --- | --- |
| `Created` | yes | no | 已进入 segment，尚未观测到离开 |
| `AckBeforeCreate` | no | yes | 乱序 evidence，等待 create |
| `Complete` | yes | yes | 两项 evidence 均已观测 |

重复 event 是 no-op；acknowledgment-before-create 不能被丢弃。由此可以在不引入
distributed coordination 的前提下，得到与 delivery order 无关的局部 evidence。

### 3.2 Topology evidence

Topology 必须与 payload evidence 分开保存，并带有时间版本。route、ownership 或
branch policy 发生变化后，当前 service graph 无法正确解释历史 bucket。

这个 graph 是 operational evidence，不只是 visualization。它负责提供：

- 某个 time bucket 对应的合法 path；
- degraded segment 的 owner；
- branch semantics 与 expected successor count；
- 解释 scalar result 所需的上下文；
- 从 detection 到 paging 和 automated mitigation 的路由。

### 3.3 Decision gating

最值得迁移的产品模式，是把 data quality 与查询结果一起返回，并由 consumer
执行 fail-closed 判断。HTTP 请求成功，不代表返回的 metrics 足以支撑
autoscaling decision。

因此，completeness result 应该是 typed envelope，而不是一个 `float`：

```text
CompletenessEvidence
  value
  expected_obligations
  acknowledged_obligations
  sample_count
  inclusion_probability
  confidence_interval
  root_bucket
  observed_at
  age
  topology_version
  observer_health
  invalidation_state
```

## 4. Sequential Composition 缺失的证明义务

对第 $i$ 个 segment，定义：

- $E_i$：一个 root cohort 中的 unique creates；
- $A_i$：同一 cohort 中匹配成功的 acknowledgments；
- $c_i$：该 segment 的 completeness ratio。

$$
c_i = \frac{A_i}{E_i}
$$

原文使用乘法合并 sequential ratios。该运算成立的前提，是相邻 segment
确实守恒同一个 cohort：

$$
E_{i+1} = A_i
$$

在此前提下，中间项可以约去：

$$
\begin{aligned}
\prod_{i=1}^{n} c_i
&= \frac{A_1}{E_1}
   \cdot \frac{A_2}{E_2}
   \cdots
   \frac{A_n}{E_n} \\
&= \frac{A_n}{E_1}.
\end{aligned}
$$

Demo 验证的链路是：

$$
100
\xrightarrow{90\%}
90
\xrightarrow{90\%}
81
\xrightarrow{\frac{8}{9}\approx 88.89\%}
72
$$

因此 end-to-end completeness 为：

$$
\frac{72}{100} = 0.72
$$

表面相似的两个 segment $(100, 90)$、$(100, 90)$ 不能据此证明 conservation。
如果机械地计算：

$$
0.9 \times 0.9 = 0.81
$$

就会产生一个没有 end-to-end 含义的数值，因为第二个 denominator 可能来自完全
不同的 population。正确行为是拒绝 composition，而不是返回一个看似合理的结果。

因此，仅比较 count 不够。生产系统必须携带 root cohort 或 lineage identity，
避免不同集合在 count 恰好相等时通过校验。

## 5. 从 Payload 推广到 Delivery Obligation

`ack/create` ratio 默认每个 create 恰好对应一个 acknowledgment。真实 pipeline
经常不满足这一假设：

| Transformation | 必需合同 |
| --- | --- |
| Filter | predicate 决定产生 0 个或 1 个 successor obligation |
| Fan-out | 一个 input 产生已知集合或数量的 successor obligations |
| Aggregation | 一组 inputs 产生一个带 group lineage 的 output obligation |
| Join | output obligation 同时依赖两个 input cohorts 与 join policy |
| Replay | replay generation 不能与原始 obligation 冲突 |
| Optional sink | policy 显式标记 optional obligation，不能只靠低权重掩盖 |

因此，更通用的 conservation unit 是 delivery obligation：

```text
root_obligation_id
root_bucket
topology_version
predecessor_id
successor_id
expected_successor_count
segment_id
sampling_randomness
inclusion_probability
```

这实际上是一份带显式状态继承的 evidence ledger。payload-preserving segment
只是其中最简单的 one-to-one 情形；同一个模型还能表达 split、merge、filter
与 replay。

## 6. Branch 必须声明 Aggregation Semantics

原文使用 volume-weighted average 合并 parallel branches。这个结果回答的是：

> 预期 delivery mass 中有多少已经被 acknowledged？

它不一定回答：

> 有多少 source payload 到达了所有 required sinks？

Demo 的 counterexample 包含 990 个成功的 optional obligations，以及 10 个完全
失败的 required obligations：

$$
C_{\mathrm{mass}}
= \frac{990 + 0}{990 + 10}
= 0.99
$$

$$
C_{\mathrm{required}}
= \min\left(\frac{0}{10}\right)
= 0
$$

单独展示 $99\%$ 会掩盖 required branch 的完全失败。常见 DAG policy 包括：

| Policy | 含义 |
| --- | --- |
| Weighted mass | 所有 expected deliveries 中已观测到的比例 |
| Required floor | 最差 required branch 的 completeness |
| All-required | 所有 required obligations 均完成的 root 比例 |
| Per-sink vector | 为每个 consumer 独立保留 completeness |

应该由 consumer 的决策语义选择 policy，而不是由 aggregation implementation
擅自决定。alerting 与 autoscaling 通常需要 required-sink rule，而不是全局
weighted average。

## 7. Sampling 是 Correctness 的一部分

独立采样 create 与 acknowledgment 会破坏 pairing。本地 deterministic
experiment 中，底层 pipeline 实际 acknowledged 了全部 payload：

| Sampling mode | Creates | Measured completeness | Ack without create |
| --- | ---: | ---: | ---: |
| Same propagated seed | 2,087 | 1.000000 | 0 |
| Independent seeds | 1,938 | 0.128483 | 1,861 |

第二行的 incomplete result 完全由 observer 制造。

可靠的 adaptive sampling contract 至少需要：

1. 为整个 obligation 继承 stable randomness 或 propagated decision；
2. sampled evidence 必须携带 effective inclusion probability；
3. probability 不同时使用 inverse-probability weighting；
4. 为低流量 customer 提供 confidence interval 或 error bound；
5. 明确定义 in-flight cohort 中 probability 变化的处理方式；
6. 处理 bucket 结束时尚未附着到下一条 sample 的 accumulated weight；
7. 单独统计 observer 丢弃的 tracking traffic。

OpenTelemetry 的 consistent probability sampling 值得参考，因为它传播共同的
randomness 与 effective threshold。其 adjusted count 为 sampling probability
的倒数：

$$
\mathrm{adjusted\ count} = \frac{1}{p}
$$

这是一种可复用的 consistency contract，但不能据此推断 Datadog 使用了相同算法。

## 8. Watermark 与 Completeness 回答不同问题

原文指出，任意延迟使 watermark 无法提供其所需保证。Apache Flink 文档同样明确：
late event 可以在 watermark 之后到达，而有限等待会限制 determinism。

正确结论不是“watermark 没有用”，而是这些机制回答不同问题：

| Mechanism | 回答的问题 |
| --- | --- |
| Watermark | event-time processing 被认为推进到了哪里 |
| Allowed lateness | result 保持可修正状态多长时间 |
| Create/ack evidence | 哪些声明过的 delivery obligations 已履行 |
| Retention/finalization | 未履行 obligation 何时变为 loss、expired 或 unknown |

Create/ack tracking 仍然需要 finalization rule。Datadog 表示 state 只保留数小时，
同时 customer data 可以任意延迟。由于 `root_bucket` 在进入 intake 后才生成，
pre-intake delay 不属于 retention 问题；但 payload 一旦被接受，晚于 eviction
才到达的 acknowledgment 就无法修复旧 state，除非系统另有 late-correction path。

最终结果至少应区分：

- latency objective 内的 pending；
- late 但仍可修正；
- finalization 后永久 missing；
- evidence 已过期导致的 unknown；
- observer 不健康导致的 invalid。

## 9. Observer 不能与故障 Fate-share

Datadog 最强的 operational principle 是：

> Completeness system 必须比它所观测的 outage 活得更久。

文章通过 direct intake/storage、minimal external dependencies、product tracks、
双 availability zones、不同 partitioning scheme、partition-aware shedding 与
manual invalidation 实现这一原则。

它本质上是一项 fate-sharing analysis：

| Potential common cause | Required control |
| --- | --- |
| Monitored Kafka failure | observer transport 不依赖该 Kafka |
| Hot customer | per-partition shedding 与不同 replica sharding |
| Product traffic surge | 独立 product tracks |
| Deployment defect | replica 间 staggered versions |
| Observer overload | observer-health signal 与 scoped invalidation |
| Client-library 或 schema defect | version visibility、canary 与 independent validation |
| Regional 或 control-plane fault | 显式接受边界，或增加隔离层级 |

仅仅声明“没有 external dependencies”并不充分。共享 instrumentation code、
schema、control bulletin、credential 和 region 仍然会产生 correlated failure。

## 10. 对现有 Stream 工作的启示

现有 stream-engine 笔记已经区分：

- `highWatermarkOffset`；
- `ackedOffset`；
- `brokerCommittedOffset`。

对应的 lag 定义可以写为：

$$
\begin{aligned}
\mathrm{brokerLag}
&= \mathrm{highWatermarkOffset}
 - \mathrm{brokerCommittedOffset}, \\
\mathrm{ackedLag}
&= \mathrm{highWatermarkOffset}
 - \mathrm{ackedOffset}, \\
\mathrm{commitGap}
&= \mathrm{ackedOffset}
 - \mathrm{brokerCommittedOffset}.
\end{aligned}
$$

这是一套可靠的 linear progress model，避免把 broker-visible committed offset
误认为本地已经处理的数据。Datadog pattern 将同样的口径纪律扩展到了跨 service
boundary 与 branching topology。

只有当系统确实需要回答下列 end-to-end 问题时，才值得引入这套机制：

- 哪些 accepted records 到达了最终 serving 或 storage boundary？
- 哪个 segment 对 missing obligations 负责？
- 该结果对当前 customer、partition 与 topology version 是否有效？
- automated consumer 是否可以基于这些数据采取行动？

如果没有这些明确问题，就给每条 record 增加 evidence，只会制造成本，而得不到
可辩护的 semantic contract。

## 11. 推荐的 Engineering Contract

生产设计应强制满足以下 invariants：

1. **Stable identity：** root obligation ID 与 bucket 必须跨 hop 继承。
2. **Idempotent evidence：** duplicate 与 reordered events 必须收敛。
3. **Explicit cardinality：** 每种 transformation 都要声明 successor
   obligations。
4. **Cohort-safe composition：** 非法 topology 或 count join 必须返回 unknown，
   不能返回貌似合理的 scalar。
5. **Consistent sampling：** selection identity 与 probability 必须继承。
6. **Statistical honesty：** estimate、sample size 与 uncertainty 必须一起传递。
7. **Independent health：** measurement failure 必须与 data failure 可区分。
8. **Versioned topology：** 历史 evidence 必须使用历史 routing 解释。
9. **Consumer policy：** weighted mass、required sink、freshness 与 fallback
   behavior 必须显式定义。
10. **Fail-closed automation：** stale、invalid、undersampled 或无法解释的
    evidence 不得授权 automated action。

## 12. 交付物

- `source.md`：来源 metadata、reference 与 evidence limitation。
- `exploration.md`：hypothesis、test-first 路径、counterexample 与 toolchain
  investigation。
- `demo/`：C++20 reference model、scenario program 与 14 项测试。
- `evidence/`：red test、sanitized test output、scenario output、environment
  与 source digests。
- `learning/sources/20260909-data-pipeline-completeness.md`：结构化 source
  archive。

构建与运行命令见 `demo/README.md`。
