# Algorithms Lab · Agent Rules

本目录是 RecallFS 的 **算法训练模块**。在此路径下工作时优先遵守：

1. 读 `WORKFLOW.md` 与 `progress.md`；触发 skill **`leetcode`**（`.grok/skills/leetcode`）。
2. **仅免费题**；**仅 Rust**；从 `progress.md` 的 `next_id` 推进。
3. **教学优先**：`learn.html` 闸门未过，不贴完整 AC 解。
4. 配图用 **`ian-xiaohei-illustrations`**；网络慢用 **`network-accel`**。
5. 文档风格对齐仓库根 `skills/doc.md`（结论先行、ASCII graph、表格对比）。
6. 单题产物落在 `problems/NNNN-slug/`；可迁移抽象回写 `patterns/`。
7. 改进度只追加/更新 `progress.md` 题录，不删历史。
8. **及时提交**：单题完成一阶段（scaffold / teaching 材料 / local-pass / accepted 回写）或 lab 模板优化后，按仓库根 `AGENTS.md` 立即 commit（并尽量 push）；不要把多题或大段 HTML 重构无限堆在工作区。

人类快速开始：

```text
workflow @leetcode 1
```
