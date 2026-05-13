# Dict2 全局字典重写规则 — 流程图与功能解析

## 1. 概述

`dict2` 是一个 Spark Catalyst **逻辑优化规则**（`Rule[LogicalPlan]`），核心目标是：

> **将全局字典支持的字符串列（String）尽可能长时间地保持为整数字典索引（Integer），仅在语义边界处或最终输出时才解码回字符串，从而减少字符串处理开销、降低 Shuffle 和聚合代价。**

### 核心设计理念

```
┌────────────────────────────────────────────────────────────────┐
│  "dict2 是一个边界管理系统。                                      │
│   大多数 bug 本质上都是：编码态应该在哪里停止？由谁负责解码？"        │
└────────────────────────────────────────────────────────────────┘
```

---

## 2. 源文件一览

| 文件 | 职责 |
|------|------|
| `RewriteWithGlobalDict.scala` | 规则主体：probe/build 两阶段重写 + 5 个算子策略 |
| `LowCardDictEncoding.scala` | 原生表达式定义：`LowCardDictDecode` / `LowCardDictExecution` |
| `DictMetadata.scala` | 字典元数据工具：列名编解码、metadata 读写 |
| `StarryPlugin.scala` | 注册入口：`injectOptimizerRule(_ => RewriteWithGlobalDict)` |
| `ColumnarTransformRule.scala` | 物理阶段：将 `OUTPUT_DECODE_TAG` 从逻辑计划传播到物理算子 |
| `ColumnarTideSinkExec.scala` | 输出边界：RPC Sink 执行最终的字典解码 |

---

## 3. 端到端流程总图

```
                    ┌─────────────────────────┐
                    │   用户 SQL / DataFrame    │
                    └────────────┬────────────┘
                                 │
                                 ▼
                    ┌─────────────────────────┐
                    │  Spark Catalyst Analyzer  │
                    │    (逻辑计划生成)          │
                    └────────────┬────────────┘
                                 │
                                 ▼
               ┌────────────────────────────────────┐
               │    RewriteWithGlobalDict (dict2)    │
               │    ┌──────────────────────────┐    │
               │    │ Phase 1: collectDictInfos │    │
               │    │  扫描叶节点发现字典列       │    │
               │    └─────────────┬────────────┘    │
               │                  │                  │
               │                  ▼                  │
               │    ┌──────────────────────────┐    │
               │    │ Phase 2: probe (自顶向下)  │    │
               │    │  决定"哪里必须解码"         │    │
               │    │  只打 Tag，不修改计划       │    │
               │    └─────────────┬────────────┘    │
               │                  │                  │
               │                  ▼                  │
               │    ┌──────────────────────────┐    │
               │    │ Phase 3: build (自底向上)  │    │
               │    │  重写叶节点输出为 dict_idx  │    │
               │    │  按策略重写各层算子         │    │
               │    │  插入 decode Project       │    │
               │    │  传播/终止字典映射          │    │
               │    └──────────────────────────┘    │
               └────────────────┬───────────────────┘
                                │
                                ▼
               ┌────────────────────────────────────┐
               │    Spark Physical Planning          │
               │  ┌──────────────────────────────┐  │
               │  │ ColumnarTransformRule         │  │
               │  │ 传播 OUTPUT_DECODE_TAG        │  │
               │  │ 到物理 ColumnarLimitExec       │  │
               │  └──────────────────────────────┘  │
               └────────────────┬───────────────────┘
                                │
                                ▼
               ┌────────────────────────────────────┐
               │    ColumnarTideSinkExec (RPC输出)    │
               │  检测 OUTPUT_DECODE_TAG             │
               │  对 dictName metadata 列注入         │
               │  LowCardDictDecode => 字符串输出     │
               │  恢复原始列名 (去掉 _dict_idx)       │
               └────────────────────────────────────┘
```

---

## 4. 数据模型

### 4.1 DictInfo

