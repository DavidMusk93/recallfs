# Lab Storyboard — SOTA 轮播（为何别人不闪）

## 结论

手机上「像刷新/重绘」不是 `location.reload`，而是 **主线程每 2s 做布局动画**。

业界 SOTA 与我们旧实现的差：

| | 业余 slideshow | SOTA carousel（现实现） |
| --- | --- | --- |
| 切帧 | `display:none/block` | 固定视口内 **横向 scroll** |
| 动画 | 每帧 `animation: rise` 重播 | 浏览器 **scroll / snap**（合成层） |
| 高度 | 随内容抖动 | **aspect-ratio 锁死**，不推文章 reflow |
| 自动播 | `setInterval` 改 DOM | `scrollTo` + 仅在 **可见/前台** 时 |
| 手势 | 自己绑 touch | 原生滑动 + scroll-snap |
| 无障碍 | 弱 | region/slide、dots、键盘 |

## 架构

```text
[data-storyboard]  contain: layout paint
        │
        ▼
   .sb-stage          ← overflow-x: auto; scroll-snap-type: x mandatory
        │                 transform: translateZ(0); 固定 aspect-ratio
        ├── .sb-frame ×N  flex: 0 0 100%; snap-align: center
        │       img 16:9 + figcaption
        └── controls / dots
                │
                ▼
   JS: scrollTo / IO(active index) / autoplay timeout
       不碰 display，不重建 DOM
```

## 为何其他网页可以

1. **运动只在合成器**：scroll、transform、opacity —— 不触发整树 layout。  
2. **槽位尺寸稳定**：轮播盒子高度预定，下方文字不跟着跳。  
3. **离屏停算**：`IntersectionObserver` + `visibilitychange`。  
4. **尊重系统**：`prefers-reduced-motion`。  
5. **大图策略**：`decoding=async`、邻帧预热，而不是每 tick 解码新图 + 入场动画。

## 代码

- `learning/algorithms/assets/lab.css` — scroll-snap stage  
- `learning/algorithms/assets/lab.js` — `initStoryboard()`  
