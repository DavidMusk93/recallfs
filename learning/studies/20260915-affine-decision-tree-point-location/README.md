---
doc_id: recallfs-study-affine-decision-tree-point-location-v1
kind: study
status: active
authority: design
applies_to:
  - learning/studies/20260915-affine-decision-tree-point-location/demo
depends_on:
  - recallfs-agent-ready-docs-v1
  - recallfs-source-affine-decision-tree-point-location-v1
  - recallfs-study-affine-decision-tree-point-location-exploration-v1
supersedes: []
verified_by:
  - ODT-RA-1
  - ODT-RA-2
  - ODT-RA-3
  - ODT-RA-4
  - ODT-RA-5
---

# Affine Decision Tree 二维点定位

## 1. Decision

文章里的“决策树”不是从样本训练分类规则，而是一个**离线编译的空间分区程序**：
构建阶段把多边形区域编译为二叉树；查询阶段从根开始，每个内部节点只计算一次
$w_x x+w_y y\ge t$，最终叶子直接返回区域 ID。

本 study 用 C11 实现通用二维 affine decision tree evaluator，再用一个可手算的
三站点 Voronoi 分区说明树如何产生。Python 3.13.12 不读取这棵树，而是用暴力
精确最近点算法生成 20,054 个测试样本，作为独立 differential oracle。

核心结论：

- 快的来源是**把昂贵的几何关系预计算成控制流**，不是“决策树”三个字本身；
- 查询代价由树深 $D$ 决定，单点为 $O(D)$，而不是逐个检查 $K$ 个区域；
- 斜切来自一个节点可同时使用多个坐标，边界不必平行于坐标轴；
- 构建器必须同时控制树深和 polygon fragmentation，否则查询快但树可能膨胀；
- 边界、浮点、树合法性和更新频率都是部署前必须显式确定的合同。

## 2. Scope

交付物包括：

- `demo/include/odt.h`：树、节点、结果和错误码；
- `demo/src/odt.c`：树结构验证和无分配查询；
- `demo/src/point_location_example.c`：三站点二维定位树；
- `demo/src/main.c`：人类可读 walkthrough 和批量 machine mode；
- `demo/tests/odt_test.c`：C 锚点、边界和坏树测试；
- `demo/python/point_location_oracle.py`：独立暴力 oracle 与样本生成器；
- `demo/python/tests/test_differential.py`：20,054 点端到端差分测试。

目标是彻底解释“静态区域如何变成快速查找树”，不是复刻 DuckDB extension。

## 3. Non-goals

- 不训练 CART、random forest 或 gradient-boosted tree。
- 不实现 apart 的 DuckDB binder、vectorized executor 或 `fixed_depth` 重写。
- 不实现上游 1,000 polygon 的 greedy builder。
- 不声称复现文章中的 35.7 ns/row 或 59 倍加速。
- 不提供动态插入、删除、并发更新或持久化格式。
- 不把这个教学 evaluator 标成 production-ready 空间索引。

## 4. Inputs and outputs

`odt_locate_2d` 接收一棵已验证的 caller-owned tree、两个有限 binary64 坐标和一个
可写 `odt_result`。成功时返回 `ODT_OK`，并写入 leaf value 和比较次数；失败时返回
明确的 `odt_status`，且保持 `result` 不变。

CLI machine mode 接收 whitespace-separated `x y` pairs，输出
`region comparisons`。`point_location_oracle.py` 接收 count 和 seed，输出 CSV，
其中包含从输入 binary64 值精确推导的期望 region 和 uncertainty band 标记。
binary64 evaluator 只要求在 8-ULP uncertainty band 外匹配精确 oracle；带内输出
仍必须是 A/B/C 中的有效 region。

Native C：

```bash
cmake -S learning/studies/20260915-affine-decision-tree-point-location/demo \
  -B .tmp/affine-decision-tree/native -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build .tmp/affine-decision-tree/native
ctest --test-dir .tmp/affine-decision-tree/native --output-on-failure
.tmp/affine-decision-tree/native/point_location
```

Python 样本生成和 differential test：

```bash
cd learning/studies/20260915-affine-decision-tree-point-location/demo/python
pyenv install -s 3.13.12
PYENV_VERSION=3.13.12 uv sync --python "$(pyenv prefix 3.13.12)/bin/python3"
uv run python point_location_oracle.py --count 8 --seed 20260915
POINT_LOCATION_BIN="$(git rev-parse --show-toplevel)/.tmp/affine-decision-tree/native/point_location" \
  uv run python -m unittest discover -s tests -v
```

