---
doc_id: recallfs-study-whale-agent-harness-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260914-whale-agent-harness
depends_on:
  - learning/studies/20260914-whale-agent-harness/source.md
supersedes: []
verified_by:
  - paper claim and table cross-check
  - author-code revision inspection
  - browser-validated HTML explainer
---

# WHALE：从提示词优化到 Agent Harness 工程

> 主文：Kim 等，[WHALE: A Simple Recipe for Joint Harness-Weight
> Optimization](../../sources/20260914-whale-joint-harness-weight-v1.pdf)，
> arXiv `2609.00196v1`。
>
> 浏览器版学习页：
> [`report.html`](report.html)。
>
> 本文严格区分论文报告、代码库可见事实与 RecallFS 工程推论。

## 1. 结论先行

WHALE 最重要的贡献不是某个新 prompt，而是把 Agent 的性能对象定义正确了：

> **Agent 能力是模型权重与可执行 harness 的联合产物。**

Harness 不只是 system prompt。它还包括：

- 哪些观察进入上下文；
- 工具 schema 与调用方式；
- 工具结果如何裁剪和重排；
- parser、执行器和错误反馈；
- retry、turn budget 与终止策略。

因此，面对 Agent 失败时，不应立即换模型或继续堆 prompt。先判断瓶颈属于
`model` 还是 `harness`，然后固定另一侧，小步修改并用独立 oracle 验收。

对普通 Agent 用户，模型权重通常不可训练。论文仍然给出可直接采用的方法：

1. 将 prompt、context、tools、control flow、error handling 和 termination
   作为一个完整 harness 管理。
2. 建立可重复的任务集和验收器，而不是凭一次对话的主观感受优化。
3. 每轮只改变一个机制，保留旧版本并记录轨迹。
4. 达到最小样本量后，连续若干轮没有改善就停止。
5. 当 harness 已充分、错误仍稳定存在时，再升级模型或训练权重。

这可以形成可复用规范。RecallFS 将它沉淀为
[`Agent Interaction and Harness Optimization Contract`](../../../designs/agent-interaction-harness.md)。
可复用对象是**任务合同 + harness 合同 + 评测与迭代协议**，不是万能提示词。

## 2. 论文的问题定义

论文将模型参数记为 $\theta$，可执行 harness 记为 $h$。二者共同诱导任务
$x$ 上的轨迹分布 $\pi_{\theta,h}(\tau \mid x)$，verifier
$R(x,\tau)$ 将轨迹映射为奖励：

$$
J(\theta,h)
=
\mathbb{E}_{x \sim \mathcal{P}}
\mathbb{E}_{\tau \sim \pi_{\theta,h}(\cdot \mid x)}
[R(x,\tau)].
$$

目标不是单独找到“最强模型”或“最好 prompt”，而是找到联合最优的
$(\theta^\*, h^\*)$。

这一区分很重要。以下问题都可能表现为“模型答错”，但根因不相同：

| 表象 | 可能的模型瓶颈 | 可能的 harness 瓶颈 |
| --- | --- | --- |
| 没找到证据 | 不会构造检索意图 | query 被错误改写、top-k 太小、结果被截断 |
| 工具调用失败 | 不理解工具语义 | schema 不一致、parser 脆弱、错误反馈不可操作 |
| 输出格式错误 | 无法稳定遵循约束 | 最终轮没有强制收口、终止条件过早 |
| 长推理被截断 | 推理冗长或无法压缩 | token/turn budget 不当、上下文未压缩 |
| 重复失败 | 无法利用反馈 | retry 仍返回同样信息、状态没有进入下一轮 |

只看最终答案无法定位瓶颈。必须保存轨迹并把失败分解到 context、tool、
parser、execution、feedback 和 termination。

## 3. WHALE 如何工作

WHALE 采用交替条件优化：

$$
\theta_{k+1}
=
\operatorname{ModelUpdate}(\theta_k;h_k,D_{\text{weight}})
$$

$$
h_{k+1}
=
\operatorname{HarnessSearch}(h_k;\theta_{k+1},D_{\text{harness}})
$$

```text
fixed harness h[k]
        |
        v
small weight update
        |
        v
fixed model theta[k+1]
        |
        v
bounded harness search
        |
        v
score candidates with verifier
        |
        +---- improve ----> accept h[k+1] ----+
        |                                     |
        +---- no gain ----> keep h[k] --------+
                                              |
                                              v
                                         next cycle
```

### 3.1 权重阶段