```scala
case class DictInfo(tenant: String, table: String, column: String) {
  def dictName: String = s"$tenant/$table/$column"  // e.g. "my_tenant/my_table/domain"
  def version: Int = -1                              // 当前硬编码
}
```

### 4.2 字典发现（DictProvider）

通过表属性（table properties）发现字典列：

```
所需属性:
  ├── tenant / fringedb.tenant
  ├── table / fringedb.table
  └── tide.sql.dict.columns  (逗号/分号/竖线分隔的列名)

支持的叶节点类型:
  ├── LogicalRelation        (Hive 表)
  ├── DataSourceV2Relation   (V2 数据源)
  └── DataSourceV2ScanRelation (V2 扫描)
```

### 4.3 编码后的 Scan 输出

```
原始:  domain: StringType
  ↓ 字典重写
编码:  domain_dict_idx: IntegerType  (附带 metadata: dictName, dictVersion)
```

### 4.4 核心映射 (Dict Mapping)

```
Map[ExprId, (DictInfo, Attribute)]
  │                │         │
  │                │         └── 当前作用域内的编码属性 (in-scope encoded attr)
  │                └── 字典信息
  └── 原始值列的 ExprId (original value column)
```

该映射在 build 阶段自底向上传播，是整个重写的核心状态。

---

## 5. Tag 系统

dict2 通过 4 种 Tag 驱动两阶段协作：

```
┌──────────────────────┬─────────────────────────────────────────────────────┐
│ Tag                  │ 含义                                                │
├──────────────────────┼─────────────────────────────────────────────────────┤
│ DICT_MAPPING_TAG     │ 自底向上传播的字典映射                                │
│ Map[ExprId,(Info,    │ key=原始值列ExprId                                   │
│   Attribute)]        │ value=(字典信息, 当前编码属性)                         │
├──────────────────────┼─────────────────────────────────────────────────────┤
│ REQUIRED_DECODE_TAG  │ 打在 Project 上：这些 ExprId 必须在此处解码            │
│ Set[ExprId]          │ 也可被向下传播直到遇到能执行解码的 Project              │
├──────────────────────┼─────────────────────────────────────────────────────┤
│ CHILD_DECODE_TAG     │ 打在 Sort/Aggregate 上：子树必须已停止转发这些列的索引  │
│ Set[ExprId]          │ 用于 build 阶段检测"未满足的解码需求"                  │
├──────────────────────┼─────────────────────────────────────────────────────┤
│ OUTPUT_DECODE_TAG    │ Boolean, 打在 GlobalLimit 上                         │
│ Boolean              │ 表示最终输出需解码为用户可见的字符串                    │
└──────────────────────┴─────────────────────────────────────────────────────┘
```

---

## 6. 两阶段重写详解

### 6.1 Phase 1: Probe（自顶向下）

**目的**: 只决定"哪里需要解码"，打 Tag，不修改计划结构。

```
                    plan
                     │
          ┌──────────┼──────────┐
          │          │          │
        Sort      Aggregate  Project
          │          │          │
     ┌────┴────┐  ┌──┴──┐   ┌──┴──┐
     │ 排序键  │  │分组键│   │表达式│
     │含字典列?│  │含复合│   │含复合│
     │         │  │表达式?│  │表达式?│
     └────┬────┘  └──┬──┘   └──┬──┘
          │          │          │
          ▼          ▼          ▼
   打CHILD_DECODE  打CHILD_   打REQUIRED_
   在Sort上       DECODE      DECODE
   打REQUIRED_    在Aggregate 在Project
   DECODE在child  打REQUIRED_ 上
                  DECODE在child
```

#### Sort Probe 逻辑

```
Sort.order 中的每个 SortOrder:
  │
  ├── 排序键引用的属性是否是原始字典列?
  │     └── 是 → 直接标记该 ExprId
  │
  └── 排序键是别名 (ORDER BY alias)?
        │
        ├── 子节点是 Project → 查找 projectList 中该别名对应的原始表达式
        ├── 子节点是 Aggregate → 查找 aggregateExpressions 中对应表达式
        └── 其他 → 直接检查属性名
              │
              └── 找到字典依赖 → 标记为需解码
```

