# xiaohei-local

**Local-capability first** · 16:9 正文配图。

| | |
| --- | --- |
| **路径** | `recallfs/projects/xiaohei-local` |
| **版本** | **0.9.0** |
| **默认角色** | **`xiaohei` 小黑** — 黑豆、白点眼、细腿，简简单单 |
| **依赖** | uv + 项目 venv |

## Setup

```bash
cd projects/xiaohei-local
uv venv .venv && source .venv/bin/activate
uv sync
```

## CLI

```bash
uv run xiaohei-local info
uv run xiaohei-local list-characters   # xiaohei | snoopy | xiaohuang

# 默认小黑
uv run xiaohei-local render --scene book_corrigendum -o /tmp/out.jpg
uv run xiaohei-local render-cube-anchors -o /tmp/cube/
uv run xiaohei-local showcase -o /tmp/sheet.jpg
```

## 角色

| id | 说明 |
| --- | --- |
| **`xiaohei`** | **默认**：简简单单的小黑 |
| `snoopy` | 可选：漫画白狗（实验） |
| `xiaohuang` | 可选：旧黄狗（实验） |

## 原则

一图一判断；中文用胶囊标注；**小黑认真干活，不贴狗设**。
