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

#### 5.1 HTML 交互学习页 `learn.html`

共享资源：`assets/lab.css` + `assets/lab.js`（相对路径 `../../assets/`）。

**页面必须包含（教育场景，不为技巧而技巧）：**

| 区块 | 目的 |
| --- | --- |
| 学习怎么用 | 自学 / 复习 / 卡关 三种用法 |
| 应用场景 | 现实/工程类比；说明「主推结构在优化什么」 |
| 多种解法 · 发散 | ≥2 条路径 + 取舍表（暴力基线、主推、变体） |
| 主推图解动画 | **skill 绘图 storyboard**（必做）+ 可选短 hand-trace |
| 约束一变 | 数据规模、是否有序、流式等 → 解法如何变 |
| 理解测 | 统一提交（见下） |
| 埋点 | `lab.js` 本地 telemetry → AI 洞察 |

**产品级内容规范（硬约束）：**

1. **交互动画 = 图，不是字**  
   - 主推路径必须用 `[data-storyboard]` + 小黑 skill 生成的帧图。  
   - **禁止**用纯文字 `.stepper` 冒充「动画」；文字只可做 `figcaption` 或 `pre.walk` 手推。  
2. **术语中英策略**  
   - 专业结构/算法名保持英文：`HashMap`、`complement`、`carry`、`two-pointers`、`dummy head`、`DFS/BFS/DP`…  
   - 叙述与教学用语用中文；不要硬译成蹩脚中文（如「哈希映射表」「补数查找器」）。  
3. **提交剪贴板**  
   - 全对时复制内容必须含：**题号**、**slug**、**用户用时**、时间戳、`nextHint`。  
   - 由 `assets/lab.js` 自动生成，例如：  
     `[Lab Pass] #1 two-sum · 两数之和 · 用时 3m12s · 2026-…`  
4. **行为埋点 → AI 核心**  
   - `lab.js` 记录：section 停留/回看、tab、details、storyboard 帧、答题改选、提交/重试、滚动深度。  
   - 默认 **beacon** 到 `http://127.0.0.1:9090/api/lab/events`（port-manager）。  
   - `interest` = 高 dwell；`confusion` = 反复 reentry / answer_flip / 帧回看。  
   - Agent 写代码前：`curl http://127.0.0.1:9090/api/lab/coach?problemId=N` 读 `coachPrompt`。  
   - 用户也可点「AI 洞察」复制本地 + remote brief。  
5. **测验交互**  
   - 作答过程不公布对错；统一提交；有错展开解析 + 再来一次。  
   - **禁止泄题**：`placeholder`/题干不得写出标准答案。  
6. **访问**  
   - 推荐 `http-port-manager` 的 `:8000` algorithms 服务；改 HTML 后跑 `tools/http-port-manager/sync.sh`。

#### 5.2 小黑图解（storyboard 帧）

- 使用 skill **`ian-xiaohei-illustrations`**（`image_gen`），**每帧一张 16:9 图**。  
- 关键认知锚点 2–4 帧；保存到 `problems/NNNN-slug/assets/`。  
- 写入 `learn.html` 的 `[data-storyboard] .sb-frame`；分析文也可引用。

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
