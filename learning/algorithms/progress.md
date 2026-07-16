# Progress · 算法进度

> 唯一进度真相源。Agent 每次 `@leetcode` 先读本文件。

## 1. 当前指针

| 字段 | 值 |
| --- | --- |
| next_id | 3 |
| next_slug | （下一免费题，开始时确认） |
| next_title | |
| phase | not-started |
| language | rust |
| free_only | true |
| platform | https://leetcode.cn |

## 2. 统计

| 指标 | 值 |
| --- | ---: |
| accepted | 2 |
| local-pass only | 0 |
| premium-skip | 0 |
| skip | 0 |
| in_progress | 0 |

## 3. 题录

| id | slug | title | difficulty | status | patterns | updated |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | two-sum | 两数之和 | Easy | accepted | hashmap-complement | 2026-07-15 |
| 2 | add-two-numbers | 两数相加 | Medium | accepted | linked-list-carry | 2026-07-16 |

## 4. 最近会话备忘

- 2026-07-15：Algorithms Lab 骨架落地。
- 2026-07-15：题 1 two-sum 理解测 + Rust 一遍哈希 + 本地测通过；leetcode.cn **AC**。
- 2026-07-16：题 2 Lab Pass → 按位+carry → **AC** 1569/1569 · 5ms(~4%) · 2.25MB(~70%)。榜百分比噪声大，复杂度已最优。

## 5. 更新规则

1. 开始一题：`phase=teaching`，`in_progress` 填题号。  
2. 理解测过：`phase=coding`。  
3. 本地测过：`status=local-pass`。  
4. leetcode AC：`status=accepted`，`accepted++`，`next_id` 推进。  
5. 会员题：`premium-skip`，自动下一免费题。  