#### Aggregate Probe 逻辑

```
分组表达式:
  ├── 纯 AttributeReference → 安全，保持编码态 ✓
  └── 复合表达式 (如 upper(domain)) → 需要解码 ✗

聚合表达式:
  ├── 纯 AttributeReference → 安全 ✓
  ├── Alias(AttributeReference, _) → 安全 ✓
  └── 含字典列的复合表达式 → 需要解码 ✗
```

#### Project Probe 逻辑

```
Project.projectList 中的每个表达式:
  ├── 纯 AttributeReference → 不需要解码（直接转发索引）
  ├── Alias(AttributeReference, _) → 不需要解码
  └── 复合表达式含字典列 → 标记 REQUIRED_DECODE_TAG
```

#### 通用传播规则

```
非 Project 节点收到 REQUIRED_DECODE_TAG 后:
  → 向下传播给子节点的 output 中匹配的 ExprId
  → 直到遇到能执行解码的 Project 节点
```

---

### 6.2 Phase 2: Build（自底向上）

**目的**: 消费 Tag + 子节点映射，执行实际的表达式和计划重写。

```
               ┌──────────────┐
               │  叶节点重写    │
               │  (Scan层)     │
               │              │
               │ domain:String │──→  domain_dict_idx:Integer
               │              │      + metadata(dictName, dictVersion)
               │              │      + DICT_MAPPING_TAG
               └──────┬───────┘
                      │ 映射向上传播
                      ▼
               ┌──────────────┐
               │  各层算子策略  │
               │              │
               │ 收集子节点映射 │
               │ 查找匹配策略   │
               │ 执行重写       │
               └──────┬───────┘
                      │
                      ▼
           ┌──────────────────────┐
           │  策略未注册时: rewriteOther │
           │  替换映射中的属性引用        │
           │  原样传播映射                │
           └──────────────────────┘
```

---

## 7. 五大算子重写策略

### 7.1 ProjectRewriteStrategy（核心策略）

```
输入: Project(projectList, child)
      + 来自 child 的 dict mapping
      + 可能的 REQUIRED_DECODE_TAG

┌─────────────────────────────────────────────────────┐
│ 遍历 projectList 中每个表达式:                        │
│                                                      │
│  对表达式中的 AttributeReference:                     │
│   ├── 在映射中? + 需要解码?                           │
│   │     └── 替换为 LowCardDictDecode(encodedAttr)    │
│   │         不再传播该列的映射 (break chain)           │
│   │                                                  │
│   └── 在映射中? + 不需要解码?                         │
│         └── 替换为 encodedAttr                       │
│             Rebase 映射: 指向新的 Project 输出属性     │
│                                                      │
│ 所有输出用 Alias 包装以保持原始 name + exprId          │
└─────────────────────────────────────────────────────┘

输出: 新的 Project + rebased mapping (或空映射若全部解码)
```

**关键不变量**: Project 始终保持用户可见的 schema 身份（name, exprId），即使内部表示已变为编码态。

### 7.2 FilterRewriteStrategy

```
输入: Filter(condition, child) + dict mapping

┌─────────────────────────────────────────────────────┐
│ Step 1: 属性替换                                     │
│   condition 中的字典属性 → LowCardDictDecode(...)     │
│                                                      │
│ Step 2: 谓词优化 (尝试下推到字典引擎)                  │
│   分割为合取谓词 (AND 拆分)                           │
│   对每个谓词:                                        │
│     ├── 恰好包含 1 个 LowCardDictDecode?              │
│     │   ├── 替换 decode 为原始列名的 unbound 属性     │
│     │   ├── 检查谓词是否只引用该单列                   │
│     │   ├── 尝试 JSON 序列化                         │
│     │   │     ├── 成功 → LowCardDictExecution         │
│     │   │     │         (index, dictName, ver, json)  │
│     │   │     └── 失败 → 保留原始 decode 形式          │
│     │   └── 多列引用 → 保留原始形式                    │
│     └── 包含 0 或 ≥2 个 decode → 保留原始形式          │
│                                                      │
│ Step 3: 原样传播映射                                  │
└─────────────────────────────────────────────────────┘
```

