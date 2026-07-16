# xiaohei-local

**Local-capability first** · 16:9 正文配图本地引擎。

| | |
| --- | --- |
| **仓库位置** | `recallfs/projects/xiaohei-local` |
| **版本** | **0.8.0** |
| **默认角色** | **`snoopy`** — 漫画史努比感：白身、黑耳、粗黑线、极简 |
| **依赖** | **uv + 项目 venv** |

> 造型受 Peanuts / Snoopy **启发**（同人式线稿），非官方授权形象。

## Setup

```bash
cd projects/xiaohei-local
uv venv .venv && source .venv/bin/activate
uv sync
```

## CLI

```bash
uv run xiaohei-local info
uv run xiaohei-local list-characters
# snoopy | xiaohuang | xiaohei

# 默认漫画狗
uv run xiaohei-local render --scene book_corrigendum -o /tmp/out.jpg

# 旧黄狗 / 小黑
uv run xiaohei-local render --scene lego_first --character xiaohuang -o /tmp/y.jpg
uv run xiaohei-local render --scene lego_first --character xiaohei -o /tmp/h.jpg

uv run xiaohei-local render-cube-anchors -o /tmp/cube/
uv run xiaohei-local showcase -o /tmp/sheet.jpg
```

## 角色

| id | 说明 |
| --- | --- |
| **`snoopy`** | **默认**：白身黑耳、侧脸黑鼻、极简漫画线（Snoopy 感） |
| `xiaohuang` | 旧版黄毛 Q 狗（legacy） |
| `xiaohei` | 经典黑豆 |

## v0.8 为什么重画

上一版黄毛 Q 版过「贴纸萌」，不像漫画。v0.8：

- 白 + 黑耳 + 粗轮廓
- 去掉腮红/吐舌/bokeh/爱心轰炸
- 箭头改砖红色（漫画分格感）
- 纯白 strip 纸底

## Scenes

| id | 锚点 |
| --- | --- |
| `book_corrigendum` | 书 / 勘误表 |
| `lego_first` | 先砖后塔 |
| `grain_vs_tumble` | grain≠tumble |
| `planned_not_shipped` | 规划中≠已交付 |
