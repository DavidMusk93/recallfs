# 1. 两数之和 · 实现心得

## 1. 一句话记忆钩子

> **我缺 need，以前见过吗？** 先查后写，O(n) 哈希。

## 2. 最终方案

| 项 | 内容 |
| --- | --- |
| 思路 | 一遍 `HashMap`：`need = target - x`，命中返回下标 |
| 时间 | O(n) |
| 空间 | O(n) |
| Rust 要点 | `HashMap<i32, i32>`；`enumerate`；`if let Some(&j) = seen.get(&need)` |

提交块见下方（与 `src/lib.rs` 同逻辑，不含测试）。

## 3. 踩坑

| 现象 | 原因 | 修正 |
| --- | --- | --- |
| 同值 `[3,3]` 错 | 先写后查可能自撞 | 先查后写 |
| 返回数值不是下标 | 题意是 indices | map 存下标 |
| `i as i32` 漏转 | 签名要 `Vec<i32>` | enumerate 转 i32 |

## 4. 本地 vs 提交

| 检查 | 结果 |
| --- | --- |
| `cargo test -p p0001_two_sum` | **4 passed** |
| leetcode 提交 | **accepted**（用户 2026-07-15 报告 AC） |
| 运行时 / 内存（若有） | — |

## 5. 可迁移点

- 以后看到 **「两数配对 / 和为 target」** 优先想到 **补数哈希**。
- 与 `patterns/hashmap-complement.md`。

## 6. 未尽

- 用户在 leetcode.cn 点提交后，把结果回填 `meta.md` / progress。