`point_location_oracle.py` 输出 CSV，包含独立推导的精确期望 region 和 uncertainty
band 标记。Python 通过 `Fraction.from_float` 精确表示输入 binary64 值并计算平方
距离，不读取或执行 C 树。路径比较次数由 C 白盒测试单独验证。

## 5. Interfaces and ownership

`struct odt_tree` 借用 caller-owned immutable arrays。库不复制树，也不拥有
`nodes` 或 `leaf_values`。调用方应在发布树之前调用一次
`odt_tree_validate`；热路径 `odt_locate_2d` 不分配内存，也不会隐式做全树验证。

`odt_tree_validate` 验证：

- weights 和 thresholds 有限；
- child reference 非零且在范围内；
- root 没有父节点，其他内部节点恰有一个父节点；
- 不存在 cycle 或共享 internal node；
- 所有 nodes 和 leaves 均从 root 可达。

叶子可以被多条边引用，因为一个逻辑 region 可以在多个控制流出口出现。

## 6. Invariants

1. root 永远是 node reference `1`。
2. 正 reference 的有效范围为 `1..node_count`。
3. 负 reference `-k` 映射到 `leaf_values[k-1]`，`0` 永远非法。
4. 每个有效查询最多访问 `node_count` 个内部节点。
5. 每个节点按 binary64 `weights[0] * x + weights[1] * y` 计算；舍入后与
   threshold 相等时固定走 `above`。
6. 矩形边界属于内部；矩形外返回 `POINT_LOCATION_OUTSIDE`。
7. evaluator 的分类由 binary64 tree predicates 定义；differential test 只在
   两个最近精确平方距离之差大于较大 ULP 的 8 倍时断言 nearest-site 等价，不把
   该采样结果外推为所有实数输入的证明。
8. 查询不分配、不修改树，也不保留输入或输出指针。

## 7. Failure semantics

| Status | Meaning |
| --- | --- |
| `ODT_INVALID_ARGUMENT` | null pointer、空 node/leaf array |
| `ODT_INVALID_TREE` | 非法 reference、不可达项、cycle、共享 internal node 或 traversal 超限 |
| `ODT_NONFINITE_FEATURE` | 输入坐标为 NaN 或 infinity |
| `ODT_NO_MEMORY` | validator 的临时数组分配失败 |
| `ODT_NUMERIC_RANGE` | 有限输入和树参数计算出 NaN 或 infinity score |

apart 对 NaN 使用 IEEE comparison，条件为 false，因此走 `below`。本地 C API
刻意拒绝非有限坐标，因为“NaN 属于哪个地理区域”没有稳定业务语义。这是明确的
行为差异，不是兼容实现。

`odt_locate_2d` 假定树已经通过 validator，但仍检查访问 reference、最大步数和
每个计算出的 score，避免坏树导致越界、无限循环或非有限值静默选择 `below`。
它不会在失败时写入 `result`。

## 8. 先分清三件事

### 8.1 树只是执行格式

一个内部节点保存：

$$
(w_x,w_y),\quad t,\quad child_{above},\quad child_{below}
$$

给定点 $p=(x,y)$：

$$
s=w_x x+w_y y
$$

evaluator 的数值合同是按 IEEE 754 binary64 `double` 直接计算这个表达式并与
binary64 threshold 比较，不是用实数或任意精度算术判断几何关系。

- 若 $s\ge t$，走 `above`；
- 否则走 `below`；
- 正 child reference 指向另一个内部节点；
- 负 reference 指向叶子值。

所以 evaluator 只负责“照树走”，不知道 polygon、Voronoi 或 SQL。

### 8.2 affine 与 oblique 是什么

若阈值为零，$w^Tp=0$ 是穿过原点的 linear boundary。一般形式
$w^Tp=t$ 等价于 $w^Tp-t=0$，允许平移，所以是 affine boundary。

axis-aligned tree 的每个 $w$ 只有一个非零分量，例如 $x\ge 5$。
oblique tree 允许多个非零分量，例如：

$$
-6x-y\ge -32.5
$$

这是一条斜线。二维中的边界是直线，三维中是平面，$n$ 维中是 hyperplane。

### 8.3 构建与查询是两个阶段

```text
polygons + labels
        |
        v
offline builder: choose cuts, clip fragments, build topology
        |
        v
constant arrays: weights, thresholds, children, values
        |
        v
online evaluator: compare, branch, return label
```

