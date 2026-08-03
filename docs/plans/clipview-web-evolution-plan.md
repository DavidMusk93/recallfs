# ClipView Web 演进实施计划书

> 关联架构设计: [`designs/clipview-web-evolution.md`](file:///Users/bytedance/Documents/trae_projects/recallfs/designs/clipview-web-evolution.md)
> Readiness: implementation-ready

---

## 🎯 实施目标
将 ClipView 从纯 macOS SwiftUI 应用改造为：
1. **macOS 后台守护服务 (Swift Daemon / Backend)**：保留 `NSPasteboard` 剪贴板自动监听、`Vision Framework` 离线 OCR、DuckDB 高性能嵌入式存储与 iCloud 双向同步；对外暴露 REST API & WebSocket 服务。
2. **独立 Web 前端 (SPA)**：采用现代 Web 技术栈构建全功能剪贴板管理界面，提供 Tiptap 富文本编辑器、代码语法高亮、图片 OCR 框选复制与局域网多端协同。

---

## 🛠 单元任务拆解 (Units)

### U1: Swift 后台 API 与服务解耦 (Backend Service)
- **目标**: 将 Swift 剪贴板监听、DuckDB 数据读写与 HTTP/WebSocket 服务封装为独立的 Service 模块。
- **文件路径**:
  - `projects/ClipView/ClipFlow/WebServer.swift`
  - `projects/ClipView/ClipFlow/ClipboardMonitor.swift`
  - `projects/ClipView/ClipFlow/DatabaseManager.swift`
- **实施步骤**:
  1. 在 `DatabaseManager.swift` 中新增 `rich_content`（富文本 HTML）与 `ocr_text` 字段支持。
  2. 扩展 `WebServer.swift` 支持 WebSocket 广播服务 (`/ws/clipboard`)，当系统剪贴板更新或 OCR 解析完成时实时广播新条目。
  3. 增加 REST API 路由：`GET /api/clips`, `POST /api/clips`, `DELETE /api/clips/:id`, `GET /api/clips/:id/image`。
- **验收验证**:
  - 启动 Swift 后台服务，运行 `curl http://localhost:8080/api/clips` 能正确返回 DuckDB 中存储的 JSON 列表。

---

### U2: 现代化 Web 前端 (React/Vite SPA) 搭建
- **目标**: 构建现代化 Web 客户端基础结构与样式。
- **文件路径**:
  - `projects/ClipView/web/` (前端工程根目录)
  - `projects/ClipView/web/src/App.tsx`
  - `projects/ClipView/web/src/components/ClipboardList.tsx`
  - `projects/ClipView/web/src/components/RichTextEditor.tsx`
- **实施步骤**:
  1. 初始化 React + TypeScript + Vite 前端工程。
  2. 设计全功能卡片与时间轴布局，支持深色模式（Dark Mode）、动画效果与快捷键（上下选择，回车复制）。
  3. 建立 WebSocket 连接，实现剪贴板实时无刷新上屏。
- **验收验证**:
  - 前端以 `npm run dev` 运行，在 macOS 复制文本后，前端页面立即收到 WebSocket 消息并动态渲染卡片。

---

### U3: Tiptap 富文本编辑器与代码高亮整合
- **目标**: 为剪贴板提供强交互富文本编辑、格式转换与代码语法高亮。
- **文件路径**:
  - `projects/ClipView/web/src/components/RichTextEditor.tsx`
  - `projects/ClipView/web/src/components/CodePreview.tsx`
- **实施步骤**:
  1. 引入 Tiptap / ProseMirror，支持 HTML、Markdown 格式展示与修改。
  2. 集成 Prism.js，自动检测语言并渲染高亮代码块。
  3. 增加一键「写回系统剪贴板」功能（调用 `POST /api/clips`）。
- **验收验证**:
  - 打开一则代码或富文本记录，能正确语法高亮，修改后点击复制能在 macOS 系统剪贴板中粘贴出修改后的内容。

---

### U4: 图片 OCR 文本框选与 iCloud 同步扩展
- **目标**: 在 Web 界面上提供图片 OCR 点选复制，并在 Swift 端补全 iCloud 同步。
- **文件路径**:
  - `projects/ClipView/web/src/components/ImageOcrViewer.tsx`
  - `projects/ClipView/ClipFlow/iCloudSyncManager.swift`
- **实施步骤**:
  1. 在前端展示剪贴板图片时，异步获取 OCR 解析得到的文本坐标分布或合并文本。
  2. 在 Swift 端创建 `iCloudSyncManager.swift`，利用 `NSUbiquitousKeyValueStore` 或 `CloudKit` 同步剪贴板历史记录。
- **验收验证**:
  - 复制包含文字的图片后，Web 端能展示识别出的文本，支持选中文本复制。