论文用 online rejection-sampling fine-tuning：

1. 当前模型和固定 harness 对一批问题生成 $G=8$ 条轨迹；
2. binary verifier 只保留成功轨迹；
3. 只对模型生成 token 做 supervised fine-tuning；
4. 更新后的权重同步给 rollout workers。

这不是通用意义上的最优训练算法。它只是满足 WHALE 所需黑盒接口的一个实现：
在固定 harness 下，用任务奖励更新模型。

### 3.2 Harness 阶段

论文用 Meta-Harness：

1. 保存当前与历史 harness 源码、分数和轨迹；
2. proposer 查看 archive 和失败样本；
3. 每轮提出 $M=3$ 个新 harness；
4. 每个候选在 $D_{\text{harness}}$ 的 256 个样本上各跑一次；
5. archive 中得分最高者成为当前 accepted harness。

搜索空间覆盖 prompt 之外的可执行机制。三个任务共同允许修改：

- system 和 user prompt；
- tool input/output formatting；
- tool-call feedback；
- stopping criteria；
- turn allocation。

各任务还有专属搜索轴，例如 SearchQA 的 query rewrite、retrieval 参数与排序，
Math 的 Python 提取与错误呈现，Chess 的观察格式、move parser 和 retry。

### 3.3 为什么使用短周期

长阶段会对一个随后要变化的 counterpart 过拟合：

```text
large weight phase under h0
        |
        v
theta specializes to h0
        |
        v
large harness phase replaces h0
        |
        v
theta and new h no longer fit well
```

阶段过短也会失败：候选只因 sampling noise 暂时高分就被接受，下一阶段再适应
这个错误选择，误差会被放大。有效调度必须位于：

```text
too little evidence <---- useful phase length ----> conditional overfit
```

Adaptive WHALE 对每个阶段设置最小长度，然后使用 patience：

- 权重阶段：滑动窗口训练奖励在 $P_w$ 个 step 内没有刷新阶段最优值；
- harness 阶段：archive 最优训练分数在 $P_h$ 轮内没有改善；
- 停止信号只看训练数据，不看 test data。

论文配置为权重阶段 `window = minimum = patience = 0.2 epoch`，
harness 阶段 `minimum = 6 iterations`、`patience = 2`。

## 4. 实验结果

### 4.1 主结果

论文在一个固定调度 $(E,I)=(0.6,6)$ 下报告：

| Domain | Harness-only | Weight-only | FST: prompt + weight | WHALE | Best single -> WHALE |
| --- | ---: | ---: | ---: | ---: | ---: |
| SearchQA | 38.29% | 38.27% | 35.34% | 48.34% | +10.05 pp |
| Math | 0.42% | 15.42% | 17.92% | 24.79% | +9.37 pp |
| Chess | 19.82% | 22.17% | 25.68% | 29.83% | +7.66 pp |

论文正文把相对 strongest single-component baseline 的范围写为
`+7.67` 到 `+10.05` 个百分点。按附录 Table 2 的显示值直接相减，
Chess 为 `29.83 - 22.17 = 7.66`；这是四舍五入造成的 `0.01 pp` 表观差异，
不应解读为实质矛盾。

WHALE 相对 prompt-restricted FST 的提升为：

| Domain | WHALE - FST |
| --- | ---: |
| SearchQA | +13.00 pp |
| Math | +6.87 pp |
| Chess | +4.15 pp |

这组对照支持“可执行 harness 比 prompt-only 搜索空间更有表达力”，但只覆盖
论文给定的三个任务、模型和搜索实现。

### 4.2 两类瓶颈

**SearchQA 是 harness-dominant。**

- harness-only 用 weight-only 约 `5.79%` 的 target-agent rollouts 达到相近
  峰值准确率；
- retrieval accuracy 从 `26.88%` 提升到 `60.61%`；
- 模型生成 query，但 harness 决定 query 后处理、返回文档和数量。

**Math 是 model-dominant。**

- harness-only 为 `0.42%`，weight-only 为 `15.42%`；
- base model 的截断率为 `95.83%`，单靠 response cap、turn limit 和 final
  answer recovery 无法修复；
- 一次小权重更新后，4,608 个 harness-search rollouts 将格式准确率从
  `0.83%` 提升到 `4.38%`，而 frozen base model 上 46,080 个 rollouts
  只从 `0.00%` 到 `0.63%`。

这说明“先优化 harness”不是绝对规则。更准确的规则是：先用低成本 probe
判定瓶颈；若 harness 修改没有改变相关中间指标，模型能力可能是阻塞点。

