---
doc_id: recallfs-study-gendb-generative-query-processing-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260928-gendb-generative-query-processing
  - projects/stream_engine
depends_on:
  - recallfs-source-gendb-generative-query-processing-v1
  - recallfs-evidence-gendb-implementation-audit-v1
  - projects/stream_engine/docs/jit/json_encoder_llvm_jit_plan.md
  - projects/stream_engine/docs/jit/json_encoder_v2_negative_optimization_case.md
  - projects/stream_engine/docs/jit/json_encoder_v2_optimization_journal.md
  - projects/stream_engine/docs/fringedb-dict-ingest-plan.md
supersedes: []
verified_by:
  - learning/studies/20260928-gendb-generative-query-processing/evidence/source-digests.txt
  - learning/studies/20260928-gendb-generative-query-processing/evidence/implementation-audit.md
  - learning/studies/20260928-gendb-generative-query-processing/report.html
---

# GenDB：把 LLM 放进数据库产品的正确位置

> 论文：Lao 和 Trummer，*GenDB: The Next Generation of Query Processing --
> Synthesized, Not Engineered*，arXiv `2603.02081v1`。
>
> 实现：`SolidLao/GenDB`，论文分支
> `b7418071fc51aacd219964e4a68e593234db9237`，当前 `main`
> `0ccf053ade163137775cd2e9c6e1288ea89d524b`。
>
> 浏览器版：[report.html](report.html)。

本文严格区分论文报告、上游代码事实、RecallFS 已有运行经验和产品建议。

## 1. 结论先行

GenDB 证明了一个值得投入的方向：

> 对**高频、只读、分析型、输入语义稳定**的查询模板，离线搜索一个针对数据分布
> 和硬件特化的执行程序，可以显著快于通用执行器。

它没有证明可以用 LLM 取代数据库内核。论文和实现都没有覆盖生产数据库最难的
语义：事务、Multi-Version Concurrency Control（MVCC）、并发更新、容灾恢复、
schema 演进、多租户隔离、权限、资源治理和长尾延迟。

对 Tide/stream_engine，正确的应用方式不是 fork GenDB，也不是让 Agent 在在线
查询路径直接生成并运行任意 C++。应新增一个 **Synthesized Accelerator**
插件平面：

1. Catalyst、现有 planner、Velox 和 FringeDB 继续拥有 SQL 语义、分布式执行、
   状态与回退路径。
2. 离线控制面从重复查询模板、真实 profile 和硬件信息生成候选 typed plan 或
   kernel。
3. 候选在隔离环境中编译，以现有引擎为独立 oracle 做 differential validation。
4. 只有通过语义、安全、资源、性能和兼容门禁的签名 artifact 才进入 registry。
5. 在线 dispatcher 只选择已经验证的 artifact；任何 identity、guard 或运行时
   检查不匹配都立即回退 Velox。

第一落点应是现有 JSON encoder JIT 和纯计算 leaf operator，而不是完整 SQL
query binary。stream_engine 已经验证过：`schema + table -> plan -> compiled
kernel` 可行，但“生成了 LLVM IR”本身不带来性能；execution shape、memory
layout 和 data movement 才决定收益。这正好给 GenDB 的搜索闭环提供了可信边界。

## 2. GenDB 的核心机制

GenDB 把传统数据库中由人长期维护的物理设计和执行器，改造成一次有反馈的搜索：

```text
schema + SQL + data profile + hardware + budget
                      |
                      v
              workload analysis
                      |
                      v
            storage/index design
                      |
                      v
             physical plan proposal
                      |
                      v
             native code generation
                      |
              +-------+-------+
              | compile + run |
              +-------+-------+
                      |
          +-----------+-----------+
          |                       |
          v                       v
   result oracle             runtime profile
          |                       |
          +-----------+-----------+
                      |
               revise or stop
```

重要的不是使用五个 Agent，而是三种约束同时存在：

- **分解约束：**分析、存储、计划、代码和优化各有明确职责；
- **结构约束：**关键交接使用 JSON 和文件合同，不依赖纯自然语言；
- **反馈约束：**候选必须真实编译、执行、校验和计时。

LLM 在这里是候选生成器，确定性程序才是预算、执行、验证和选择器。

## 3. 性能从哪里来

GenDB 不是靠更好的 cardinality estimate 小幅调整 join order。它能够跨越传统
operator 边界，同时特化数据布局、算法和机器代码：

