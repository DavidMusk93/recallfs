# binary-partition-median

## 1. 结论

| 项 | 内容 |
| --- | --- |
| 何时用 | 两（多）段**已排序**序列上的 order-statistic / median，要求 log 级 |
| 核心 | 找合法 **partition（cut）**：数量钉死 + 跨数组顺序不变量 |
| 复杂度 | 在短侧二分 cut → \(O(\log\min(m,n))\) / \(O(1)\) |

## 2. 骨架

```text
保证 m ≤ n（短侧 = A）
h = (m+n+1)/2          # 全局 |L|
二分 cut i ∈ [0, m]:
  j = h - i            # B 的 cut 被钉死
  哨兵取 A_{i-1}, A_i, B_{j-1}, B_j
  A_{i-1} > B_j  → i 太大
  B_{j-1} > A_i  → i 太小
  else           → 合法
    odd  → max(A_{i-1}, B_{j-1})
    even → avg( maxL , min(A_i, B_j) )
```

不变量：

\[
L=\{A[0..i),\,B[0..j)\},\quad |L|=h
\]
\[
A_{i-1}\le B_j \ \land\ B_{j-1}\le A_i
\]

## 3. 例题

| id | slug | 备注 |
| --- | --- | --- |
| 4 | median-of-two-sorted-arrays | 中位数 = 合法 cut 的切缝 |

## 4. 常见变形与坑

| 坑 | 修正 |
| --- | --- |
| 把 \(i\) 说成「左半个数」 | \(i\) 是 **A 贡献给全局 L** 的 cut |
| 在长数组上二分 | \(j\) 可能越界；默认短侧 |
| 端点特判一堆 if | \(\pm\infty\) 哨兵，公式不变 |
| 奇数/偶数两套搜索 | 只改返回；\(h\) 的 +1 统一下中位数在 L |
| 偶数答成单个值 | 总长偶数 → \((L_{\max}+R_{\min})/2\) |
| 交叉只写一条 | 两条都要（或等价 \(L_{\max}\le R_{\min}\)） |
