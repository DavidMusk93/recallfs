# RecallFS · Agent Rules

本仓库是工程知识库 + 算法训练营。Agent 在此工作必须遵守下列规则。

## 1. 改动及时提交（硬约束）

| 规则 | 说明 |
| --- | --- |
| **及时 commit** | 完成一个可独立描述的单元（骨架 / 单题 / 一项 workflow 优化 / 一批 docs）后 **立即** `git commit`，不要攒大批未提交改动。 |
| **粒度** | 一步一提交；message = short subject + 空行 + long details（完整句子）。 |
| **及时 push** | commit 后 **尽快** `git push origin <branch>`（默认 `master`）。推送失败（如 SSH key）须在回复里明确说明，不得假装已推送。 |
| **不混装** | 骨架与业务题、无关重构不要塞进同一 commit。 |
| **可提交内容** | 算法模块 `learning/algorithms/`（含 progress、patterns、单题 notes/html/rs）、skills、AGENTS、rules、designs 等长期沉淀 **应入库**。 |
| **勿提交** | `target/`、`.tmp/`、密钥、大体积无关二进制；遵守 `.gitignore`。 |
| **docs 例外** | 根 `rules.md` 写「部分 docs 可不进 git」：指远端业务仓同步的噪音文档；**本仓库主动写的 learning/skill/design 不在此列，要提交。** |

推荐节奏：

```text
改完一个单元 → git status/diff → commit → push → 再开下一单元
```

## 2. 仓库地图

| 路径 | 角色 |
| --- | --- |
| `rules.md` | 全局工程与学习态度 |
| `skills/` | 通用 agent 技能 |
| `projects/` | 业务系统文档镜像 |
| `designs/` | 跨项目设计 |
| `learning/algorithms/` | LeetCode 算法训练（见该目录 `AGENTS.md`） |
| `.grok/skills/leetcode/` | `/leetcode` workflow skill |

## 3. 算法训练摘要

详情：`learning/algorithms/AGENTS.md` 与 `WORKFLOW.md`。

- 仅 leetcode.cn **免费题**；语言 **Rust only**。
- 触发：`/leetcode <n>` / `workflow @leetcode <n>`。
- **教学闸门**：`learn.html` 未通过前不贴完整 AC 代码。
- HTML：场景 + 多解法 + 统一提交测验；禁止 placeholder 泄题。

## 4. 网络

下载慢 / GitHub 超时：用户 skill **`network-accel`**（`socks5h://127.0.0.1:2080` 或国内镜像）。

## 5. 行为

- 结论先行；复杂流程用 ASCII graph；对比用表格。
- 不发明「已 AC / 已 push」；无浏览器代操作 leetcode 登录态。
