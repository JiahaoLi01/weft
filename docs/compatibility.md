# Web 标准与前端生态兼容参考

Weft 面向实时游戏界面精选并实现了现代 Web 核心标准子集，所有特性均以 **Chrome 行为作为语义基准** 并通过自动化契约测试校验。对于超出支持范围的语法或属性，Weft 提供明确的诊断反馈，而非静默渲染错位。

## 1. 严格模式与宽容模式

Weft 支持两种页面样式解析策略（通过 `weft_page_desc::flags` 配置）：

- **严格模式（默认）**：要求样式表全部落在引擎支持子集内，遇到不支持的属性或规则时明确报错，适合对渲染确定性要求极高的手写界面。
- **宽容模式（`WEFT_PAGE_FORGIVING_CSS`）**：采用与浏览器一致的渐进降级行为，遇到超出支持范围的声明、选择器或 `@` 规则时丢弃该条并输出场景诊断，其余样式照常生效；支持 Tailwind CSS、CSS-in-JS 等现代前端构建工具生成的样式表。

## 2. HTML、DOM 与 SVG

- **文档与元素模型**：每个 View 拥有独立 `Document`，支持常规 HTML 容器与语义节点（`div`、`span`、`p`、`button`、`img`、`input`、`textarea`、`bdi`、`bdo`、`style`、`script` 等）、文本与注释节点，以及 `createElementNS` 命名空间创建。
- **原生 SVG 渲染**：支持 `<svg>`、`<g>`、`<rect>`、`<circle>`、`<ellipse>`、`<line>`、`<polyline>`、`<polygon>`、`<path>`、`<text>` 等矢量图元，支持描边线宽、圆头/圆角连接、`stroke-dasharray` / `stroke-dashoffset` 路径动画与坐标变换（可直接承载原版 D3 图表与动态矢量 Logo）。
- **DOM 查询与事件模型**：统一选择器引擎驱动 `querySelector` / `querySelectorAll` / `matches` / `closest`；完整支持事件捕获/冒泡、`preventDefault`、`stopPropagation`、指针/鼠标/滚轮/触控/键盘/焦点/输入法组合与自定义事件。
- **Web 平台 API（`WEFT_PAGE_WEB_PLATFORM`）**：提供 `console`、`localStorage`、`sessionStorage`、`element.dataset`、`getComputedStyle`、`getBoundingClientRect`、`matchMedia`、`IntersectionObserver`、`requestAnimationFrame`、`requestIdleCallback`、`performance.now()`、`navigator.getGamepads()`、`navigator.maxTouchPoints` 等常用接口。

## 3. CSS 样式与布局

### 选择器与层叠

- 类型、类、ID、通配符、属性选择器（存在、`=`、`~=`、`|=`、`^=`、`$=`、`*=`）及后代/子代/兄弟组合器。
- Selectors 4 伪类与伪元素：`:hover`、`:active`、`:focus`、`:focus-visible`、`:root`、`:is()`、`:where()`、`:not()`、结构伪类，以及 `::before` / `::after`。
- `@layer` 级联层、`@media`（含 `pointer` / `hover` / `any-pointer` / `any-hover` 与视口查询）、`@supports`、`@property`、`@keyframes`、`@font-face`、CSS 嵌套、自定义属性（`--var` / `var()`）、`calc()` 与 `env(safe-area-inset-*)`。

### 布局模型

- **Flexbox（Yoga）**：完整弹性布局，涵盖主/交叉轴对齐、伸缩、换行、`gap`、收缩最小尺寸与替换元素比例换算。
- **块级、行内流式与定位**：标准块级/行内格式化上下文、多行折行、匿名块盒、相对与绝对定位（支持四向 `inset` 与 `auto` 居中）、逻辑属性（`margin-inline` / `padding-block` 等，随 `direction` 自动翻转）与 `box-sizing`。
- **Grid 布局与滚动容器**：支持网格容器布局及溢出滚动（`overflow: auto / scroll / hidden`、`scrollTop` / `scrollLeft`、滚轮与触控惯性滚动、`touch-action`）。

## 4. 视觉表现与动画

### 盒模型与绘制

