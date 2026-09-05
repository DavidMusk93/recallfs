# Engineering Q&A

本目录记录可独立阅读、可长期复用的工程概念答疑。

| 主题 | 问题 |
| --- | --- |
| [从 C 的视角理解 Rust](rust-from-c.md) | 如何用 C 的内存、ABI 和成本模型掌握 Rust 的 ownership、borrowing、ADT、trait 与 unsafe？ |
| [Pointer Chasing](pointer-chasing.md) | 什么是 pointer chasing，为什么它影响 CPU cache，以及 Rust 中如何识别和优化？ |

## 写作要求

- 先给一句可检验的定义，再解释底层机制。
- 区分严格定义、常见近似说法和容易混淆的概念。
- Rust 示例应可独立编译，并对关键数据布局和访存依赖添加注释。
- 优化建议必须包含不适用场景和验证方法。
- 涉及性能时区分 layout、allocator、CPU 和生产指标。
