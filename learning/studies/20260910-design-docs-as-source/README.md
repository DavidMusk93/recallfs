---
doc_id: recallfs-study-design-docs-as-source-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260910-design-docs-as-source
depends_on:
  - learning/studies/20260910-design-docs-as-source/source.md
supersedes: []
verified_by:
  - PDF digest and metadata verification
  - claim and evidence review
---

# Design Docs 作为持久 Agent Context

> 主文：Kushnir 等，
> [Design Docs Are All You Need](../../sources/20260910-design-docs-are-all-you-need-v1.pdf)，
> arXiv `2609.05364v1`。
>
> 本文严格区分论文声称、论文给出的证据和 RecallFS 的工程推论。

## 1. 结论先行

这篇论文最值得吸收的不是“删掉所有代码”，而是下面这条工程原则：

> **把文档写成可翻译、可组合、可验证的语义接口，而不是实现完成后的解释。**

对 RecallFS，建议采用受约束的版本：

1. 复杂任务必须有一个明确的 governing doc，Agent prompt 只传入口路径和
   执行协议，不复制整份需求。
2. 文档声明权威类型、适用范围、显式依赖、稳定 invariant、worked example
   和 reconciliation anchor。
3. Agent 按显式 `depends_on` 拓扑读取入口文档及其依赖，不扫描全库后自行拼凑
   一套隐式需求。
4. 只有子树明确声明 `authority: generation`，并给出 clean regeneration
   和独立验收门禁时，其生成代码才是 disposable build product。
5. 其他模块仍是 maintained source。文档描述目标，代码和测试描述当前可执行
   行为，运行证据描述真实观察；冲突意味着 drift，不能静默挑一个相信。
6. 每次实现的 handoff fields 必须恰好为 `docs_read`、`anchor_results`、
   `ambiguities`、`changed_artifacts`、`verification_evidence`、
   `successor_id`。`successor_id` 是未解决工作的下一个持久 run/task/doc
   标识；没有后继时为 `none`。反复出现的歧义应修正文档，而不是长期堆在
   prompt 中。

仓库级合同见
[`designs/agent-ready-docs.md`](../../../designs/agent-ready-docs.md)。

## 2. 论文解决的两个问题

### 2.1 Incremental generation debt

论文把时刻 $t$ 的 specification 记为 $S_t$，generator 记为 $G$。理想的
greenfield build 是：

$$
C_t = G(S_t)
$$

现实中的增量修改却把旧实现也作为输入：

$$
C_{t+1} = G(S_{t+1}, C_t)
$$

论文用下面的差异表达增量修补带来的 debt：

$$
D_{t+1} = G(S_{t+1}, C_t) - G(S_{t+1})
$$

这不是可直接测量的数学距离，因为论文没有定义两个实现如何相减。它表达的是
一个重要直觉：旧实现会把已经失效的结构假设带入下一版。定期只从当前
specification 重建，可以去除这种历史路径依赖，但不能自动消除错误的
specification、错误的 oracle 或新的实现缺陷。

### 2.2 Context-window myopia

成熟代码库无法稳定塞入一个 Agent 的上下文。局部 patch 容易满足眼前代码，
却破坏跨模块 invariant。论文不是尝试扩大单次 prompt，而是改变任务分解：

```text
design docs
    |
    v
dependency DAG
    |
    v
one bounded doc per coding agent
    |
    v
topological integration
    |
    v
independent reconciliation
```

核心是把全局一致性从“某个 Agent 全都记住”转移到“文档边界、依赖图和验收
锚点共同约束”。

## 3. SMART 的工作方式

论文描述的生成流程如下：

```text
human edits docs only
          |
          v
read-only agents infer dependencies
          |
          v
orchestrator builds a DAG
          |
          v
coding sub-agents run in topological waves
          |
          v
reference reconciliation + guards + unit tests
          |
     +----+----+
     |         |
   fail       pass
     |         |
     v         v
ambiguity log  replace old build
```

它依赖四个互相咬合的条件。

### 3.1 Self-contained doc

一个 coding sub-agent 只处理一个文档。文档必须在自己的任务边界内完整，
依赖只通过前置模块暴露的接口进入。这样才能真正缩小上下文，而不是把缺失信息
变成 Agent 的猜测。

### 3.2 Worked example

论文反对只有高层规则的“constitution-only”写法。关键语义需要具体演算：

- 给定输入；
- 每一步中间 shape、value 或 state；
- 精确的闭式表达式；
- 最终期望输出。

这类示例同时是解释、few-shot demonstration 和测试样例。

### 3.3 Reconciliation anchor

论文要求每个包含数值语义的文档以一个小型 preset 收尾，明确写出 exact
expected output，并由生成测试执行。它的作用不是增加案例数量，而是固定最容易
被自然语言模糊掉的边界。

### 3.4 Minimal stable IR

