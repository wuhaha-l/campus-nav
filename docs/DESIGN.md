# 项目展示页 / 设计说明

校园地图导航系统的单页作品展示站，静态站点，直接打开 `index.html` 即可运行。本目录同时是 GitHub Pages 的发布目录。

## 设计定调

- **受众**：招聘方与技术面试官，30 秒内看懂项目技术含量，并能一键复制简历文案。
- **方向**：工业实用（Industrial / 导航控制台）。深墨底 + 仪表盘信息密度 + 等宽数字 + 发丝边框，非对称首屏（左大标题、右截图向右溢出栅格）。
- **字体**：`Archivo Black` 做数字与代号，`Noto Sans SC` 做中文标题，`IBM Plex Sans` 正文、`IBM Plex Mono` 数据与标签。Google Fonts 加载失败时回退到系统中文字体。
- **配色**：`--color-bg #0d1113` 主导，`--color-accent #ff6a33` 单一强调色（对应程序里红色高亮路线），其余全部灰阶；网格线与噪点作氛围层。
- **记忆点**：可交互的「算法对照台」——同一 OD、两条等长路径，切换标准 Dijkstra / 状态扩展示意图，读出转向次数与罚时的变化。

## 结构

```
docs/
├── index.html              # 展示页入口：首屏 / 指标 / 亮点 / 算法台 / 图集 / 架构 / 简历文案 / 下载
├── styles/
│   ├── tokens.css          # 颜色、字阶、间距、阴影、动效变量
│   ├── base.css            # 重置、氛围背景、排版、焦点、入场与降级
│   └── components.css      # 组件与 1024 / 768 / 420px 响应式构成
├── scripts/main.js         # IntersectionObserver 入场、算法台切换、剪贴板复制
├── assets/                 # 截图、源码、设计报告、可执行文件
├── USAGE.md                # 项目操作说明
├── ALGORITHM.md            # 核心算法与面试讲解
└── DESIGN.md               # 本文件：设计定调与自检
```

## 运行 / 发布

- 本地：直接打开 `docs/index.html`（`file://` 下剪贴板 API 会走 `execCommand` 回退）。
- 静态服务：在仓库根目录运行 `python3 -m http.server 8080`，访问 `http://localhost:8080/docs/`。
- 线上：GitHub Pages 从 `main` 分支的 `/docs` 目录部署，地址 https://wuhaha-l.github.io/campus-nav/ ，push 后自动更新。
- 为什么不用 GitHub Actions：本机推送凭据为 OAuth App token，缺 `workflow` scope，无法推送 `.github/workflows/*.yml`。改用分支目录部署，零 workflow 依赖。

## 动效规范

- 一次编排好的入场序列：`.reveal` 错峰 80 / 160 / 240 / 320ms，只动 opacity 与 translateY。
- 悬停：按钮上浮 2px、下载卡上浮 3px、亮点行左移并铺底色、导航链接下划线扫过。
- 降级：`prefers-reduced-motion: reduce` 下关闭全部过渡与平滑滚动，内容直接可见。

## 自检

- 反 AI 味：无 Inter / Roboto / Arial / system-ui 展示字；无白底紫渐变；首屏非三卡片；亮点用规则行而非等高卡片；无通用装饰图标；氛围层含径向光、工程网格与噪点。
- 可访问性：正文 `#eef2ee` / 次要 `#9eaea9` 压在 `#0d1113` 上，对比度均 > 4.5:1；强调按钮用深色文字 `#1a0b05` 压橙底；`:focus-visible` 2px 描边；按钮与分段控件命中区 ≥ 40–44px；SVG 有 title/desc；复制状态用 `aria-live`。
- 响应式：1024px 收窄栅格并让对照台单列；768px 首屏单列、指标 2×2、隐藏文字导航；420px 再压缩字号与内边距。
- 状态：按钮含默认 / 悬停 / 按下 / 聚焦；分段控件有 `aria-pressed`；复制含成功与失败反馈；图片有 alt 与懒加载。
- 资源：全部相对路径，无外部 JS 依赖；字体走 CDN 且 `font-display: swap`，离线可回退。