- 颜色空间与语法：十六进制、`rgb()` / `rgba()`、`hsl()` / `hsla()` 及现代 CSS Color 语法；线性与径向渐变。
- 边框与圆角：独立四边宽度/颜色、椭圆圆角与嵌套圆角裁剪、盒阴影（`box-shadow`）。
- 2D 变换与合成：`transform`（平移、旋转、缩放、斜切、矩阵与 `transform-origin`）、`opacity` 离屏不透明度组、`z-index` 层叠上下文。

### 高级视觉特效

- **`backdrop-filter`**：实时采样元素背后游戏画面或下层 UI 实现毛玻璃模糊等背景滤镜。
- **`clip-path`**：矢量几何路径裁剪（与命中测试同步生效）。
- **`mask-image` / `mask`**：支持多层渐变与图像遮罩（含 `-webkit-mask*` 别名与遮罩合成模式）。
- **`mix-blend-mode`**：标准混合模式合成。
- **自定义着色器与 UI 粒子**：`background-image: -weft-effect(name)` 接入离线编译的 HLSL 像素效果（含背景折射/热浪），以及 `weft.emitParticles(...)` 内置 UI 粒子发射接口，详见 [自定义 UI 着色器效果](effects.md)。

### 动画系统

- **CSS Animations & Transitions**：`@keyframes`、多属性 `transition`、`linear`、`ease*`、`cubic-bezier()` 及完整 `steps()` 缓动函数。
- **Web Animations API**：完整支持 `element.animate()`、`KeyframeEffect`、`Animation`、`document.timeline`、`getAnimations()`、播放/暂停/反向/倍速控制、`ready` / `finished` Promise 与自动替换清理；动画插值直接在原生场景层执行，播放期间无需每帧回跳 JS。

## 5. 图像与媒体资产

- **解码格式**：原生集成 PNG（`lodepng`）、JPEG（`libjpeg-turbo`）与 WebP（`libwebp`），支持通过宿主资源回调加载本地/打包文件以及内联 `data:` URL。
- **`<img>` 与背景图**：完整支持 `HTMLImageElement`、`new Image()`、`decode()`、`onload` / `onerror`、固有宽高与 `aspect-ratio` 排版、`object-fit`（`fill` / `contain` / `cover` / `none` / `scale-down`）、`object-position`、`image-rendering` 与多级 Mip 缩放质量控制。
- **引擎实时纹理**：通过 `background-image: -weft-texture(name)` 零拷贝嵌入引擎 GPU 渲染目标。

## 6. 国际化排版与双向文字（CJK & BiDi）

- **复杂脚本塑形**：基于 **HarfBuzz + FreeType** 实现高质量字形塑形、连写与合体字渲染（中文、日文、韩文、阿拉伯文、希伯来文、天城文、泰文等），支持跨行内元素边界的连续塑形。
- **双向混排（BiDi / RTL）**：集成 **SheenBidi**（Unicode 17），完整支持 `direction: ltr | rtl`、`unicode-bidi`（`normal` / `embed` / `isolate` / `bidi-override` / `isolate-override` / `plaintext`）、`dir="auto"`、RTL 盒子布局、RTL 滚动方向、RTL `text-overflow: ellipsis` 及 RTL 文本框编辑。
- **字体发现与回退**：自动接入平台系统字体（Windows DirectWrite、macOS / iOS CoreText、Android NDK），并支持宿主注册自定义字体文件、字体集合索引（`.ttc`）与 `@font-face` `unicode-range` 分段回退。

## 7. 前端库生态支持

内置的 QuickJS-ng 与 DOM/CSS 兼容层已验证可直接运行未修改的主流前端工具与库：

- **UI 框架**：React（函数组件、Hooks、状态驱动渲染、Vite 构建产物）
- **状态管理**：Zustand 及自定义 `useSyncExternalStore` 引擎视图模型订阅
- **数据可视化与公式**：D3（完整 SVG 坐标轴、折线/柱状/桑基图、过渡动画与交互）、KaTeX 数学公式排版

## 8. 刻意排除的非游戏场景特性

为保持极致的包体、内存与确定性帧预算，Weft 刻意不包含以下浏览器特性：

- 页面导航、远程网络抓取（`fetch` / WebSocket 由宿主引擎桥接负责）、`<iframe>`、多标签页进程模型
- 内置音视频解码器（`<audio>` / `<video>` 交由游戏音频/视频系统处理）
- 浏览器默认控件皮肤与容错式畸形 HTML 纠错树构建