文章把两个阶段压在一起讲，容易让人误以为查询时仍在处理 polygon。实际上性能
收益恰恰来自查询阶段不再做 polygon clipping、edge intersection 或 candidate
join。

## 9. 为什么 Voronoi 边界恰好是 affine test

设两个站点为 $s_i$ 和 $s_j$，点为 $p$。点更靠近 $s_i$ 的条件是：

$$
\lVert p-s_i\rVert^2\le\lVert p-s_j\rVert^2
$$

展开后，两边相同的 $p^Tp$ 被消掉：

$$
2(s_i-s_j)^Tp\ge\lVert s_i\rVert^2-\lVert s_j\rVert^2
$$

令：

$$
w=s_i-s_j,\qquad
t=\frac{\lVert s_i\rVert^2-\lVert s_j\rVert^2}{2}
=w^T\frac{s_i+s_j}{2}
$$

就得到节点形式 $w^Tp\ge t$。边界经过两个站点的中点，并垂直于它们的连线，
也就是 perpendicular bisector。

这个推导是整套方法的关键：**平方距离看起来是二次函数，但两两比较后变成一次
不等式**。

## 10. 本地二维例子

闭矩形范围为 $[0,10]\times[0,8]$，内部有三个站点：

| Region | Site |
| --- | --- |
| A | $(2,2)$ |
| B | $(8,3)$ |
| C | $(4,7)$ |

精确几何模型把区域定义为最近站点；距离相等时选择较小 region ID。binary64
evaluator 在 ULP 级边界邻域可能与精确模型不同。矩形外返回 `outside`。

### 10.1 三条斜切边界

将上节公式代入：

| Winner comparison | Node condition | `above` |
| --- | --- | --- |
| A vs B | $-6x-y\ge-32.5$ | A survives |
| A vs C | $-2x-5y\ge-28.5$ | A wins |
| B vs C | $4x-4y\ge4$ | B wins |

第一条并不直接返回结果。它先在 A/B 中保留较近者，再让胜者与 C 比较。这是一个
两轮 tournament：

```text
                       A vs B
                     /        \
              A survives      B survives
                 /                 \
              A vs C             B vs C
              /    \             /    \
             A      C           B      C
```

C 出现在两个叶子上不是错误。不同路径都可能证明 C 是最终最近站点；叶子是控制流
出口，不要求和逻辑 region 一一对应。

### 10.2 完整树

| Node | Condition | `above` | `below` |
| ---: | --- | --- | --- |
| 1 | $x\ge0$ | node 2 | outside |
| 2 | $-x\ge-10$，即 $x\le10$ | node 3 | outside |
| 3 | $y\ge0$ | node 4 | outside |
| 4 | $-y\ge-8$，即 $y\le8$ | node 5 | outside |
| 5 | $-6x-y\ge-32.5$ | node 6 | node 7 |
| 6 | $-2x-5y\ge-28.5$ | A | C |
| 7 | $4x-4y\ge4$ | B | C |

数组编码沿用 apart 的核心约定：node reference 从 1 开始，leaf reference 为负数。
本例 `-1/-2/-3/-4` 分别表示 `outside/A/B/C`。

### 10.3 手算查询

输入 $(5,5)$：

1. 前四个条件都成立，点位于闭矩形内；
2. node 5 得分 $-6\times5-5=-35<-32.5$，B 比 A 近，走 node 7；
3. node 7 得分 $4\times5-4\times5=0<4$，C 比 B 近；
4. 返回 C，共 6 次比较。

输入 A/B 中点 $(5,2.5)$：

1. 前四个条件成立；
2. node 5 得分恰好为 $-32.5$，`>=` 走 `above`，所以 tie 选择 A；
3. node 6 判断 A 比 C 近；
4. 返回 A。

边界归属不是几何细节，而是 API 行为。这里的 `>=` 作用于舍入后的 binary64
score；把它改成 `>` 会改变 score 与 threshold 恰好相等时的结果，但不保证每个
精确几何 bisector 输入都产生相等的 binary64 score。

## 11. 上游 1,000 polygon 树如何构建

本地例子直接写出七个节点；上游 builder 面对 1,000 个 convex Voronoi cells，
需要递归选切线。

对当前的 $n$ 个 polygon fragments：

1. 从生成站点对中产生候选 perpendicular bisectors；
2. 对每条候选线，判断每个 fragment 位于上侧、下侧还是跨线；
3. 设两侧 fragment 数为 $n_+$ 和 $n_-$；
4. 跨线 fragment 会出现在两侧，因此复制量为 $n_++n_--n$；
5. 选择最小 score：