**优化示例**:

```sql
WHERE domain = 'example.com'
```

```
优化前: Filter(LowCardDictDecode(domain_dict_idx) = 'example.com')
优化后: Filter(LowCardDictExecution(domain_dict_idx, dictName, -1, '{"eq":"example.com"}'))
```

### 7.3 AggregateRewriteStrategy

```
输入: Aggregate(groupingExprs, aggExprs, child) + dict mapping + CHILD_DECODE_TAG

┌───────────────────────────────────────────────────────────┐
│ Step 1: 确定未满足的解码需求                                │
│   unsatisfied = CHILD_DECODE_TAG ∩ 当前mapping中仍存在的列  │
│                                                            │
│ Step 2: 在 child 下方插入 decode Project                    │
│   调用 buildDecodeAndBreakProject(child, mapping, unsatisfied)│
│   → 生成一个新 Project，对需解码的列插入 LowCardDictDecode   │
│   → 从 mapping 中移除已解码的列                              │
│                                                            │
│ Step 3: 用剩余映射重写 grouping 和 aggregate 表达式          │
│                                                            │
│ Step 4: 重建输出映射                                        │
│   扫描 newAggregateExpressions，把仍编码的输出属性           │
│   重新映射到 Aggregate 输出                                  │
└───────────────────────────────────────────────────────────┘
```

**安全 vs 不安全**:

```
安全 (保持编码):          不安全 (需解码):
  GROUP BY domain           GROUP BY upper(domain)
  (纯属性引用)              GROUP BY split(domain, '.', 1)
                            count(distinct concat(domain, path))
```

### 7.4 SortRewriteStrategy

```
输入: Sort(order, child) + dict mapping + CHILD_DECODE_TAG

与 Aggregate 策略结构类似:
  1. 读取 CHILD_DECODE_TAG
  2. 找出 unsatisfied 的编码列
  3. 调用 buildDecodeAndBreakProject 在 child 下方插入解码 Project
  4. 用剩余映射重写 SortOrder 表达式
```

**原因**: 字典索引的数值排序 ≠ 字符串的字典序排序，所以 Sort 必须看到字符串值。

### 7.5 GlobalLimitRewriteStrategy

```
输入: GlobalLimit(limitExpr, child) + dict mapping

┌────────────────────────────────────┐
│ 1. 重写表达式中的编码属性引用       │
│ 2. 设置 OUTPUT_DECODE_TAG = true   │
│ 3. 传播映射                        │
└────────────────────────────────────┘

注意: 这里不执行实际解码！
只是标记"最终输出需要解码"，
由后续物理计划阶段处理。
```

---

## 8. 解码辅助工具

### Dict2RewriteUtils.buildDecodeAndBreakProject

```
buildDecodeAndBreakProject(child, mapping, decodeExprIds)
    │
    ├── effectiveDecodeExprIds = decodeExprIds ∩ mapping.keySet
    │   (若为空，直接返回)
    │
    ├── 创建新的 Project:
    │   child.output.map { attr =>
    │     if (该属性对应需解码的列)
    │       Alias(LowCardDictDecode(attr, dictName, version), originalColName)(origExprId)
    │     else
    │       attr  // 保持不变
    │   }
    │
    └── 从 mapping 中移除已解码的列
        返回 (新Project, 缩减后的mapping)
```

**保证**:
1. 语义边界获得值形式
2. 父节点不再将该列视为编码态

---

## 9. 原生表达式

### LowCardDictDecode

