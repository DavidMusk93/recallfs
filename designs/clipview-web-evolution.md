# ClipView (ClipFlow) Web 演进架构与设计方案

> 本设计由 `/ce-brainstorm` （Compound Engineering Brainstorming）流程沉淀，旨在将原 macOS SwiftUI 单体应用演进为 **macOS 后台服务 (Swift Daemon) + 现代化 Web 客户端 (SPA)** 架构。

---

## 1. 演进背景与痛点分析

| 原 ClipView (ClipFlow) 痛点 | 演进后的目标方案 |
| --- | --- |
| **原生 GUI 体验受限与 UI 欠佳** | 移至 Web 端，采用现代 Web 视觉语言（Dark Mode、微动画、响应式布局），提供流畅的高端体验 |
| **富文本与多媒体支持弱** | 引入强大 Web 富文本编辑器（如 Tiptap/ProseMirror），支持完整 RTF/HTML、代码高亮、图片预览与 OCR 交互 |
| **仅局限于 Mac 本地应用** | 后台提供 REST & WebSocket 服务，局域网内手机、iPad、其他 PC 均可实时访问与复制 |
| **原生系统能力剥离问题** | 保持 Swift 运行于 macOS 后台，继续保留 `Vision Framework` (离线 OCR)、`NSPasteboard` 监听及 `CloudKit/iCloud` 备份能力 |

---

## 2. 总体系统架构

系统划分为两部分：**macOS 后台守护服务 (Swift Daemon / Menu Bar App)** 与 **Web 客户端 (Vite + React SPA)**。

```text
+-------------------------------------------------------------------------+
|                              macOS 系统环境                              |
|                                                                         |
|  +-------------------+    +--------------------+    +----------------+  |
|  | NSPasteboard 监听 |    | Apple Vision OCR   |    | iCloud / CloudKit|  |
|  +---------+---------+    +---------+----------+    +-------+--------+  |
|            |                        |                       |           |
|            v                        v                       v           |
|  +-------------------------------------------------------------------+  |
|  |                 ClipView Swift Backend (Daemon Service)           |  |
|  |  - 剪切板去重与解析 (Text, HTML/RTF, Image)                       |  |
|  |  - DuckDB 高性能嵌入式存储 (全文索引 + 附件表)                       |  |
|  |  - HTTP & WebSocket Server (Hummingbird / Native Network)        |  |
|  +-----------------------------------+-------------------------------+  |
+--------------------------------------|----------------------------------+
                                       | HTTP API / WS Event Stream
                                       v
+-------------------------------------------------------------------------+
|                        Web 前端客户端 (Browser / Mobile)                 |
|                                                                         |
|  +----------------------+  +---------------------+  +-----------------+ |
|  | 剪切板卡片与时间轴 UI |  | Tiptap 富文本编辑器 |  | 代码高亮 (Prism)| |
|  +----------------------+  +---------------------+  +-----------------+ |
|  | OCR 交互与文本选择   |  | 一键发送至本机剪切板 |  | 快速搜索与筛选  | |
|  +----------------------+  +---------------------+  +-----------------+ |
+-------------------------------------------------------------------------+
```

---

## 3. 核心模块与设计细节

### 3.1 macOS 后台守护服务 (Swift Daemon)
* **剪切板监听与解析 (`ClipboardMonitor`)**:
  * 定时/ChangeCount 监听 `NSPasteboard`。
  * 支持多数据格式识别：`public.utf8-plain-text`, `public.html`, `public.rtf`, `public.png`, `public.file-url`。
  * MD5 / SHA256 哈希计算与增量去重，避免重复存储。
* **OCR 文字识别引擎 (`OCREngine`)**:
  * 监听到图片类型剪切板时，自动异步提交至 Apple `VNRecognizeTextRequest`。
  * 提取图片中的全部文本并建立索引用以全局搜索。
* **DuckDB 数据库与存储 (`DatabaseManager`)**:
  * 保持 DuckDB 嵌入式高性能优势，表结构扩展支持富文本字段与多媒体关联。
  * 数据表结构定义：
    ```sql
    CREATE TABLE IF NOT EXISTS clipboard_items (
        id VARCHAR PRIMARY KEY,
        content_type VARCHAR,        -- 'text', 'html', 'rtf', 'image', 'code'
        plain_text TEXT,             -- 纯文本或 OCR 提取结果
        rich_content TEXT,           -- HTML 或 RTF 数据
        image_path VARCHAR,          -- 本地图片相对路径
        ocr_text TEXT,               -- OCR 辨识文本
        hash VARCHAR,                -- 内容 Hash（去重）
        is_favorite BOOLEAN,         -- 是否收藏
        created_at TIMESTAMP,
        updated_at TIMESTAMP
    );
    ```
* **iCloud 备份与同步 (`iCloudSyncManager`)**:
  * 采用 CloudKit Private Database 或 `NSUbiquitousKeyValueStore` + iCloud Drive Document 沙盒。
  * 支持全量剪切板数据（文本、富文本、图片、OCR 文本）的跨设备同步与增量恢复。

### 3.2 REST API & WebSocket 实时通信
后台暴露出 JSON API 与 WebSocket 广播，以便前端实现无刷新实时感知：

* `GET /api/clips`: 分页与关键词检索历史记录。
* `GET /api/clips/:id`: 获取单条记录完整富文本/多媒体数据。
* `POST /api/clips`: 从 Web 端新增/写入本地系统剪切板。
* `DELETE /api/clips/:id`: 删除剪切板记录。
* `WS /ws/clipboard`: 实时 WebSocket 推送服务。系统剪切板变化或 OCR 完成时，立即向所有连接的 Web 客户端广播 `clip_created` / `clip_updated` 事件。

### 3.3 Web 客户端 (Rich UX/UI Frontend)
* **现代化交互布局**:
  * 左侧/顶部：搜索框、格式筛选器（文本/代码/富文本/图片/收藏夹）。
  * 中间：剪切板历史虚拟列表（Virtual List），保证数万条数据流畅滚动。
  * 右侧/弹窗：富文本与代码预览区，包含复制、编辑、导出一键操作。
* **富文本与代码支持**:
  * 整合 Tiptap (ProseMirror 驱动) 提供媲美 Notion/Feishu 的富文本渲染与编辑体验。
  * 支持 Markdown 快捷语法、表格、列表、代办事项及格式转换。
  * 代码剪切板自动语法高亮（Prism.js / Highlight.js）与格式化。
* **图片与 OCR 点选**:
  * 图片高精预览，叠加 OCR 辨识文本遮罩，支持在 Web 界面上拖拽框选图片中的局部文字直接复制。

---

## 4. 实施 roadmap (分阶段演进)

1. **Phase 1: Swift 后台 API 化与拆离**
   * 从原 SwiftUI App 中解耦业务逻辑，增强 `WebServer`（提供 REST API 与 WS 事件）。
   * 扩展 DuckDB Schema 以支持富文本和 OCR 字段。
2. **Phase 2: Web 前端搭建与富文本体验构建**
   * 构建现代化 SPA 网页应用，整合富文本组件与代码高亮。
   * 实现与 Swift 后台的 WS 双向实时通信（实时更新 + 反向写入系统剪贴板）。
3. **Phase 3: iCloud 备份与系统能力补全**
   * 接入 CloudKit / iCloud Drive 同步模块，实现数据的云端自动备份与恢复。
   * 优化跨终端（手机/平板浏览器）连通体验。