### 4.3 Alternating 对 Stagewise

| Domain | Stagewise best | WHALE `(0.6, 6)` | Difference | 超过 stagewise 时的 rollout 比例 |
| --- | ---: | ---: | ---: | ---: |
| SearchQA | 43.02% | 48.34% | +5.32 pp | 29% |
| Math | 15.63% | 24.79% | +9.16 pp | 49% |

在调度 sweep 中，`(0.2,6)` 比论文主结果使用的 `(0.6,6)` 更强：

| Domain | `(0.2,6)` | `(0.6,6)` |
| --- | ---: | ---: |
| SearchQA | 50.09% | 48.34% |
| Math | 28.33% | 24.79% |

Adaptive WHALE 在 SearchQA 达到 `52.82%`，超过最佳固定调度 `2.73 pp`；
在 Math 达到 `26.46%`，低于最佳固定调度 `1.87 pp`。因此 adaptive rule
减少调参，但论文没有证明它在所有域都优于手工最优调度。

## 5. 证据账本

| Claim | Evidence status | 能支持什么 | 不能支持什么 |
| --- | --- | --- | --- |
| 三域主结果优于四个对照 | 作者报告，表格已交叉核对 | 给定设置下的经验优势 | 任意模型、任务或生产系统上的优势 |
| Harness 可成为主要瓶颈 | 行为指标与消融支持 | SearchQA 中 retrieval/format 机制很关键 | 所有检索 Agent 都是 harness-dominant |
| 小步交替优于 stagewise | 两域 budget-matched ablation | 论文设置下 conditional overfit 解释成立 | 任意更新算子和预算下都成立 |
| Adaptive patience 可替代固定调度 | 两域实验 | 能找到有效 phase-length 区间 | 总是达到最佳固定调度 |
| 作者代码可复现 | 代码与说明公开 | 实现结构、配置和依赖可审查 | 公开 launcher 已端到端重跑 |
| Rollout 更省 | target-agent rollout 计数 | 目标模型采样量更少 | 总 GPU/API/人工成本更低 |

## 6. 论文没有证明什么

1. **没有多随机种子统计。** 未报告方差、置信区间或显著性，候选评估又只有
   每题一条 rollout，noise 对 harness selection 的影响不能量化。
2. **峰值使用 test 轨迹。** Algorithm 1/2 返回 `D_test` 上 accuracy 最好的
   pair，图表报告 best-so-far test mean@8。Adaptive phase switching 不看
   test，但最终峰值选择仍会带来乐观偏差。
3. **成本口径不完整。** Harness efficiency 明确排除了 proposer compute，
   也没有统一报告人工、服务、judge API 和失败重试成本。
4. **模型规模和任务范围有限。** 只测试 Qwen3.5-2B/4B 与三个有清晰 binary
   verifier 的任务域。
5. **Verifier 可能成为共同偏差。** 同一 verifier 用于 RSFT 接受、harness
   candidate 排名和测试评分；SearchQA 还依赖 LLM judge。
6. **公开代码尚缺完整复现证据。** 作者仓库说明 cleaned launchers 仅用 stub
   验证 handoff/resume，未做端到端 GPU 运行；仓库也未附论文 run logs、
   checkpoints 或 candidate archives。
7. **没有安全与权限评估。** Harness search 可修改可执行逻辑，论文的任务域
   有严格 sandbox/anti-cheating 边界，但没有覆盖通用生产权限风险。

## 7. 如何与 Agent 高效交互

### 7.1 首先给出可执行任务合同

高质量请求至少回答：

| Field | Question |
| --- | --- |
| Objective | 最终要改变哪个可观察结果？ |
| Deliverables | 哪些具体文件、API、报告或运行状态必须存在？ |
| Scope | 哪些边界内允许修改？ |
| Non-goals | 哪些相邻工作明确不做？ |
| Authority | 哪些 policy、design、source 与 runtime evidence 分别回答什么？ |
| Verification | 哪些独立 oracle、测试和真实 E2E 证明完成？ |
| Execution | Agent 可自主执行到哪一步，什么条件必须停止？ |
| Handoff | 最终必须返回哪些证据和未决状态？ |

“帮我优化这个系统”要求 Agent 同时猜目标、约束和验收器，交互轮数必然增加。
“实现 X，以 Y 为 governing doc，保持 Z invariant，通过 A/B/C 验收，完成后
commit/push”才是可执行合同。

