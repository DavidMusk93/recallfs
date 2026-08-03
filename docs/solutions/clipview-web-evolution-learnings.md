# ClipView Web 服务演进最佳实践与经验沉淀 (Compound Notes)

> 本文档由 `/ce-compound` 自动生成，记录了 ClipView 从原生 macOS App 改造为 Web 服务架构的过程经验。

---

## 💡 关键架构模式

### 1. Swift Headless Daemon + REST/WS API
* **模式优势**: macOS 的系统级 API（`NSPasteboard` 剪贴板广播监听、`Vision Framework` 离线 OCR、`CloudKit` / `NSUbiquitousKeyValueStore`）在 Swift 中原生调用效率最高。通过将 Swift 作为无界面守护进程（Daemon），利用 `Network.framework` 暴露轻量级 REST API 与 CORS 支持，解耦前端显示。
* **HTTP API 设计**:
  * `GET /api/clips`: 分页与条件检索 DuckDB 剪贴板历史记录。
  * `POST /api/clips`: 写入新文本至 macOS 系统剪贴板（`NSPasteboard.general`）。
  * `DELETE /api/clips?id=<uuid>`: 从本地 DuckDB 中物理删除指定剪贴板记录。
  * `OPTIONS`: 返回 CORS `204 No Content`，允许局域网或 Vite 跨域通信。

### 2. 现代 Web UI (Glassmorphism & High-Contrast Code Highlighting)
* **响应式体验**: 使用 `CSS Variables` 与 `Backdrop-Filter` 建立现代感深色极简风格。
* **富文本与代码渲染**: 整合 Highlight.js 对代码片段自动高亮，并提供格式分类筛选器（All, Text, Rich Text/HTML, Image OCR）。

### 3. iCloud 增量同步机制
* **实现策略**: 通过 `NSUbiquitousKeyValueStore` 监听 `didChangeExternallyNotification`，当其他 macOS/iOS 设备有剪贴板变动时自动增量恢复至本地 DuckDB。