| 特化维度 | 论文示例 | 通用机制 |
| --- | --- | --- |
| 数据分布 | 低基数组合直接映射到小数组 | 用已知 domain 消除 hash 和动态分派 |
| cache 拓扑 | 让 key working set 进入共享 L3 | 以 cache budget 选择共享/线程局部结构 |
| workload | 为重复 query 构建专用索引 | 用执行频率摊销离线构建成本 |
| execution shape | 融合 scan、filter、join、aggregate | 删除中间 materialization 和阶段边界 |
| storage layout | dictionary、zone map、紧凑列 | 降低带宽与随机访问 working set |

TPC-H Q1 的 6 个 group 可以用直接数组；Q3 的约 400 万 group 则需要重新考虑
hash table 布局、共享方式与 cache footprint。这里的普遍结论是：

> 物理实现选择应由“语义 + 数据分布 + 硬件 + 复用次数”共同决定，而不是只由
> SQL operator 类型决定。

这与 stream_engine 的 JSON encoder 经验一致。第一版 JIT 将 column-major
路径改成 row-major，并保留大量 host callback，steady-state 只有 legacy 的
`0.49x` 到 `0.77x`。恢复 execution shape、减少 data movement、改进 layout
后才达到约 `3x` 中位数。GenDB 可用于扩展搜索空间，但不能替代这类底层判断。

## 4. 论文实际证明了什么

| Claim | Evidence | 结论强度 |
| --- | --- | --- |
| 针对固定 OLAP workload 生成专用代码可行 | 开源代码、生成产物、计划和运行记录 | 已支持 |
| 在论文机器上快于五个通用引擎 | 作者报告的 5 个 TPC-H 和 6 个 SEC-EDGAR 查询 | 仅限该设置 |
| runtime feedback 能产生数量级改进 | Q18 `12147 ms -> 74 ms`，SEC Q4/Q6 多轮下降 | 已展示个案 |
| 多 Agent 优于单 Agent | 一组 ablation run | 有信号，缺少多 seed |
| 能替代生产 DBMS | 未评测事务、更新、恢复、隔离、权限、并发 | 未支持 |
| 能泛化到任意 SQL 和数据变化 | 只测固定数据和少量查询 | 未支持 |

论文报告的 aggregate hot runtime：

| Workload | GenDB | Best reported baseline | Reported speedup |
| --- | ---: | ---: | ---: |
| TPC-H SF10，5 个查询 | 214 ms | Umbra 590 ms | 2.8x |
| SEC-EDGAR，6 个查询 | 328 ms | DuckDB + GenDB indexes 1,549 ms | 5.0x |

这些结果来自 384 GB 内存、64 hardware threads、全部数据驻留内存的单机。
每个系统测三个 hot run。本文没有独立复现，不能把数字外推到 Tide 的分布式、
流式或混合 workload。

## 5. 经济性：先算摊销点

使用专用 artifact 的前提不是“它更快”，而是收益能够覆盖生成、构建、验证、
部署和维护成本。最小模型是：

$$
N_{\text{break-even}}
=
\left\lceil
\frac{
T_{\text{generate}} + T_{\text{build}} + T_{\text{verify}} + T_{\text{deploy}}
}{
L_{\text{baseline}} - L_{\text{accelerated}}
}
\right\rceil.
$$

论文仓库的 multi-agent telemetry 记录：

| Workload | 生成 wall time | LLM cost |
| --- | ---: | ---: |
| TPC-H | 90.9 min | $14.15 |
| SEC-EDGAR | 139.7 min | $23.49 |

只用论文 aggregate runtime 粗算，且忽略 ingestion、验证、发布和美元成本：

- TPC-H 约需重复 14,400 轮五查询 workload 才摊平生成 wall time；
- SEC-EDGAR 约需重复 6,900 轮六查询 workload 才摊平。

因此 admission policy 应排序：

1. 总 CPU-hours 或资源成本，而不是单次 speedup；
2. 模板稳定性和预计剩余生命周期；
3. p95/p99 latency 或 SLA 缺口；
4. 生成与验证成本；
5. artifact 失效概率。

短查询即使有高倍 speedup，也可能不值得生成。

## 6. 上游实现不能直接进生产

完整代码审计见
[`evidence/implementation-audit.md`](evidence/implementation-audit.md)。最关键的
问题是：

1. **错误候选可能被晋升。**首轮 `best` 判断使用进程是否成功退出，而不是结果
   是否验证通过；当前 `main` 仍然如此。
2. **复用 identity 不正确。**当前跳过逻辑主要按 `queryId + hardware` 判断；
   同一个 `Q3` 换 SQL 仍可能复用旧 binary。
3. **artifact manifest 不完整。**缺 schema、data epoch、SQL semantics、compiler、
   ABI、oracle 和 prompt/model 版本。