### 7.2 让 Agent 优化 harness，而不只是润色 prompt

出现失败后，按以下顺序检查：

1. **Context:** 权威文档是否被读取？是否装入过多无关材料？
2. **Tool contract:** schema、权限、参数、返回结构是否稳定？
3. **Control flow:** 是否需要 plan、并行读取、重试、review 或浏览器验收？
4. **Observation:** 工具结果是否完整、可定位、带 revision 与环境？
5. **Error feedback:** 失败是否转成下一轮可操作信息？
6. **Termination:** Agent 是否过早宣布完成，或无收益地无限尝试？
7. **Model:** 前六项充分后，剩余错误是否稳定指向能力上限？

### 7.3 一轮只检验一个假设

每轮记录：

```text
baseline:
failure_class:
hypothesis:
changed_mechanism:
fixed_components:
evaluation_set:
result:
decision: retain | revert | inconclusive
next_probe:
```

同时修改 model、prompt、工具、上下文和 verifier，即使结果变好也无法归因，
下一任务也无法复用。

### 7.4 使用分离的任务集

将现实工程中的样本分成：

| Set | Purpose | 禁止事项 |
| --- | --- | --- |
| `D_work` | 日常任务和实现素材 | 不作为唯一优化依据 |
| `D_harness` | 失败分类与 harness 候选比较 | 不得包含最终 gate 的答案特征 |
| `D_gate` | held-out 回归、E2E、安全和性能门禁 | 不用于挑最佳候选 |

小规模项目不需要几百条样本，但必须有角色分离。哪怕 `D_harness` 只有 5 个
历史失败，`D_gate` 只有 3 个未见过场景，也比在同一个案例上反复调 prompt
更可信。

### 7.5 设置最小证据和停止条件

直接照搬论文的 `0.2 epoch / 6 iterations / patience 2` 没有依据。工程上应先
定义：

- 最小样本数或最小运行次数；
- 必须改善的主指标；
- 不得退化的正确性、安全和成本指标；
- 连续多少轮未改善后停止；
- 何时回退；
- 何时判定 harness-limited 或 model-limited。

停止条件同时防止两类浪费：证据不足就接受噪声，以及在固定模型上无限雕刻
harness。

## 8. 可复用规范

RecallFS 的
[`Agent Interaction and Harness Optimization Contract`](../../../designs/agent-interaction-harness.md)
把上面的推论固化为：

```text
task contract
    |
    v
context + tool + control harness
    |
    v
trajectory evidence
    |
    v
failure classification
    |
    v
one-mechanism candidate
    |
    v
harness set evaluation
    |
    +---- pass ----> held-out gate ----> retain
    |                                  |
    +---- fail ----> revert             +---- fail ----> revert
    |
    v
patience or model-bottleneck decision
```

该合同与
[`Agent-ready Docs Contract`](../../../designs/agent-ready-docs.md)
互补：

- agent-ready docs 约束**稳定意图如何进入上下文**；
- interaction-harness contract 约束**一次任务如何执行、取证与迭代**；
- nmem 保存跨会话 rationale 和历史；
- code、tests 和 runtime evidence 描述当前可执行行为和真实观察。

## 9. 为什么不做 Demo

WHALE 的关键结论依赖：

- 大规模模型权重更新；
- 多轮 target-agent rollout；
- executable harness 搜索；
- 三套隔离数据；
- GPU、retrieval、sandbox 和外部 judge/proposer 服务。

写一个小脚本交替修改两个字符串，只能演示 coordinate descent 的外形，不能
验证论文的瓶颈转移、noise/overfit 区间或 rollout efficiency。这里选择更有
长期价值的产物：

- 归档并校验原始 PDF；
- 固定作者代码 revision；
- 建立 claim/evidence/limitation 账本；
- 将可迁移机制写入 repository-wide contract；
- 留下可执行 reconciliation anchors，供真实任务验证。

## 10. 最终判断

WHALE 对日常 Agent 协作的最强启示是：

> **把失败当成系统诊断问题，不要把所有失败都归因于模型，也不要把所有改进都压进 prompt。**

高效交互来自三个闭环：

1. **语义闭环：**目标、边界、权威信息和完成条件明确；
2. **执行闭环：**工具、上下文、错误反馈、重试和终止可观察；
3. **证据闭环：**候选修改可归因，held-out gate 防止过拟合，失败可回退。

这些结构可以跨 Agent、跨模型和跨项目复用。具体 prompt 文案通常不可以。
