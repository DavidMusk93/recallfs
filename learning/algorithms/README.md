# Algorithms Lab · 算法训练营

> 在 RecallFS 中重走算法升级路。目标不是刷题数，而是把 **可迁移的思维模式** 沉淀成长期能力。

## 1. 结论

| 项 | 约定 |
| --- | --- |
| 平台 | [leetcode.cn](https://leetcode.cn/problemset/)（已登录会话由你维护） |
| 题库范围 | **仅免费题**；从编号 **1** 起顺序推进（可跳过已掌握，但需写 `skip` 原因） |
| 实现语言 | **Rust only**（agent-friendly，亦为你的主语言） |
| 触发方式 | `workflow @leetcode <题号>` / `/leetcode <题号>` / 「做第 N 题」 |
| 学习形态 | 分析 → 场景/多解法 HTML → **统一提交理解测** → 小黑图 → Rust → 心得 |
| HTML 服务 | `cd learning/algorithms && python3 -m http.server 8000`（Tailscale 可访问） |
| 执行 | 本地 `cargo test` 闭环后，再协助你在 leetcode.cn 提交 |

## 2. 目录地图

```text
learning/algorithms/
├── README.md                 # 本文件：模块入口
├── WORKFLOW.md               # 完整 workflow（人读 + agent 对齐）
├── progress.md               # 当前进度状态机（唯一真相源）
├── catalog.md                # 免费题索引与标签
├── Cargo.toml                # Rust workspace
├── patterns/                 # 跨题模式库（hashmap、双指针、DP…）
├── templates/                # 新题脚手架模板
├── problems/                 # 每题一个目录 0001-two-sum/
│   └── NNNN-slug/
│       ├── meta.md           # 元数据、链接、状态
│       ├── analysis.md       # 题意、约束、思路与社区灵感
│       ├── learn.html        # 交互式理解测 / 问答
│       ├── notes.md          # 实现心得与坑
│       ├── assets/           # 小黑配图等视觉资产
│       ├── Cargo.toml
│       └── src/lib.rs        # LeetCode 解法 + 本地测试
├── scripts/                  # 脚手架与辅助脚本
└── assets/                   # lab.css / lab.js 与公共资产
```


## 3. 快速开始

```text
+---------------------------+
| 打开 leetcode.cn 并登录    |
+-------------+-------------+
              |
              v
+---------------------------+
| 对 agent 说:              |
| workflow @leetcode 1      |
+-------------+-------------+
              |
              v
+---------------------------+
| 完成 HTML 理解测          |
| 打开 problems/.../learn.html
+-------------+-------------+
              |
              v
+---------------------------+
| 本地 cargo test 通过      |
| 再提交 leetcode.cn        |
+---------------------------+
```

命令速查：

```bash
# 新题脚手架（由 agent 或你执行）
bash learning/algorithms/scripts/new-problem.sh 1 two-sum "两数之和" Easy

# 跑某一题本地测试
cargo test -p p0001_two_sum

# 在浏览器打开理解测
open learning/algorithms/problems/0001-two-sum/learn.html
```

## 4. 与 RecallFS 的关系

| 层 | 角色 |
| --- | --- |
| `projects/*` | 业务系统工程文档（stream_engine、gw2…） |
| `skills/*` | 通用工程技能（doc、bench、gdb…） |
| `designs/*` | 跨项目系统设计 |
| **`learning/algorithms/`** | **算法能力训练与模式沉淀**（本模块） |

算法阻塞时：先查 `patterns/`，再查相关题的 `notes.md` / `analysis.md`。

## 5. 原则

1. **先理解，后代码。** 未完成 `learn.html` 关键测前，不直接甩满分答案。
2. **证据驱动。** 复杂度、边界、错误解都要写进 `analysis.md` / `notes.md`。
3. **模式优先。** 每题结束后，回写 `patterns/`，服务未来思维阻塞。
4. **Rust 习惯。** 解法可提交 leetcode；本地用 `#[cfg(test)]` 覆盖官方样例 + 边界。
5. **只做免费题。** 遇到会员题：标记 `premium-skip`，跳到下一道免费题。
