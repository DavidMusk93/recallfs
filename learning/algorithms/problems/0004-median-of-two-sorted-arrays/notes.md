# 4. 寻找两个正序数组的中位数 · notes

## 一句话

在短数组上二分 cut \(i\)：\(j=h-i\)，合法当 \(A_{i-1}\le B_j\) 且 \(B_{j-1}\le A_i\)；哨兵 \(\pm\infty\)。

## 卡点回顾（Lab coach）

| 信号 | 映射 | 实现里怎么钉 |
| --- | --- | --- |
| q6 摇摆 2 ↔ 2.5 | 偶数要平均 | `(max_left + min_right) / 2.0` |
| q3 摇摆只勾一条交叉 | 两条都要 | 两个 if 分支 + else 命中 |
| q5 长停 / boundary 高 dwell | 端点 | `i==0 → MIN`，`i==m → MAX` |
| q7 曾选「必须平均」 | 奇数只取 \(L_{\max}\) | `(m+n) % 2 == 1` |
| why-short | 短侧二分 | 先 `m ≤ n` 再搜 |

## Rust

- 签名：`find_median_sorted_arrays(nums1, nums2) -> f64`
- `lo/hi` 用 `isize`，避免 `hi = i-1` 在 `usize` 下溢出
- 值域哨兵用 `i32::MIN/MAX`（题面值域在 \(\pm10^6\) 内安全）

## 提交后

- 本地：`cargo test -p p0004_median_of_two_sorted_arrays`
- leetcode.cn：待贴代码提交
