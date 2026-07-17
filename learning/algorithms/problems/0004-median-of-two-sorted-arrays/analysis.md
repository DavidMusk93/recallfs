# 4. 寻找两个正序数组的中位数 · 分析

## 1. 结论

| 项 | 内容 |
| --- | --- |
| 一句话题意 | 两个**已排序**数组 `nums1`、`nums2`，求合并后的 **median**（中位数）。 |
| 主推思路 | **划分（partition）+ 二分**：在较短数组上二分左半切分点 `i`，使左半总元素数 = `(m+n+1)/2`，且 `maxLeft ≤ minRight`。 |
| 时间 / 空间 | **O(log(min(m,n)))** / O(1)（题目要求 log 级，不能只 merge） |
| 关联 pattern | `binary-partition-median` |
| 难度 | **Hard** · 免费 |
| 链接 | https://leetcode.cn/problems/median-of-two-sorted-arrays/ |

```text
合并后中位数 = 把全体元素分成左右两半
  左半个数 = (m+n+1)/2   （奇数时中位数在左半 max）
  左半所有 ≤ 右半所有
找合法划分即可，不必真合并
```

## 2. 题面形式化

### 2.1 输入 / 输出 / 约束

| 项 | 内容 |
| --- | --- |
| 输入 | `nums1` 长度 m，`nums2` 长度 n，均非降序 |
| 输出 | `f64` 中位数 |
| 规模 | `0 ≤ m,n ≤ 1000`，`1 ≤ m+n ≤ 2000`（以 leetcode.cn 为准） |
| 值域 | 常见 `[-10^6, 10^6]` |
| 复杂度 | 目标 **O(log(m+n))** 级（官方主推 log(min(m,n))） |
| Rust | `find_median_sorted_arrays(nums1: Vec<i32>, nums2: Vec<i32>) -> f64` |

样例：

| nums1 | nums2 | 合并 | median |
| --- | --- | --- | ---: |
| `[1,3]` | `[2]` | `1,2,3` | 2.0 |
| `[1,2]` | `[3,4]` | `1,2,3,4` | 2.5 |

### 2.2 边界

- 一数组为空 → 退化为单数组中位数。  
- 总长奇数 / 偶数：奇数取左半 max；偶数取 `(maxLeft+minRight)/2`。  
- 划分落在数组端点：用 ±∞ 哨兵处理 `i=0` 或 `i=m`。

## 3. 思路演进

### 3.1 Merge 双指针 O(m+n)

归并到中位位置即停。正确但**不满足** log 要求；作基线。

### 3.2 主推：二分划分

```text
保证 m ≤ n（对短数组二分）
half = (m + n + 1) / 2

二分 i ∈ [0, m]：nums1 左半取 i 个
  j = half - i      ：nums2 左半取 j 个

Aleft = nums1[i-1] 或 -∞
Aright = nums1[i]   或 +∞
Bleft = nums2[j-1] 或 -∞
Bright = nums2[j]   或 +∞

若 Aleft ≤ Bright 且 Bleft ≤ Aright → 合法划分
  maxLeft = max(Aleft, Bleft)
  minRight = min(Aright, Bright)
  奇数 → maxLeft
  偶数 → (maxLeft + minRight) / 2
若 Aleft > Bright → i 太大，缩小
否则 → i 太小，增大
```

| 对比 | Merge | 二分划分 |
| --- | --- | --- |
| 时间 | O(m+n) | O(log min(m,n)) |
| 是否合并 | 是 | 否 |
| 难点 | 低 | 边界 + 交叉比较 |

## 4. 社区灵感

| 来源 | 吸收点 |
| --- | --- |
| 官方「划分数组」 | 中位数 = 均匀分组后的边界 |
| 高频写法 | 短数组二分；±∞ 哨兵 |
| 标签 | Array · Binary Search · Divide and Conquer |

## 5. 卡点预期

- 为什么 `j = half - i`  
- 为什么比较 **交叉**：`Aleft≤Bright` 与 `Bleft≤Aright`  
- 端点 `i=0/m` 的 ±∞  
- 奇偶总长时返回值不同  