$$
score=\max(n_+,n_-)+\lambda(n_++n_--n)
$$

第一项压低最坏分支，目标是减小最大深度；第二项惩罚 fragmentation，目标是控制
节点和叶子膨胀。选中切线后，跨线 convex polygon 用 half-plane clipping 分成
两片，两片保留同一个 cell ID，然后递归。

上游还强制候选线对应的两个 defining cells 进入相反分支。这既表达 bisector
语义，也保证即使浮点 clipping 有舍入，递归仍然取得进展。

文章报告的 1,000-cell 结果有 4,090 个内部节点而不是约 999 个，根因就是切线会
穿过其他 cells，产生带相同 ID 的多个 fragments。

## 12. 为什么查询会快

设区域数为 $K$，树深为 $D$，每个 polygon 平均边数为 $E$：

| 方法 | 单点主要工作 | 备注 |
| --- | --- | --- |
| 逐 polygon 扫描 | 最坏 $O(KE)$ 几何判断 | 无预处理，但重复做边关系 |
| spatial index + exact geometry | 索引过滤 + 候选精确判断 | 适合动态、复杂、重叠对象 |
| nested SQL `CASE` | $O(D)$ expressions | 逻辑同树，但执行表示可能庞大 |
| compiled affine tree | $O(D)$ 乘加、比较、child load | 固定分区、叶子值恒定 |

平衡树通常希望 $D$ 接近 $\log_2 K$，但 fragmentation 会使实际深度和节点数变大。
上游示例把复杂几何离线变成最多 18 次比较，所以工作量显著下降。

这不等于任意 decision tree 都快：

- 极度倾斜的树仍可退化到 $O(K)$；
- child 跳转可能造成 branch misprediction 和 cache miss；
- 若分区频繁更新，重建成本可能抵消查询收益；
- R-tree 能处理重叠对象和动态数据，本方法要求结果可表达为固定的 piecewise
  constant partition；
- 文章的 59 倍是特定 DuckDB workload 的端到端结果，不是算法常数。

## 13. `fixed_depth` 到底优化什么

variable-depth tree 的不同输入可能执行不同次数。`fixed_depth` 把浅叶子替换为
重复返回同一值的 padding subtree，使所有路径长度相同。

可能的收益是执行器知道精确循环次数，更容易批量化并减少每行终止判断；代价是原本
很快到达浅叶子的输入被迫继续比较，节点数最坏增长到 $2^D-1$。

因此：

- 已接近 complete tree 时可能有利；
- 深度差异大、输入集中在浅叶时可能更慢；
- 它不改变分类语义，也不降低理论比较上界；
- 必须限制展开规模。apart 的上限是 1,000,000 个内部节点。

## 14. Reconciliation anchors

| Anchor | Input or condition | Expected result | Verification |
| --- | --- | --- | --- |
| `ODT-RA-1` | sites A/B/C | 分别返回 `0/1/2`，各 6 次比较 | `odt_test` |
| `ODT-RA-2` | 三个可精确表示的 bisector midpoints | binary64 score tie 分别返回 A、A、B | `odt_test` |
| `ODT-RA-3` | 四个方向的矩形外点 | 返回 `outside`，短路为 1/2/3/4 次比较 | `odt_test` |
| `ODT-RA-4` | 20,000 seeded random points + 54 deterministic cases | 8-ULP band 外与精确 oracle 相同；带内返回有效 region | Python differential test |
| `ODT-RA-5` | bad topology、non-finite weight/input、non-finite computed score | 返回明确错误且失败不改 result | `odt_test` + FIL-C |

## 15. Evidence and unknowns

[`evidence/README.md`](evidence/README.md) 记录 native、Zig、sanitizer、
FIL-C、Python differential 和 document DAG 的实际结果。

证据只支持本地七节点 evaluator 与三站点模型。它没有：

- 复现 apart 的 DuckDB benchmark；
- 验证 1,000-cell builder 的质量或性能；
- 证明 uncertainty band 内与精确最近点 region 一致，或不同浮点实现的边界结果
  bit-identical；
- 测量 cache、branch predictor、SIMD 或 `fixed_depth`；
- 验证动态更新、并发访问或持久化。

若继续探索性能，下一步应把 evaluator 与 brute-force polygon scan 放到同一批
目标机数据上，固定编译器、CPU、输入分布和结果 checksum 后再做 benchmark。
