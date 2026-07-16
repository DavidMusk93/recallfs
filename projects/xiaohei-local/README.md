# xiaohei-local

**Local-capability first** · Ian 小黑 16:9 正文配图本地引擎。

| | |
| --- | --- |
| **仓库位置** | `recallfs/projects/xiaohei-local`（权威源，随 recallfs 推送） |
| **版本** | **0.3.0** |
| **依赖管理** | **uv + 项目 venv**（禁止全局 pip） |
| **用途** | 中文标注/拓扑必须正确可复现；Imagine 503 时的默认路径 |

Skill 入口仍在 `~/.grok/skills/ian-xiaohei-illustrations/`，实现以本目录为准。

## Setup

```bash
cd projects/xiaohei-local   # under recallfs root
uv venv .venv
source .venv/bin/activate
uv sync
```

字体（可选）：

```bash
export XIAOHEI_FONT=/path/to/SourceHanSansSC-Regular.otf
```

## CLI

```bash
uv run xiaohei-local info
uv run xiaohei-local list-scenes

uv run xiaohei-local render \
  --scene book_corrigendum \
  -o /tmp/out.jpg

uv run xiaohei-local render-cube-anchors -o /tmp/cube-anchors/
uv run xiaohei-local render-spec examples/shots-example.json
```

## v0.3 相对 v0.1 的文字优化

| 项 | 说明 |
| --- | --- |
| `ss=3` 默认 | 多级 LANCZOS 缩回 1280×720 |
| 分角色字体 | title / body / annot；优先 Hiragino Sans GB |
| `label()` 胶囊 | 圆角 + 阴影 + CJK tracking |
| 双重描墨 | 高 ss 下轻微叠字，字重更稳 |
| 端点缓抖 | 折线端点少抖，更干净 |
| JPEG | `subsampling=0` |
| `tilt` | 次要批注可微倾 1.2° |

## Scenes

| id | 锚点 |
| --- | --- |
| `book_corrigendum` | 书=DAG / 勘误表=清单 |
| `lego_first` | 先砖后塔 |
| `grain_vs_tumble` | grain≠tumble |
| `planned_not_shipped` | 规划中≠已交付 |

## Agent 规则

1. 默认 `xiaohei-local`（本目录）  
2. Imagine 可选增强  
3. 字段表用 ASCII，不进图  
4. 新场景只加 `scenes.py` 注册  

详见 skill：`ian-xiaohei-illustrations/references/local-capability.md`
