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
5 填 learn.html（场景+多解+storyboard 小黑帧+测验+埋点）
6 【闸门】等用户完成理解测（剪贴板含题号/用时）或明确跳过
6b 拉取 coach brief 再讲解/写代码（见 3.7）
7 Rust 实现 + cargo test
8 协助 leetcode.cn 提交（open 题页 + 可粘贴代码；不假装 AC）
9 notes.md + patterns/ + progress.md
```

### 3.0 learn.html 内容规范（必做 · 产品级）

- 复用 `learning/algorithms/assets/lab.css` + `lab.js`。
- **应用场景**：真实/工程类比；写清「为何值得用某结构，而非炫技」。
- **多种解法**：至少暴力基线 + 主推 + 1 条变体；对比表写清换到/丢掉什么。
- **主推动画**：必须是 **skill 绘图 storyboard**（`[data-storyboard]` + 小黑图帧），禁止纯文字 stepper 冒充动画。
- **术语**：专业名保持英文（HashMap / complement / carry / two-pointers …）；叙述用中文。
- **发散**：约束变化时解法如何变。
- **测验**：只在最终提交后显示对错；全对剪贴板 = **题号 + slug + 用时 + nextHint**；失败展示解析 + 再来一次。
- **埋点**：静默后台 only（无用户分析 UI）。`LAB_TELEMETRY.summary()` / `understanding` → coach API。
- **AI 核心**：写代码前 `curl .../api/lab/coach?problemId=N`，用 `understanding.level` + `confusion` 评估掌握度；禁止无差别贴模板。
- 禁止：答题过程中即时亮正确答案。
- 禁止：`placeholder` / 题干把标准答案写出来；只用中性格式提示。

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

### 3.7 AI Coach（产品核心）

用户做过 `learn.html` 后，**写代码前**优先拉取行为 brief：

```bash
curl -fsS "http://127.0.0.1:9090/api/lab/coach?problemId=<N>"
```

| 字段 | 用途 |
| --- | --- |
| `interest` | 高停留 section → 用户感兴趣 |
| `confusion` | reentry / answer_flip / 帧回看 → 卡点 |
| `quiz` | 是否通过、分数、提交次数 |
| `talkingPoints` / `coachPrompt` | 直接可作 system 提示 |

规则：

1. 先处理 `confusion`，再强化 `interest` 概念。  
2. `quiz.passed=false` 时继续教学闸门，**禁止**完整 AC 代码。  
3. 术语保持英文（HashMap / complement / carry …）。  
4. 管理台 http://127.0.0.1:9090/ 可看「学习会话」并复制 coach brief。  
5. 前端默认 `POST http://127.0.0.1:9090/api/lab/events`（可在 lab-config 改 `telemetryEndpoint`）。

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
