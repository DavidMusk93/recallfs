# s-ui

学习向文档：racknerd 上 **s-ui Panel + 内嵌 sing-box** 的架构、入站/出站心智模型与脱敏拓扑。

## 打开交互文档

```bash
open projects/s-ui/docs/index.html
# 或
python3 -m http.server 8765 --directory projects/s-ui/docs
# 浏览器访问 http://127.0.0.1:8765/
```

主入口：[`docs/index.html`](./docs/index.html)

## 内容

| 能力 | 说明 |
| --- | --- |
| 流量管道点击 | 客户端 → Inbound → 核心 → Outbound → 目标 |
| 协议过滤表 | 本机 inbounds 按 VLESS / TUIC / Hysteria2 / CF / Reality 过滤 |
| 路径切换 | Cloudflare WS / RELAY / 直连 ORIGIN |
| 主题配图 | **小黑** 手绘（`ian-xiaohei-illustrations`），贴在对应章节；点击放大 |
| 搜索 / 主题 | 顶栏搜索；亮暗色（localStorage） |
| 自测题 | CF 属于入口路径还是 outbound |

## 设计风格

基于 [apple-design](https://github.com/emilkowalski/skills/tree/main/skills/apple-design)（WWDC 流体界面 / 材质 / 字体）：

- **材质**：frosted topbar / scrim / sheet 灯箱（`backdrop-filter` + 半透明）；侧栏更重、内容区更轻
- **字体**：系统字体；大标题负 tracking + 紧 leading；正文舒适 leading
- **反馈**：pointer-down 即反馈，`scale(0.92–0.98)` ~100ms；路径切换 ease-out 可中断
- **空间一致**：灯箱 materialize（scale + blur 同进同出）；移动端 drawer 同路径滑入/出
- **无障碍**：`prefers-reduced-motion` / `reduced-transparency` / `prefers-contrast`
- **正文默认可视**；概念图为小黑位图（本地 assets，随章节）

## 目录

```text
projects/s-ui/
├── README.md
└── docs/
    ├── index.html      # 交互学习站（单页）
    └── assets/         # 配图
```

配置均已脱敏。凭据不入文档。

## 项目记忆（教训）

- [本地 HTML 配图白块/慢：根因不是压缩](./docs/lessons/local-html-images-root-cause.md)  
  结构脱节 + `opacity:0`/批量 decode；local 页禁止用「压图」当默认排障。