SMART 不是仅靠好 prompt 获得一致性。它还把建模对象压缩到少量、正交、可递归
组合的 `Op`：

- interior node 是 graph 或 loop；
- leaf node 是算法与硬件定价的边界；
- shape 和 cost 使用 symbolic expression；
- fast mode 做分析式 roll-up；
- slow mode 做 resource-aware modulo scheduling。

对一般工程的启示是：如果 underlying abstraction 本身频繁交叉泄漏，再详细的
docs 也只是在描述不稳定结构。可重复生成首先要求少量稳定接口和清晰 ownership。

## 4. 证据账本

| 论文内容 | 证据状态 | 能支持什么 | 不能支持什么 |
| --- | --- | --- | --- |
| SMART 有 50 份 design docs、约 9,000 行 prose | 作者报告 | 工作流不是单文件玩具 | 未给出文档、规模分布或复杂度 |
| 全量重建耗时 1.5 到 3 小时 | 作者报告 | 在作者环境中可能具有可操作性 | 无 run 数、方差、硬件和模型版本 |
| Claude Code API 成本约 100 美元 | 作者报告 | 给出量级估算 | 未计人类写 doc、修复、review 和 CI 成本 |
| 与 hand-audited reference model 达到 round-off precision | 作者报告 | 存在数值 reconciliation | 无样本数、容差、失败率和结果表 |
| 包含 DeepSeek-V3 serving on TPU pod slice | 作者报告 | 覆盖了一个复杂案例 | 未公开该模型、输入和 oracle |
| Flash Attention DSL listing | 论文内可见 | minimal symbolic IR 具有表达力 | 不是端到端实现或复现证据 |

因此，论文提供的是可信的系统设计案例和作者经验，不是可以独立复核的完整实验。

## 5. 论文没有证明什么

1. **没有公开可复现 artifact。** 论文没有给出 SMART docs、生成代码、
   orchestration、prompt、测试、日志或固定模型配置。
2. **没有可靠性统计。** 未报告首轮成功率、重试次数、Agent 引入缺陷率、
   多次生成方差和人类修复时间。
3. **没有维护基线。** 未把 clean regeneration 与同一系统的传统增量维护做
   同口径比较。
4. **测试可能与 spec 共错。** 如果实现和测试都从同一份错误文档生成，
   reconciliation 只能证明自洽，不能证明外部正确。
5. **依赖推断未经评估。** machine-discovered DAG 的漏边、错边、环和稳定性
   没有数据。
6. **外推范围有限。** Symbolic performance model 具有紧凑数学语义和强数值
   oracle，不能直接推出数据库迁移、分布式状态机、安全系统、UI 或 native
   runtime 也适合全量重建。
7. **“almost no code”仍有 trusted base。** 论文明确保留少量 leaf utilities；
   它们的正确性和演进责任没有展开。

因此，“docs are all you need”应理解为一个满足严格前提的 architecture
option，不是所有仓库的默认法则。

## 6. RecallFS 的权威分层

不同 artifact 回答不同问题，不使用一个含混的“source of truth”覆盖全部：

| Artifact | 回答的问题 | Agent 行为 |
| --- | --- | --- |
| `AGENTS.md` / policy | 允许和禁止什么 | 必须遵守，nearest scope 优先 |
| active design / contract | 我们要实现什么 | 作为目标行为和 invariant |
| source / schema / migration / tests | 当前能执行什么 | 阅读并验证现状，不假定已符合 design |
| runtime evidence | 机器上实际发生什么 | 带环境、revision 和时间解释 |
| study / incident / archive | 当时学到或观察到什么 | 保留历史范围，不冒充当前指令 |
| nmem | 跨会话为何这样决定、如何演变 | 保存 rationale/history，不覆盖当前实现目标 |
| generated output | 某份 spec 生成了什么 | 仅在显式 generation contract 内可丢弃 |

冲突处理不是简单覆盖：

```text
policy conflict       -> stop
design vs code        -> report drift, then change doc/code/tests coherently
code vs runtime       -> investigate environment and revision
history vs current    -> preserve history, add superseding context
generated vs spec     -> regenerate from clean input, do not patch output
```

nmem 与 repository doc 冲突时，必须报告冲突；active tracked design 约束当前
实现目标。tracked decision 变化后，追加一条关联旧记录的 immutable evolved
memory，不得改写旧记忆或静默覆盖任一来源。

## 7. Agent-ready doc 的最小结构

新建或实质修改的复杂 design doc 至少应包含：

1. `doc_id`、`kind`、`status`、`authority`、`applies_to`、`depends_on`、
   `supersedes`、`verified_by`；
2. 一句话 decision；
3. scope 和 non-goals；
4. 输入、输出、接口、ownership 和 invariant；
5. happy path、边界和失败语义；
6. 至少一个逐步 worked example；
7. 带稳定 ID 的 reconciliation anchors；
8. 证据、假设、未知项和 supersession 关系。

