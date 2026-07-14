# LeetCode Workflow

> 触发：`workflow @leetcode <题号>` · `/leetcode <题号>` · 「算法第 N 题」· 「做 two-sum」

Agent 执行时 **必须** 读取本文件与 `.grok/skills/leetcode/SKILL.md`。

## 1. 结论：阶段流水线

```text
+------------------+
| 0. 解析题号/slug |
+--------+---------+
         |
         v
+------------------+
| 1. 读 progress   |
|    校验免费题    |
+--------+---------+
         |
         v
+------------------+
| 2. 拉题面+讨论   |
|    leetcode.cn   |
+--------+---------+
         |
         v
+------------------+
| 3. 脚手架目录    |
|    templates/*   |
+--------+---------+
         |
         v
+------------------+
| 4. 分析+灵感     |
|    analysis.md   |
+--------+---------+
         |
         v
+------------------+
| 5. 教学层        |
|  learn.html      |
|  小黑流程图      |
+--------+---------+
         |
         v
+------------------+
| 6. 你完成理解测  |
|    (交互闸门)    |
+--------+---------+
         |
         v
+------------------+
| 7. Rust 实现     |
|    cargo test    |
+--------+---------+
         |
         v
+------------------+
| 8. leetcode 提交 |
|    协助执行      |
+--------+---------+
         |
         v
+------------------+
| 9. notes+patterns|
|    更新 progress |
+------------------+
```

## 2. 各阶段细则

### 0–1 启动

| 动作 | 说明 |
| --- | --- |
| 解析输入 | 数字 `1` / slug `two-sum` / 中文名「两数之和」 |
| 读 `progress.md` | 当前题、已完成、跳过列表 |
| 会员题 | 写 `meta.status=premium-skip`，推进下一免费题，**不实现** |

### 2 获取题面与灵感

1. 打开/抓取 `https://leetcode.cn/problems/<slug>/` 题面（中文）。
2. 读取「题解 / 评论」中高质量思路（哈希、双指针、复杂度等），**写进 analysis，不整段抄袭**。
3. 记录：输入输出、约束、样例、易错边界。

### 3 脚手架

```bash
bash learning/algorithms/scripts/new-problem.sh <id> <slug> "<中文标题>" <Easy|Medium|Hard>
```

生成 `problems/NNNN-slug/` 并登记 workspace member。

### 4 分析（analysis.md）

必须包含：

1. 题意一句话  
2. 形式化：输入 / 输出 / 约束  
3. 暴力解与复杂度  
4. 最优（或主推）思路与不变量  
5. 社区灵感来源（链接 + 吸收了什么）  
6. 与已有 `patterns/` 的关联  

使用 ASCII graph 画主流程（遵守 `skills/doc.md`）。

### 5 教学层（核心）

#### 5.1 HTML 交互理解测 `learn.html`

- 从 `templates/learn.html` 复制后填空。
- 结构建议：概念 → 手推样例 → 复杂度判断 → 陷阱多选 → 小代码填空。
- **默认隐藏标准答案**；用户点选/填写后即时反馈。
- 本地用 `open learn.html`；不依赖服务器。

#### 5.2 小黑流程图

- 使用 skill **`ian-xiaohei-illustrations`**。
- 为关键认知锚点生成 1–3 张 16:9 配图（不是 PPT 流程图）。
- 保存到 `problems/NNNN-slug/assets/`。
- 在 `learn.html` / `analysis.md` 中引用。

### 6 交互闸门

在用户完成理解测（或明确说「跳过测验，直接写代码」）之前：

- 可以引导、提示、拆解  
- **不要**一次性贴出完整可提交解  

闸门通过后进入实现。

### 7 Rust 实现

| 规则 | 说明 |
| --- | --- |
| 签名 | 对齐 leetcode.cn Rust 模板（`impl Solution`） |
| 本地测 | `#[cfg(test)]` 覆盖官方样例 + 边界 |
| 风格 | 清晰 > 炫技；需要时再写最优常数优化 |
| 验证 | `cargo test -p pNNNN_<slug_underscored>` |

### 8 在 LeetCode 执行

Agent 能力边界：

| 能做 | 不能默认假设 |
| --- | --- |
| `open` 对应题目页 | 已登录（由你在浏览器保持会话） |
| 生成可粘贴的 Rust 提交块 | 自动绕过登录 / 验证码 |
| 指导点击提交、读报错、改代码 | 无浏览器自动化时「假装已 AC」 |

提交后把结果写入 `meta.md`（`accepted` / `wrong-answer` / `tle` 等）与 `notes.md`。

### 9 沉淀

1. `notes.md`：踩坑、API 细节、一句话记忆钩子  
2. `patterns/`：新模式或强化旧模式  
3. `progress.md`：状态推进  
4. （可选）nmem：跨会话可检索的 pattern 要点  

## 3. 状态机（progress.md）

| status | 含义 |
| --- | --- |
| `todo` | 未开始 |
| `teaching` | 分析/理解测进行中 |
| `coding` | 实现中 |
| `local-pass` | 本地测试通过 |
| `submitted` | 已提交 leetcode |
| `accepted` | AC |
| `premium-skip` | 会员题跳过 |
| `skip` | 主动跳过（需 reason） |

## 4. 退出条件（单题完成）

- [ ] `analysis.md` 完整  
- [ ] `learn.html` 可用（或用户明确跳过）  
- [ ] ≥1 张关键思路配图（极简题可 1 张）或说明为何无图  
- [ ] `cargo test` 通过  
- [ ] leetcode 提交结果已记录  
- [ ] `notes.md` + `progress.md` 已更新  