4. **Agent 与 generated binary 无隔离。**Claude 配置绕过权限，Codex 使用
   `danger-full-access`；没有发现进程、文件、网络和资源 sandbox。
5. **oracle 只覆盖固定 CSV。**没有验证 result schema，tolerance 依赖列名启发式，
   也没有 held-out 参数、数据变体、metamorphic test 或 fuzz。
6. **基准不是完整 end-to-end。**GenDB 计时减去 output 阶段，而 baseline 包含
   `fetchall()`；没有并发、NUMA、tail latency、更新或恢复证据。
7. **工程可复现性不足。**无 release tag、lockfile、CI 和真正 test target；
   编译器未固定。

因此，可复用的是思想和部分 artifact shape，不是上游 orchestrator。

## 7. Tide/stream_engine 的目标架构

### 7.1 所有权边界

```text
Online data plane

SQL -> Catalyst -> distributed DAG -> Velox operators -> FringeDB
       [semantics]   [snapshot/task/retry]   [trusted fallback]

Offline synthesis plane

query telemetry -> workload fingerprint -> candidate IR
                                             |
                                             v
                                    isolated build runner
                                             |
                       +---------------------+---------------------+
                       |                                           |
                       v                                           v
                 semantic oracle                            performance gate
                       |                                           |
                       +---------------------+---------------------+
                                             |
                                             v
                                  signed artifact registry
                                             |
                                             v
                                    guarded runtime lookup
```

不可让 LLM 拥有的 authority：

- SQL 语义定义；
- snapshot、watermark、checkpoint 和 exactly-once 语义；
- 生产 artifact 的最终晋升；
- 生产 host 权限；
- verifier 通过标准；
- fallback 和熔断策略。

### 7.2 插件接口

遵循 Everything is a plugin，但 hot path 不使用通用虚调用广播。建议接口面：

| Plugin | Input | Output | Trust |
| --- | --- | --- | --- |
| `WorkloadProfiler` | normalized plan、stats、hardware、telemetry | bounded profile | trusted |
| `CandidateGenerator` | optimization contract | typed candidate IR | untrusted |
| `LoweringBackend` | candidate IR、target ABI | object/module | trusted compiler |
| `SemanticOracle` | query contract、snapshot、candidate result | verification report | independent |
| `BenchmarkRunner` | artifact、resource envelope | distribution + counters | isolated |
| `ArtifactRegistry` | signed manifest + evidence | immutable version | trusted |
| `RuntimeDispatcher` | execution key + live guards | artifact or fallback | trusted |

Agent 可以实现多个 `CandidateGenerator`，例如 rule-based、LLM、search 或人工提交；
下游门禁不感知候选来自哪种模型。

### 7.3 Artifact identity

cache key 至少包含：

```text
logical_plan_digest
+ parameter_schema_digest
+ result_schema_digest
+ sql_semantics_digest
+ catalog_schema_epoch
+ storage_layout_digest
+ statistics_digest
+ data_visibility_contract
+ hardware_class
+ compiler_and_flags_digest
+ runtime_abi_version
+ generator_version
+ verifier_suite_version
```

其中 `data_visibility_contract` 不一定绑定每一批数据内容。若 kernel 只依赖 schema
和 cardinality guard，可绑定“允许的 stats 区间”；若使用 query-specific derived
data，则必须绑定 snapshot/partition epoch。

## 8. 最小可行落地顺序

### L0：离线 advisor

让 LLM 只读取匿名化 plan、profile 和硬件计数器，输出 typed optimization
proposal。人工审查后由现有工程流程实现。目标是验证建议质量，不执行生成代码。

**成功门槛：**在 20 个已知慢 query/encoder case 上，至少 30% 的建议能通过
工程师复核并形成可测候选；无数据泄露。

### L1：JSON encoder kernel search

接入现有 `EncoderPlan -> LLVM JIT` 路径。LLM 只能选择和组合受支持的 lowering
策略，例如 column-major、null specialization、formatter fusion、row reserve 和
cache policy，不直接生成任意 host code。

**原因：**该模块已有 baseline、负优化记录、稳定性脚本、kernel cache 和
debug artifact 需求，是最小且证据最强的切入点。

### L2：纯计算 Velox leaf/subtree

扩展到 deterministic、read-only、无副作用的 filter/project/aggregate 小子树。
candidate 通过稳定 ABI 读取 `RowVector` 或 FringeDB column view，输出标准
Velox vector。只允许受控 helper allowlist。

**排除：**shuffle、stateful streaming、source/sink、watermark、checkpoint、
remote RPC 和写路径。

### L3：高频查询模板 accelerator