论文让 Agent 推断 DAG。RecallFS 应把**显式依赖作为权威边**，Agent 推断只做
lint：它可以发现疑似漏边，但不能仅凭推断决定执行顺序。原因是 dependency
ordering 本身就是 correctness contract，不应交给不可复现的语义猜测。

手工执行时，仓库相对路径必须精确解析；doc ID 必须通过扫描 repository
frontmatter 唯一解析。缺失、重复或循环引用在 mutation 前阻断。该校验在
dependency linter 落地前保持手工执行并记录。

## 8. 如何提示 Agent

Prompt 应该薄。它不复制设计正文，只指定入口、读取协议和完成条件：

```text
Implement from <entry-doc>.

Read and obey applicable policy docs before interpreting intended behavior:
the nearest AGENTS.md for each artifact, then the repository-root AGENTS.md.
For intended behavior, treat only active docs with authority=design or
authority=generation as normative. Resolve each explicit depends_on reference:
repository-relative paths resolve exactly; doc_id values resolve uniquely by
scanning repository document front matter. Missing references, duplicate
doc_id matches, or cycles block all mutation. Read resolved dependencies in
topological order. Validation is manual until a dependency linter exists.

Preserve stated invariants and execute every reconciliation anchor. Convert
anchors into tests when practical. If the docs, code, tests, or runtime
evidence disagree, report the drift; do not silently choose one.

Code is disposable only for paths covered by an explicit generation contract.
Return exactly: docs_read, anchor_results, ambiguities, changed_artifacts,
verification_evidence, successor_id.
```

这样做有三个好处：

- prompt 不会复制并逐渐偏离长期文档；
- Agent context 由 DAG 控制，而不是由一次搜索的偶然结果控制；
- 每次失败都能定位到某个 doc 或 anchor，形成可修复反馈。

## 9. 采用路线

### P0: 先规范文档

- 引入 typed authority 和显式 dependency metadata；
- 新改复杂文档必须有 worked example 与 reconciliation anchors；
- Agent prompt 使用 governing-doc path；
- 不批量改写历史文档，避免制造无意义 churn。

### P1: 选择低风险 generated island

优先选择无持久状态、输出可完全比较、clean build 成本低的模块。outputs 必须
使用规范化、仓库相对且互不重叠的路径；`clean_root` 位于 tracked outputs 外。
拒绝 `..`、仓库根目录、symlink escape 和 maintained paths overlap。生成后
记录 path/digest manifest，promotion 只能修改声明的 outputs。至少记录：

| Metric | Purpose |
| --- | --- |
| first-pass anchor pass rate | 文档能否被正确翻译 |
| ambiguity count per doc | 哪些文档最难解释 |
| repeated-build equivalence | 生成是否稳定 |
| human repair time | 隐藏维护成本 |
| generation duration and cost | 是否优于增量维护 |
| differential and property results | 是否与独立 oracle 一致 |
| escaped defects | 自洽测试是否遗漏真实错误 |

### P2: 自动化

- 对 metadata、missing dependency、cycle、broken link 做 lint；
- 从 reconciliation anchor 生成测试骨架，但保留独立 oracle；
- 在隔离目录 clean-generate，验收通过后再替换 tracked output；
- 记录 doc digest、DAG、模型、工具版本、耗时、成本、重试和结果。

### P3: 用证据决定是否扩大

只有 pilot 在正确性、总成本和变更速度上持续优于 maintained implementation，
才扩大 generation boundary。不能因为一次成功生成就宣布代码可丢弃。

## 10. 为什么本 study 不做代码 demo

本论文的核心是 repository lifecycle，而不是一个可由小程序验证的算法。SMART
没有公开 artifact；构造一个玩具 generator 只能证明“Agent 能从 Markdown
写代码”，无法复现 50-doc DAG、1.5 到 3 小时重建或 reference-model parity。

本 study 选择更诚实的可验证产物：

- 归档原始 PDF 并记录 digest；
- 把论文的 claim、evidence 和 limitation 分离；
- 产出可直接执行的
  [`agent-ready docs contract`](../../../designs/agent-ready-docs.md)；
- 用新 contract 约束后续真实项目，再通过 pilot 指标判断是否值得生成化。

## 11. 最终判断

论文真正改变的是“文档的角色”：

```text
passive explanation
        |
        v
versioned semantic interface
        |
        v
bounded agent context
        |
        v
worked examples + independent anchors
        |
        v
repeatable implementation
```

对 RecallFS，最合理的目标不是 docs-only repository，而是
**docs-governed engineering**：文档承载稳定意图和验收语义，Agent 负责翻译，
代码、测试和真实运行证据共同阻止错误 spec 被“高一致性地生成”出来。