```
输入: (dict_index: Int, dictName: String, dictVersion: Int)
输出: String (字典反查后的原始值)
语义: index → value

JVM eval/codegen: 抛出 UnsupportedOperationException
实际执行: 由 native 引擎处理
```

### LowCardDictExecution

```
输入: (dict_index: Int, dictName: String, dictVersion: Int, jsonFilter: String)
输出: Boolean (谓词在字典引擎中的执行结果)
语义: 对 index 对应的值执行 jsonFilter 描述的过滤

JVM eval/codegen: 抛出 UnsupportedOperationException
实际执行: 由 native 引擎处理
```

---

## 10. 物理阶段输出解码路径

```
逻辑计划                          物理计划
────────                          ────────

GlobalLimit                       ColumnarLimitExec
  │ OUTPUT_DECODE_TAG=true           │ OUTPUT_DECODE_TAG=true
  │                                  │
  │  (ColumnarTransformRule          │
  │   .createGlobalLimit 传播 tag)   │
  │                                  ▼
  │                               ColumnarTideSinkExec
  │                                  │
  │                                  ├── decodeOutput = child 子树中存在
  │                                  │   OUTPUT_DECODE_TAG=true 的节点?
  │                                  │
  │                                  ├── 若 true + 属性 metadata 含 dictName:
  │                                  │   ├── 数据类型: IntegerType → StringType
  │                                  │   ├── 列名: xxx_dict_idx → xxx
  │                                  │   └── 表达式: LowCardDictDecode(attr)
  │                                  │
  │                                  └── 生成 native JSON 表达式 → 输出到 RPC
```

---

## 11. 完整示例追踪

### 示例 SQL

```sql
SELECT domain, count(*) AS cnt
FROM events
WHERE domain = 'example.com'
GROUP BY domain
ORDER BY cnt DESC
LIMIT 10
```

### 重写过程

```
原始逻辑计划:
  GlobalLimit 10
    LocalLimit 10
      Sort [cnt DESC]
        Aggregate [domain] [domain, count(*) AS cnt]
          Filter [domain = 'example.com']
            Project [domain, ...]
              LogicalRelation(events)
                  output: [domain: String, ...]

─────────────────────────────────────
Phase 1: collectDictInfos
  → {domain.exprId → DictInfo("tenant1", "events", "domain")}

─────────────────────────────────────
Phase 2: probe (自顶向下)
  Sort:
    排序键 cnt 不是字典列 → 不打 tag
  Aggregate:
    GROUP BY domain 是纯 AttributeReference → 不打 CHILD_DECODE_TAG
  Project:
    纯 AttributeReference → 不打 REQUIRED_DECODE_TAG
  Filter:
    无特殊处理

─────────────────────────────────────
Phase 3: build (自底向上)

  1) LogicalRelation 重写:
     domain:String → domain_dict_idx:Integer (带 metadata)
     mapping = {domain.exprId → (DictInfo, domain_dict_idx)}

  2) Project 重写 (ProjectRewriteStrategy):
     domain_dict_idx 转发，Alias 保持原名
     rebased mapping 指向 Project 输出

  3) Filter 重写 (FilterRewriteStrategy):
     domain = 'example.com'
       → LowCardDictDecode(domain_dict_idx) = 'example.com'
       → LowCardDictExecution(domain_dict_idx, "tenant1/events/domain", -1, json)
     映射继续传播

  4) Aggregate 重写 (AggregateRewriteStrategy):
     GROUP BY domain → GROUP BY domain_dict_idx (编码态保持)
     输出映射重建

  5) Sort 重写 (SortRewriteStrategy):
     ORDER BY cnt → cnt 不在映射中，无需处理
     映射继续传播

  6) GlobalLimit 重写 (GlobalLimitRewriteStrategy):
     设置 OUTPUT_DECODE_TAG = true

─────────────────────────────────────
物理阶段:
  ColumnarTransformRule → 传播 OUTPUT_DECODE_TAG 到 ColumnarLimitExec
  ColumnarTideSinkExec → 检测 tag, domain_dict_idx → LowCardDictDecode → "domain": String
```