从线上 telemetry 选取稳定模板，在真实历史 snapshot 和 held-out 参数上离线
训练与验证。runtime dispatcher 在 guard 命中时调用 artifact；否则回退原
Velox plan。先 shadow，再 1%、10%、50% canary。

### L4：物理设计建议

允许系统提出 FringeDB dictionary、zone map、排序或 index 候选，但由独立
storage migration workflow 创建新版本。旧版本保持可读，promotion 与 query
artifact 分离，禁止 Agent 原地修改生产布局。

## 9. 必须建立的门禁

| Gate | 最低要求 | 失败动作 |
| --- | --- | --- |
| Semantic | 独立引擎 differential + held-out 参数/数据 + result schema | reject |
| Numeric | DECIMAL scale、overflow、NaN、NULL、timezone、collation 合同 | reject |
| Memory safety | sanitizer、UB 检查、边界 fuzz、malformed storage | reject |
| Security | 无网络、只读 input、受限 output、syscall/cgroup/timeout | kill + reject |
| Resource | peak RSS、threads、file descriptors、temporary disk 上限 | kill + reject |
| Performance | target host、固定 CPU/NUMA、p50/p95/p99、PMU | keep baseline |
| Compatibility | runtime ABI、compiler、schema、storage、hardware identity | cache miss |
| Recovery | crash、timeout、worker loss、registry corruption、rollback | fallback |
| Operations | shadow diff、canary、kill switch、artifact provenance | disable |

正确性集合至少分成三份：

- `D_search`：优化过程可见；
- `D_validate`：候选筛选可见；
- `D_gate`：最终 promotion 才运行，CandidateGenerator 不可见。

否则 Agent 会对一个固定 oracle 过拟合，而不是生成语义正确的程序。

## 10. Worked Example

以 stream_engine 的固定 schema JSON encoder 为第一目标：

1. key 为 `schema fingerprint + encoder options + physical input kind`；
2. profiler 记录 column kind、null density、string length histogram、batch size、
   table identity 和现有 hot path counters；
3. CandidateGenerator 只能输出 `EncoderPlan` 中的合法策略组合；
4. trusted lowering 生成 LLVM object；
5. oracle 对 legacy encoder 做 byte-for-byte 或规范化 JSON differential；
6. benchmark 同时测 compile、cold、steady-state、RSS 和输出 bytes；
7. artifact registry 保存 plan、IR、object、symbols、load range、compiler 和 ABI；
8. crash 或 guard miss 后熔断该 artifact，回退 legacy encoder。

这比直接生成整个查询 executable 更适合 Tide，因为：

- 语义边界小；
- 输入输出 ABI 已存在；
- baseline 独立；
- failure 可以局部回退；
- 现有证据已经说明哪些 execution shape 有效。

## 11. Reconciliation Anchors

| ID | 输入 | 预期 |
| --- | --- | --- |
| `GDB-APP-1` | 同一 `queryId`，SQL template digest 改变 | registry lookup 必须 miss |
| `GDB-APP-2` | candidate 退出码 0，但结果值或 result schema 错误 | 不得进入 `best` 或被签名 |
| `GDB-APP-3` | hardware 相同，catalog/schema epoch 改变 | artifact 不可复用 |
| `GDB-APP-4` | candidate timeout、crash、resource breach | kill process group，熔断并回退 |
| `GDB-APP-5` | DECIMAL、NULL、排序 tie、timezone 边界 | 与 canonical engine 语义一致 |
| `GDB-APP-6` | 新 artifact shadow 结果不一致 | 线上返回 baseline 结果并自动撤销 |
| `GDB-APP-7` | 生成成本高于预测生命周期收益 | 不生成，继续使用 baseline |

当前研究验证了上游在 `GDB-APP-1`、`GDB-APP-2` 和安全隔离上的缺口；其余是
Tide 产品化前必须实现的验收合同，不是已完成能力。

## 12. 最终判断

**建议采用 GenDB 的思想，但不要采用其产品边界。**

对 Tide/stream_engine，优先级应是：

1. 将 LLM 作为离线 candidate generator；
2. 将生成目标收缩为 typed plan 和可验证 kernel；
3. 复用 Catalyst/Velox/FringeDB 的语义、ABI 和 fallback；
4. 建立 content-addressed artifact registry 与独立 promotion gate；
5. 从 JSON encoder 开始，以真实 remote benchmark 和运行时 profile 决定是否扩展；
6. 只有 leaf/subtree 证明稳定收益后，才评估完整 query template acceleration。

最不应该做的是：在生产查询到达时，把原始数据和 SQL 交给有完整 host 权限的
Agent，让它现场写 C++、编译并执行。那是一个有研究价值的 prototype，不是数据库
产品的可信执行路径。
