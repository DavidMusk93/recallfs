# 4. 寻找两个正序数组的中位数 · 分析

## 1. 结论

| 项 | 内容 |
| --- | --- |
| 一句话题意 | 两个**已排序**数组求合并后的 median；不必真合并。 |
| 主推思路 | **定位不变量 + binary search**：在短数组上二分左半个数 \(i\)；边界用 \(\pm\infty\) 哨兵统一。 |
| 时间 / 空间 | \(O(\log\min(m,n))\) / \(O(1)\) |
| 关联 pattern | `binary-partition-median` |
| 难度 | **Hard** · 免费 |
| 链接 | https://leetcode.cn/problems/median-of-two-sorted-arrays/ |

\[
\begin{aligned}
h&=\left\lfloor\frac{m+n+1}{2}\right\rfloor,\quad j=h-i \\
\text{valid}&\iff A_{i-1}\le B_{j}\ \land\ B_{j-1}\le A_{i} \\
\mathrm{median}&=
\begin{cases}
\max(A_{i-1},B_{j-1}) & m+n\text{ odd}\\
\frac{\max(A_{i-1},B_{j-1})+\min(A_{i},B_{j})}{2} & m+n\text{ even}
\end{cases}
\end{aligned}
\]

缺侧：\(i=0\Rightarrow A_{i-1}=-\infty\)，\(i=m\Rightarrow A_{i}=+\infty\)（\(B\) 同理）。

## 2. 题面形式化

### 2.1 输入 / 输出 / 约束

| 项 | 内容 |
| --- | --- |
| 输入 | `nums1` 长度 \(m\)，`nums2` 长度 \(n\)，均非降序 |
| 输出 | `f64` 中位数 |
| 规模 | \(0\le m,n\le 1000\)，\(1\le m+n\le 2000\)（以 leetcode.cn 为准） |
| 复杂度 | 目标 **log** 级（官方主推 \(O(\log\min(m,n))\)） |
| Rust | `find_median_sorted_arrays(nums1: Vec<i32>, nums2: Vec<i32>) -> f64` |

样例：

| nums1 | nums2 | 合并 | median |
| --- | --- | --- | ---: |
| `[1,3]` | `[2]` | \(1,2,3\) | 2.0 |
| `[1,2]` | `[3,4]` | \(1,2,3,4\) | 2.5 |

### 2.2 边界

- 一数组为空 → 单数组中位数。  
- 划分落在端点：\(i\in\{0,m\}\) 或 \(j\in\{0,n\}\) → 哨兵 \(\pm\infty\)。  
- 奇偶只影响**返回值**，不改搜索与不变量。

## 3. 思路演进

### 3.1 Merge 双指针 \(O(m+n)\)

归并到中位即停。正确但**不满足** log；作基线。

### 3.2 主推：不变量 + 二分划分

```text
保证 m ≤ n（对短数组二分）
h = floor((m+n+1)/2)

二分 i ∈ [0, m]:
  j = h - i
  用哨兵取 A_{i-1}, A_i, B_{j-1}, B_j
  if A_{i-1} > B_j  → i 太大
  else if B_{j-1} > A_i → i 太小
  else → 合法
       L_max = max(A_{i-1}, B_{j-1})
       R_min = min(A_i, B_j)
       odd  → L_max
       even → (L_max + R_min) / 2
```

| 对比 | Merge | 二分划分 |
| --- | --- | --- |
| 时间 | \(O(m+n)\) | \(O(\log\min(m,n))\) |
| 是否合并 | 是 | 否 |
| 难点 | 低 | 不变量理解 + 哨兵 |

### 3.3 为何只需交叉两条

\(A,B\) 各自有序 ⇒ 数组内部左 ≤ 右已保证。  
全局 \(\max(L)\le\min(R)\) 只可能在跨数组边界破 → 交叉两条。

### 3.4 为何 \(h\) 带 \(+1\)

把「下中位数」固定在左半：奇数 median \(=L_{\max}\)；偶数再与 \(R_{\min}\) 平均。搜索过程统一。

## 4. 社区灵感

| 来源 | 吸收点 |
| --- | --- |
| 官方「划分数组」 | 中位数 = 均匀分组后的边界 |
| 高频写法 | 短数组二分；\(\pm\infty\) 哨兵消特判 |
| 标签 | Array · Binary Search · Divide and Conquer |

## 5. 卡点预期

- 数量不变量：\(j=h-i\)（\(j\) 不算二分）  
- 顺序不变量：为何只需交叉两条  
- 违规方向：\(A_{i-1}>B_{j}\Rightarrow i\) 太大  
- 端点哨兵 \(\pm\infty\)  
- 奇偶只改返回  