---

## 12. 核心不变量

| # | 不变量 | 说明 |
|---|--------|------|
| 1 | **映射以原始值列 ExprId 为 key** | 不是以编码属性 ExprId 为 key，但 value 指向当前作用域的编码属性 |
| 2 | **Project 保持 schema 身份** | 即使内部表示变化，输出保持原始 name 和 exprId |
| 3 | **解码终止映射传播** | 一旦某列在边界处被解码，父算子不再视其为编码态 |
| 4 | **纯分组键可保持编码** | 只有复合/本地计算表达式需要解码 |
| 5 | **Sort 需要值语义** | 字典索引的数值序 ≠ 字符串的字典序 |
| 6 | **最终输出解码独立于逻辑正确性解码** | 逻辑解码处理语义边界；物理/Sink 解码处理用户可见输出 |
| 7 | **原生字典表达式不可回退到 JVM 执行** | `LowCardDictDecode` / `LowCardDictExecution` 在 JVM 中会抛异常 |

---

## 13. 策略扩展机制

dict2 提供了可扩展的策略注册机制：

```scala
trait OperatorRewriteStrategy[T <: LogicalPlan] {
  def apply(plan: T, mapping: Map[ExprId, (DictInfo, Attribute)],
            jsonConverter: ExpressionJsonConverter): LogicalPlan
}

// 注册新策略:
RewriteWithGlobalDict.registerStrategy(classOf[Join], JoinRewriteStrategy)
```

当前已注册的策略：

```
classOf[Project]     → ProjectRewriteStrategy
classOf[Filter]      → FilterRewriteStrategy
classOf[Aggregate]   → AggregateRewriteStrategy
classOf[Sort]        → SortRewriteStrategy
classOf[GlobalLimit] → GlobalLimitRewriteStrategy
```

未注册策略的算子走 `rewriteOther` 通用逻辑：替换映射属性引用，原样传播映射。

---

## 14. 常见 Debug 排查路径

```
问题分类                    排查方向
──────                      ──────
A. Scan/输出 schema 错误    → 检查叶节点重写和映射建立
B. Filter 下推/JSON 错误    → 检查 FilterRewriteStrategy.optimizeCondition
C. Aggregate/GroupBy 错误   → 表达式是纯属性还是复合? 解码插入位置对不对?
D. OrderBy 错误             → 排序键是直接字典属性、Project 别名、还是 Aggregate 输出别名?
E. 客户端可见输出错误        → 检查 OUTPUT_DECODE_TAG 和 ColumnarTideSinkExec
```

---

## 15. 与旧版 dict 规则对比

| 维度 | 旧版 dict | 新版 dict2 |
|------|----------|-----------|
| 架构 | 单体式，重写与解码混合 | probe/build 两阶段分离 |
| 解码边界 | 隐式，难以追踪 | 显式通过 Tag 标记 |
| 输出解码 | 与逻辑解码混合 | 分离为独立的物理阶段 |
| Sort/Aggregate 处理 | 容易出 bug | 通过 CHILD_DECODE_TAG 显式管理 |
| 可扩展性 | 困难 | 策略模式，可注册新算子策略 |

**核心升级**:

> dict2 将 "哪里需要值以保证正确性？" 与 "哪里需要值用于最终输出？" 这两个问题分离开来。

---

## 16. 待扩展项

1. **Join 策略** — 尚未实现 (`// TODO: Add Join strategy`)
2. **dictVersion** — 当前固定为 `-1`，未来可能需要真实版本管理
3. **名称回溯 (`dictExprIdsIn`)** — 按名称匹配辅助别名/Sort 处理，需注意名称冲突
4. **Filter 优化** — 当前只处理单字典列的合取谓词
5. **多字典列谓词** — 比较两个字典列的谓词尚未优化
