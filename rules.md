# 规则
- 对于cpp 项目，代码风格严格遵守项目.clang-format 文件中的配置. 除了class,namespace 外，其余name 全部使用little-camel-case. 对于类的私有变量，以_ 结尾。
- 对于一个复杂需求，在docs/$subject 目录创建一个markdown 文件，说明需求、参考、实现、设计、执行计划。
- 设计流程分析式，使用ascii graph 表达流程。
- 我的目标是学习，docs 对我是一个学习的工具。解决了一个复杂的问题，可以梳理心得为文档，帮助他人理解。
- 算法训练在 `learning/algorithms/`：仅 leetcode.cn **免费题**、实现语言 **Rust**、触发 `workflow @leetcode <题号>` 或 `/leetcode`。教学闸门（learn.html）优先于给完整答案。详见该目录 `WORKFLOW.md` 与 `.grok/skills/leetcode`。

# 开发规范
- 测试驱动 & 追溯日志驱动
- 对于优化问题，切换到bench 模式，以结果来决定优化方向。如果你觉得没有思路，我可以为你提供素材。
- 根据plan，依次实现功能。一步一个提交。
- commit message format: short message & long details
- docs 中的文件不必提交到git 仓库。
- 算法模块的 progress / patterns / 单题 notes 属于长期能力沉淀，应纳入版本管理；`target/` 与临时产物忽略。

# 行为态度
- 我的目标是优秀的工程师，你的目标是最佳的ai 伙伴。
- 遇到困难，不要害怕，我们有无限的token/子弹。在学习中成长，成为这个领域中的专家。
- 不要被已有的实现束缚住，基于state-of-the-art 的实现，通过创新去解决问题。
- 简单，可扩展，通过组合去实现复杂的功能。
- 遇到复杂的问题，通过divide & conquer 来解决。
