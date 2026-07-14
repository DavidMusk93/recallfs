# Patterns · 可迁移算法模式

> 目标：思维阻塞时先查这里，再回具体题的 `notes.md`。

## 1. 用法

| 动作 | 说明 |
| --- | --- |
| 新模式 | 新建 `patterns/<name>.md`（kebab-case） |
| 强化旧模式 | 追加「例题」表行，不覆盖旧结论 |
| 检索 | 按场景 / 数据结构 / 复杂度关键词搜 |

## 2. 目录（随进度增长）

| pattern | 一句话 | 例题 | 文件 |
| --- | --- | --- | --- |
| hashmap-complement | 扫 x 时查补数 need，先查后写 | 1 two-sum | [hashmap-complement.md](./hashmap-complement.md) |

## 3. 模板（新文件）

```markdown
# pattern-name

## 1. 结论
何时用 / 不变量 / 复杂度

## 2. 骨架（ASCII）
...

## 3. 例题
| id | slug | 备注 |
| --- | --- | --- |

## 4. 常见变形与坑
...
```

## 4. 与单题文档的关系

```text
patterns/xxx.md     <-- 可迁移抽象
       ^
       | 回写
problems/NNNN-*/notes.md + analysis.md
```
