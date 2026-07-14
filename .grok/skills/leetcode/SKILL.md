---
name: leetcode
description: >
  RecallFS Algorithms Lab workflow for leetcode.cn free problems in Rust.
  Teach-first: analyze, community inspiration, HTML learn.html quiz, xiaohei
  illustrations, then implement and help submit. Use when user says workflow
  @leetcode, /leetcode, 算法第N题, 做第N题, leetcode, two-sum, 刷题, or
  continues algorithm training in learning/algorithms.
metadata:
  short-description: "LeetCode free problems · Rust · teach-first workflow"
---

# LeetCode Workflow (RecallFS)

## 1. 必读路径

| 文件 | 作用 |
| --- | --- |
| `learning/algorithms/WORKFLOW.md` | 完整阶段流水线 |
| `learning/algorithms/progress.md` | 进度唯一真相源 |
| `learning/algorithms/README.md` | 模块入口 |
| `learning/algorithms/catalog.md` | 题号索引 |
| `skills/doc.md` | 文档与 ASCII graph 规范 |
| `ian-xiaohei-illustrations` skill | 算法认知配图 |

工作目录默认：`learning/algorithms/`（Cargo workspace 根）。

## 2. 触发解析

从用户输入提取：

- 题号：`1` / `第 1 题` / `@leetcode 1`
- 或 slug：`two-sum`
- 未指定时读 `progress.md` 的 `next_id`

## 3. 执行清单（按序）

```text
0 解析题号
1 读 progress.md；确认 free_only
2 拉 leetcode.cn 题面 + 题解/评论灵感
3 bash scripts/new-problem.sh ...（若目录不存在）
4 写 analysis.md（结论先行 + ASCII）
5 填 learn.html；用 ian-xiaohei-illustrations 生成 1–3 张图到 assets/
6 【闸门】等用户完成理解测或明确跳过
7 Rust 实现 + cargo test
8 协助 leetcode.cn 提交（open 题页 + 可粘贴代码；不假装 AC）
9 notes.md + patterns/ + progress.md
```

### 3.1 脚手架

```bash
bash learning/algorithms/scripts/new-problem.sh <id> <slug> "<中文标题>" <Easy|Medium|Hard> ["English Title"]
```

### 3.2 网络

下载慢 / GitHub 超时 → skill **`network-accel`**（`socks5h://127.0.0.1:2080` 或国内镜像）。  
Rust 工具链优先 rsproxy / USTC 镜像。

### 3.3 教学闸门（硬约束）

在用户说「理解测完成」或「跳过测验，直接写代码」之前：

- 可以引导、拆解、给提示  
- **禁止**一次性贴出完整可提交 AC 代码  

### 3.4 Rust

- 语言：**仅 Rust**
- 签名对齐 leetcode.cn 模板（`impl Solution`）
- 验证：`cargo test -p pNNNN_<slug_us> --manifest-path learning/algorithms/Cargo.toml`
- 清晰优先；需要时再优化常数

### 3.5 提交

```bash
open "https://leetcode.cn/problems/<slug>/"
```

- 用户保持浏览器登录  
- 提供可粘贴提交块；根据 WA/TLE 迭代  
- 结果写入 `meta.md` / `notes.md` / `progress.md`  
- 无浏览器自动化时 **不得** 声称已 AC

### 3.6 会员题

`meta.status=premium-skip` → 更新 progress → 自动尝试下一免费题（告知用户）。

## 4. 单题完成定义

- [ ] analysis.md  
- [ ] learn.html（或用户跳过）  
- [ ] assets 配图或说明为何无图  
- [ ] cargo test 通过  
- [ ] 提交结果已记录  
- [ ] notes + patterns + progress  

## 5. 相关 skills

| skill | 用途 |
| --- | --- |
| `ian-xiaohei-illustrations` | 小黑 16:9 认知图 |
| `network-accel` | 代理 / 镜像 |
| `doc`（repo `skills/doc.md`） | 文档结构 |

## 6. 开场话术（建议）

开始一题时先给用户：

1. 题号 / 标题 / 链接 / 难度  
2. 当前 phase  
3. 今日路径：分析 → 打开 learn.html → 理解测通过 → 写 Rust → 提交  
4. 本地命令：`open .../learn.html` 与 `cargo test -p ...`
