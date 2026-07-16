# xiaohei-local

**Local-capability first** · Ian 小黑 16:9 正文配图本地引擎。

| | |
| --- | --- |
| **仓库位置** | `recallfs/projects/xiaohei-local`（权威源，随 recallfs 推送） |
| **版本** | **0.7.0** |
| **默认角色** | **小黄狗 `xiaohuang`**（Q 版可爱黄狗）；可选 `xiaohei` |
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
uv run xiaohei-local list-characters

# 默认小黄狗
uv run xiaohei-local render --scene book_corrigendum -o /tmp/out.jpg

# 经典小黑
uv run xiaohei-local render --scene book_corrigendum --character xiaohei -o /tmp/out-hei.jpg

uv run xiaohei-local render-cube-anchors -o /tmp/cube-anchors/ --character xiaohuang
uv run xiaohei-local render-spec examples/shots-example.json
```

## 角色

| id | 说明 |
| --- | --- |
| **`xiaohuang`** | **默认**：Q 版黄狗，垂耳、双高光、项圈吊牌、吐舌、地面阴影 |
| `xiaohei` | 经典黑豆 IP（skill DNA 兼容） |

## v0.7 优化要点（honey edition）

| 项 | 说明 |
| --- | --- |
| **氛围** | 暖色 bokeh 光斑 + 星 + **小爱心** |
| **小狗** | 分层毛色、更湿润眼睛、吊牌高光、爪印轨迹 |
| **姿态** | + **love**（头顶三颗心） |
| smooth + showcase | 仍保留 v0.6 能力 |

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
