# Weft HTML CSS DOM 兼容范围

本表规定目标范围，不代表已经实现。当前状态见 [证据记录](status.md)。P0 对应计划的 M0 架构探针，M1 是桌面原型；v0.1 只有 M0 到 M5 的必需项全部退出后才是可用版本，包括 M3 移动真机验收。后续能力只有完成实现、反例测试和平台验证后才能改为支持。

支持标准属性时沿用其标准语义。主动限制一个功能，必须同时限制相关值与组合，给出源码诊断；不能只统计属性名称数量。

## 能力与阶段对应

| 范围 | 必需阶段 |
| --- | --- |
| 依赖、GC roots、布局语义、文字、颜色和资源退休探针 | M0 |
| 基础 HTML/native DOM、层叠、盒模型、Flex、简单 block/inline、基础图片和绘制、D3 SVG 图表基础 | M1 |
| JS DOM 包装、query/样式/几何 API、事件、焦点、资源动态更新与增量 | M2 |
| touch/IME、输入控件、composition/beforeinput、移动生命周期与平台后端 | M3 |
| 虚拟列表、聊天、背包、HUD、动画、固定 Preact 与 D3 真实应用 | M4 |
| 检查、trace、兼容诊断、打包与干净构建 | M5 |
| var、media queries、更宽 CSSOM 与其他未被首批场景依赖的能力 | 可选扩展，不作为 v0.1 退出必需项 |
| 固定 CRPG gallery 的 72 例中的 70 例，含 Canvas2D、第三方动画与所需 CSS/SVG/交互；两例 Three.js 于 2026-10-12 移出需求 | 新增 workload 承诺；逐例执行/像素/交互/性能验收，尚未完整实现 |
| UI 着色器效果（`-weft-effect()`）、UI 粒子（`-weft-particles`）、背景拷贝与 `backdrop-filter: blur()` | 新增目标（2026-10-12），设计见 [架构](architecture.md)。已实现：效果绘制项，D3D11 与 Vulkan 的显示列表渲染器可以画（11.1）；scene 页面的 `background-image: -weft-effect(name)`（也可写在 `background` 简写里），参数取自元素的自定义属性，效果注册与诊断，用时间的效果让帧持续推进，D3D11 与 Vulkan 的 scene 渲染器可以画（11.2）。尚未实现：文档页面（它们不支持任何背景图片）、D2D 参考光栅、C ABI 的效果特性位、粒子、背景拷贝与 `backdrop-filter` |
| WebGL/WebGL2 | 不在目标内（用户 2026-10-12 决定，见 [决策](decisions.md)）：3D 由宿主引擎渲染、经外部纹理合成；`getContext('webgl')` 与 `getContext('webgl2')` 返回 `null` |

如果固定测试应用需要额外 API，先将它作为明确的新增承诺记录，而不是悄悄扩大 v0.1 范围。完整功能列表必须在实现时标注 planned/implemented/verified 与对应测试。

Windows 另有受限主鼠标 HWND/swapchain 样例，使用 actual layout hit、代际 target、MouseEvent、capture cancel 与 resize。自动测试从自己的 HWND 消息路径进入 React；实体鼠标、完整焦点/键盘/IME/PointerEvent 和移动未自动验收，见 [实际窗口范围](native-window-probe.md)。

GPU 后端已有可注册的实验 C ABI 与实际 D3D11 reference/window 接入；公共层不依赖图形 API。当前四边形帧与显示列表帧（盒子、图片、圆角裁剪链、隔离组；2026-10-11）、coverage/SDF、target/capability 和生命周期合同见 [backend API](render-backend-api.md)。Vulkan/Metal API ID 和模拟接口测试不构成这些平台的实际渲染支持。2026-10-12 起有实际的 Vulkan 渲染器：Windows 上 RTX 5090（Vulkan 1.4.341）与 AMD Radeon 核显（1.4.315）离屏绘制显示列表，与 D3D11 WARP 最多差 2 级，验证层零告警。同日起有 Vulkan 的 C ABI 适配器：宿主录制、提交并报告完成 serial，两帧在途，丢弃、失败回滚与设备放弃，目标格式 sRGB RGBA8/BGRA8 与线性 RGBA16F，验证层含同步验证零告警。缓存渲染器与 D3D11 共用纹理缓存和字形 atlas 的策略（同日）。Windows 上的 Vulkan 交换链窗口已验证（RTX 5090，4K CaseCheetah 流式页面，输入、缩放与像素对照，同日）。Android arm64（API 29，NDK r30）的配置、编译与链接已验证：全部可移植模块、Vulkan 后端与 Android 平台层，零警告；模拟器与真机运行、Android 输入与 Activity 生命周期均未验证。AMD 核显上的 Vulkan 交换链窗口与 iOS 尚未验证。2026-10-12 起有 Vulkan 的 scene pass（编码空间）：gallery 的 26 CSS + 8 D3 在 RTX 5090 与 AMD 核显上通过 102 张 Chromium 参考，与 D3D11 WARP 的原生 pass 逐像素比较；同日起可以记录进宿主的命令缓冲，多帧在途，按宿主报告的提交与完成退休（三帧在途与同步绘制逐字节相同）。Windows 上的 Vulkan scene 窗口已在 RTX 5090 上验证：交换链实时显示 gallery 的 34 个 scene，每个 scene 的最后一帧与离屏绘制逐字节相同；AMD 核显上的窗口未运行。同日起 Vulkan 的 C ABI 支持宿主纹理（外部纹理扩展 v1）：线性 RGBA8、RGBA16F，以及设备能线性过滤时的 RGBA32F；显示列表帧与四边形帧都能采样，支持行从底部开始的纹理，租约随帧完成退休（RTX 5090 与 AMD 核显，验证层含同步验证零告警）。Android 设备上未运行。

新增可选 external texture lease 扩展，通用生命周期与 D3D11 的 linear-premultiplied RGBA8/16F/32F、top/bottom origin、UV/opacity 和真实两帧退休已验证。首版同 device/queue、单 mip/采样、无 shared/CPU write；encoded sRGB/HDR/多队列/ANGLE/移动均显式未支持。这是 renderer 能力，尚没有 JS WebGL API 或新增 gallery 案例通过，见 [实际范围](external-texture-lease.md)。

2026-10-09 固定 gallery 的 **26 CSS + 8 D3** 新增 native 像素验收，连同已有两个 Canvas 共 36/72，详见 [scene 实际支持与诊断](scene-gallery.md)。SceneSolver 单独支持固定 utility sheet 的有界 block/Flex/absolute、圆角/边框、三类 gradient、2D transform、group opacity、关键帧/transition 与单个 perspective/rotateY 平面子树；8 个 D3 原例所需 SVG rect/circle/line/polygon/path/g/text 已在真实 JS→native DOM→绘制链路通过。`setInterval` 增加 function-only、有界容量/暂停合并的宿主调度，full HTML timer nesting/clamp 仍未实现。

这不提升完整 Tailwind/Grid、通用 SVG viewport/viewBox/clipPath、SVG geometry/hit、CSSOM/computed style、D3 drag/zoom/brush 或 gallery 的另外四例 D3 Interactive。SceneSolver 遇到未支持 CSS 属性/选择器/布局值、SVG 元素/path command、viewBox/clip-path/filter/mask、SVG descendant opacity 或 SVG text stroke 会诊断；general 3D 也在范围外（HTML 混合行内内容 11.3b 起支持）。原始 34 函数、React/D3 包、bundle 不变，固定 theme/rem/calc/logical utilities 的离线编译适配器是显式范围，运行不读取浏览器截图/几何。字体由 Windows OS DirectWrite 负责，移动/字体稳定打包与生产性能预算待验收。Windows 固定 scene 的 D2D GPU 绘制、颜色字体缓存与现有 external texture 接口已接通，沿用 102 个 Chrome reference 的阈值；没有扩大 HTML/CSS/SVG 语义范围，见 [性能证据](gallery-performance.md)。

2026-10-09 固定 gallery scene 接入 owner 直接 task/rAF 调用、弱身份 mutation journal/subtree revision、API 无关数值 paint 参数与有界静态绘制参数缓存。owner/renderer 线程边界、完整 packet、group opacity/clip 和真实 completion 退休不变；同步 geometry read 仍全量 fresh layout，未实现一般增量 DOM patch、后台 UI 线程或生产 retained display list。实际接口、参考路径与限制见 [本轮合同](owner-hotpath-performance.md)。

## 内容与 HTML

2026-10-11 重构第 8 步：positioned、auto 与 page profile 已退役。原样 NineSlice、AlertBar（四种 CSS）、React 页面与 Canvas gallery 都由统一的文档格式化器运行，各自的浏览器对照按原阈值通过。下文按 profile 记录的范围是各自当时的证据；统一路径支持这些子集的并集，另加 flex item 的 margin、静态根元素（视口初始包含块）与 canvas 替换元素。p、button 的 UA 默认 margin/padding/border 尚未建模，作者需显式设置；ul 未显式设置会诊断。

CaseCheetah 原样 NineSlice 已验证有界 author rules、类/id/type 及后代/子代选择器、层叠、明确尺寸的绝对定位、px/%/受限 calc、矩形图片裁剪及孤立层叠。它是独立 positioned profile，尚未与 Flex/inline 或一般文档/CSSOM 合并；JS 原生 DOM 的 querySelector/All 与 matches 和样式层叠共用一个选择器引擎，支持范围见下方“样式和布局”的第 9.2 步说明（D3 axis `.tick line` / `.tick text` 已验证）。完整原游戏、滚动和通用 positioned hit-test 未验收，见 [实际样本合同](casecheetah-workload.md)。

已验证固定 Roboto/Noto CJK 的有界 LTR 中英回退和灰度页面，原样 AlertBar/Zustand/store/双向 UE 桥在作者预览 CSS 下运行。span/b/ul/li 仅限显式 block/positioned、normal/400、零边距和 list-style:none；已独立接原始主题块与展开规则；尚不支持整个 operations.css、一般 inline、段落换行/bidi 或 IME。浏览器状态/几何/四项字体度量与 GPU 证据见 [中文 workload](cjk-alert-probe.md)。

| 能力 | 目标与边界 |
| --- | --- |
| 文档 | 一个 View 一个 Document；显式根容器和 viewport，首版无 iframe |
| 基础节点 | html、head、body、div、span、p、button、img、input、textarea、style、link、script；文本与 comment；svg/g/rect/circle/line/path/text 等基础 SVG |
| Namespace | createElementNS 与 namespaceURI/localName、NS attribute；HTML 与 SVG 的大小写和创建规则分别实现 |
| 解析 | 首版接受定义清楚、结构正确的 HTML 子集，包括 void tag、常用实体、属性与注释；拒绝超出范围的错误恢复场景 |
| 默认 display | 保留元素的 UA 语义；P0 页面显式写 flex，尚未实现的格式上下文报错 |
| HTML 错误恢复 | 不承诺完整 WHATWG tree construction、隐式闭合、table foster parenting 等 |
| 脚本加载 | 首版打包 bundle，在文档构建安全点执行；async/defer/module 的支持分别验收 |
| 资源 | 本地打包 PNG、字体和 CSS/JS；具体编码器随依赖探针锁定 |
| 图片 | intrinsic width/height、显式尺寸与替换元素；加载完成影响布局必须回归 |
| 表单 | 文本输入、textarea、button 与程序性 value；checkbox 等后续逐项加入 |
| 不在 v0.1 | 导航、远程页面、iframe、audio/video、完整 Canvas、SVG filter/foreignObject 等高级 SVG、浏览器默认表单外观 |

构建工具和运行时 DOM fragment parser 必须使用同一套可接受语法规则。动态 innerHTML 不能因为绕过构建工具而得到另一套解释。解析失败保留旧子树，禁止部分替换。


公共样式已抽到独立 value evaluator，支持有界 author/inline important、:root、属性 selector（存在、`=`、`~=`、`|=`、`^=`、`$=`、`*=`）、继承、token var、选中分支循环、CSS-wide 和 rgba。positioned/Flex 共用求值但仍是独立 formatting profile；没有 JS getComputedStyle；DOM querySelector 与它共用选择器引擎。16-case 浏览器语义、原始 theme/expanded rule 与八帧 DX11 的实际范围见 [公共样式合同](style-cascade-probe.md)。


新自动页面 profile 已在固定 AlertBar 和 48 个 mutation 场景验证受限 block/Flex/absolute、内容高度和中英固有宽度；零 margin/border、px padding、leaf text，以及受限百分比尺寸是硬边界。仅 ASCII 空格换行，未实现一般 inline/Unicode 换行；旧 positioned API 范围不扩大。见 [自动布局实际合同](automatic-layout-probe.md)。

2026-10-08 新 flow profile 单独扩大至 nested inline/span/br、匿名 block 行盒、normal/nowrap/pre-line 的有界 Unicode 换行、同步 geometry/client rects、scrollTop/Left/extent、可取消 wheel 默认滚动与椭圆圆角嵌套裁剪。27 个 mutation 场景和原样 React 八帧已进行 browser/GPU 验证，旧 auto/positioned profile 不扩张。跨源 grapheme/ligature、inline-block/replaced、inline 内 block/非零 decorations、bidi/字典分词、其他字重、soft hyphen/可见控制字形、完整 DOMRectList/scroll task 时序、实体 wheel/触摸/scrollbar 及移动仍有明确限制，见 [flow 支持值、诊断与证据](flow-layout-probe.md)。上文旧组件的 inline/滚动未验收描述只适用于各自历史 profile。

后续 flow 输入阶段已实现逻辑树/paint-order 分片区域、圆角/滚动裁剪命中、完整像素 WheelEvent metadata 和 HWND mouse/wheel owner 队列。原样固定 React 长列表在隐藏窗口完成实际滚动/选中/resize，3,150 点独立 Chromium 命中中 3,130 精确、20 点在显式 2 CSS px 每轴边界容差内。该范围不扩张旧 auto/positioned API；实体设备、触摸/惯性、完整 focus/keyboard/IME/PointerEvent 和移动仍未验收。见 [实际输入合同](flow-input-window-probe.md)。

## 样式和布局

2026-10-11 重构第 9.2 步：DOM 查询与样式层叠共用一个选择器引擎（`src/dom/selector`）。
- JS 的 querySelector/querySelectorAll/matches 新增：子代组合子 `>`；属性选择器（存在、`=`、`~=`、`|=`、`^=`、`$=`、`*=`，值可以加引号）；ASCII 范围的 CSS 转义；`:root`、`:hover`、`:active`。逗号列表按引号外的逗号切分。
- 样式随之接受上述全部属性运算符；非 scene 的样式仍不接受转义和 `:hover`/`:active`。
- 大小写按 HTML 规范：与 HTML 元素比较时，选择器里的类型名和属性名先转成小写再精确比较；SVG 等其他命名空间按原样比较。样式里的 `linearGradient`、`[viewBox]` 这类驼峰名此前被转成小写，永远匹配不上，现在能匹配。class 按 HTML 的 ASCII 空白切分（样式此前还把垂直制表符当分隔符）。
- 标识符按 CSS 规则：未转义的数字或“连字符加数字”不能开头。`[x=1]`、`.1a` 与浏览器一样被拒绝（DOM 查询报 `WEFT_SELECTOR_SYNTAX`，样式报 `WEFT_STYLE_PROFILE`），属性值要写成 `[x="1"]`；两套旧解析器此前都接受。
- 仍不支持：兄弟组合子 `+`/`~`、其他伪类（含 `:focus-visible`、`:not()`、`:nth-child()`）、伪元素、属性大小写标志 `i`/`s`、非 ASCII 转义、超过 16 段的链，以及 DOM 查询中超过 32 项的列表。DOM 查询遇到这些合法但未支持的选择器报 `WEFT_UNSUPPORTED_SELECTOR`，遇到不是选择器的文本报 `WEFT_SELECTOR_SYNTAX`；样式中两类都报 `WEFT_STYLE_PROFILE`（不变）。

2026-10-11 重构第 8.3a 步：scene 的 transform 只有一个文法。SVG transform 属性里无单位的角度按规范当作度（此前当作弧度，gallery 的 D3 径向标签因此画错）；SVG 列表可以用逗号分隔函数，rotate 可带中心；perspective(px) rotateY() 平面也接受函数间不留空格等 CSS 合法写法。CSS transform 中无单位的角度仍按弧度接受（CSS 规定只有 0 合法）。

2026-10-11 重构第 8.2 步：文档与 scene 共用一个 Yoga 适配器。scene 布局因此改用文档已有的 CSS 语义：flex item 默认收缩并有自动最小尺寸；block 的子项不 flex，align-self 不起作用；display:none 盒子的几何为 0；grid 的 display:none 子项不占格，行、列间距分别取 row-gap、column-gap。scene 也接受适配器支持的 space-between、baseline、content-box、inline-flex 等值。scene 的 102 个 Chrome 几何采集与 cluster 的 210 个采集不变。仍是近似的部分：scene 文字不换行；子项全是行内级的块按单行、顶对齐排列；自动最小尺寸不考虑百分比的指定尺寸。（前两项 11.3b 起已改，见下段。）

2026-10-13 重构第 11.3b 步：scene 有了内联格式化上下文，按 Blink LayoutNG 的规则排行。
- 范围：块容器（非 flex/grid）里的文字、行内元素（可嵌套、带内边距/边框/外边距）、`<br>`，以及原子内联（`inline-block`、`inline-flex`、`inline-grid` 与 svg，按收缩适配宽度排，按其最后一行的基线对齐，没有行时按下外边距边）。
- 空白：`white-space: normal`、`nowrap`；空格、制表、换行跨元素折叠，行首行尾空格去掉，`<br>` 之后的行首空格也去掉；软换行机会用 libunibreak（UAX 14）在整个上下文上算，两字符之间能否换行看它们最近公共祖先的 `white-space`；行内元素的起始边随后面的内容、结束边随前面的内容。
- 行盒：根的 strut、行上每个行内框的 strut、每段文字（`line-height: normal` 时含回退字体）与原子内联的外边距盒，共用一条基线；`text-align` 支持 left、right、center、start、end，溢出时按起始对齐；各段宽度按 LayoutUnit 向上取整。
- 行内元素按行分片：背景与边框逐片画，左边与左侧圆角只在首片、右边只在末片（`box-decoration-break: slice`）；命中与 `getClientRects` 按片，非原子行内元素的 `clientWidth/Height` 为 0，transform 不作用于它。
- `text-align`、`white-space` 改为继承；scene 的 UA 表补上常见块元素（`p`、`h1`–`h6`、`ul`、`li`、`section` 等）为 block、`button` 为 inline-block 并居中；仍没有 UA 外边距与标题字号（页面自带，Tailwind preflight 就是这样）。
- 画廊的原生样式表补上 Tailwind preflight 的替换元素规则（`svg` 等为 block、`vertical-align: middle`），与浏览器参照一致。
- 验证：23 个用例（段落、富文本、嵌套框、空白、`<br>`、长词、连字符、居中/右对齐与继承、匿名 flex 项、字距、inline-block、行内 svg、中文、混合字号、行内框 strut、nowrap、回退字体、emoji）与 Chrome 的每个元素片段、每行文字与位置逐一相同（最大差 0 px）。
- 列方向 flex 项里会换行的文字，宽度要由布局决定时，只在 flex-basis 与 height 都是 auto 时支持（自动最小高度等于 basis）；其他组合仍报 `WEFT_UNSUPPORTED_INTRINSIC`。
- 诊断：其他 `white-space` 值、`text-align: justify`、行内级盒子的非 baseline `vertical-align`、行内元素里的块级盒子与定位元素都报 `WEFT_SCENE_PROFILE`；float 与 bidi 不支持。

2026-10-13 重构第 11.3c 步之一：scene 的点线与虚线边框。
- 画法按 Chrome（`BoxBorderPainter` 与 `StyledStrokeData`）：
  - 边框盒先对齐整数设备像素，边宽向下取整为设备像素；沿边的中线排列点或段，所以缩放 2 时 1px 点线是 2×2 设备像素的方点，与 Chrome 在设备像素比 2 时相同；
  - 虚线的段长为边宽的 3 倍（1–2 设备像素宽）或 2 倍（更宽），间隔取让两端都是整段的值；点线 1–3 设备像素宽时为方点，两端按 Blink 的规则补整点，更宽时为圆点；
  - 先画点线与虚线边，再画实线边；实线边在与它们相交的角上斜接。
- 写法：
  - `border` 与 `border-top/right/bottom/left` 简写：宽、样式、颜色顺序任意，缺样式时为 none；
  - `border-style` 一到四个值（Tailwind 的 `border-dotted`）；`none`、`hidden` 的边宽为 0。
- 与此前不同：scene 以前不把 `border-style` 展开到各边（被静默忽略），`border` 简写缺样式时当作 solid；现在都按 CSS。
- 原生 scene pass 中没有圆角、没有旋转的盒子按像素面积算覆盖率（与 Skia、D2D 的矩形一致）：整像素上的盒子再小也是实的，离格盒子每个像素按所占份额混合；这类盒子带边框时，外缘那圈抗锯齿像素此前取背景色，现取边框色。旋转的带边框盒子外缘仍取背景色。
- 验证：17 个用例（各宽度的点线与虚线、四边、竖边、小数位置、与实线相交、带背景、长圆点线、行内关键词）在缩放 1、2 下与 Chrome 截图逐像素对照：
  - 方点与虚线在参考光栅与原生 pass 都与 Chrome 完全相同；
  - 圆点的边缘像素按各自的抗锯齿有差异，墨量差 3% 以内，每个点的质心差不到 0.05 px。
- 限制与诊断：
  - 带圆角的点线、虚线边框，`double`、`groove`、`ridge`、`inset`、`outset` 报 `WEFT_SCENE_PROFILE`；
  - 原生 pass 的圆点外有一圈约 9% 覆盖的淡晕（距离场的抗锯齿）；
  - 实线边框宽度不按设备像素取整，离格盒子也不像 Chrome 那样对齐像素，所以离格时比 Chrome 虚一些。

2026-10-13 重构第 11.3c 步之二：scene 的文字样式与相关 CSS。
- `text-transform`（继承）：
  - `uppercase`、`lowercase` 按 Unicode 完整大小写映射：ß → SS、ﬁ → FI、İ → i̇，小写 Σ 按 Final_Sigma；
  - 不做语言定制；映射表来自固定版本 QuickJS-NG 的 libunicode；
  - `capitalize`、`full-width`、`full-size-kana` 报 `WEFT_SCENE_PROFILE`。
- `font-style`（继承）：`italic`、`oblique` 选用字体自带的斜体。字体没有斜体时，与 Chrome 一样合成倾斜：字形绕基线向右倾，水平偏移为基线以上高度的 1/4，字宽不变。原生 pass、参考光栅、SVG 文字轮廓都画出倾斜；斜体文字的图像四周多留 0.35em，倾出盒子的笔画不被裁掉。
- `font-variant-numeric`（继承）：`lining-nums`/`oldstyle-nums`、`proportional-nums`/`tabular-nums`、`diagonal-fractions`/`stacked-fractions`、`ordinal`、`slashed-zero` 作为 OpenType 特性交给 HarfBuzz；同组两值或未知词报诊断。
- 视口长度：
  - `vw`、`vh`、`vmin`、`vmax` 及其 `s`/`l`/`d` 前缀形式（scene 只有一个视口，三种相同），按帧的视口换算成 px；
  - 视口变化而 DOM 不变时，只重新展开用到它们的元素；
  - 关键帧里的视口长度报诊断（关键帧只解析一次）。
- `overflow-x`、`overflow-y`：
  - scene 只能两轴一起裁剪，所以一轴裁剪即整体裁剪；
  - 如 CSS，与裁剪轴并存的 `visible` 计为 `auto`，内容须能放下；
  - `clip`、`scroll` 仍报诊断；`overflow` 简写在层叠前展开到两轴，先后顺序按 CSS。
- `-webkit-user-select`：接受，无效果（与 `user-select` 相同）。
- 验证：
  - Chrome 的行几何新增 6 个用例：大写（含 ß、ﬁ、İ）、继承的大写、斜体、Impact 的合成倾斜、Constantia 的数字变体、Segoe UI 的等宽数字。与 Chrome 逐一相同，最大差 0 px；
  - 合成倾斜的斜率与 Chrome 截图相差不到 0.02：Chrome −0.266、参考光栅 −0.254、原生 −0.248 px/行。
- 限制：Final_Sigma 只看同一个文字节点；没有按 `lang` 的大小写规则；`shape_with_fallback` 的文字仍限于拉丁、汉字与通用字符，希腊、西里尔等字母的文字本来就报诊断。

2026-10-13 重构第 11.3d 步之一：JS 的 `Element.closest(selectors)`：元素自身或最近的匹配祖先元素，没有时为 null；与 querySelector/matches 共用选择器文法与诊断。

2026-10-13 重构第 11.3e 步之一：
- 字体回退按 emoji 呈现选择系统回退字体，与 Chrome 相同：
  - `font-family` 列表里的字体按顺序优先，不论呈现；
  - 列表之外的系统回退中，默认按文字呈现的 emoji（如 🗡 U+1F5E1、☠ U+2620）或后跟 VS15 的取单色字体（Windows 上是 Segoe UI Symbol），默认按 emoji 呈现的（如 📜）或后跟 VS16 的取彩色字体（Segoe UI Emoji）；该类没有字体覆盖时取任一覆盖它的回退字体；
  - Emoji 与 Emoji_Presentation 属性取自 QuickJS-NG 的 libunicode。
- scene 的 UA 样式中 `html`、`body` 为块，scene 可以以 html 为根运行整页。
- UI 着色器效果（`-weft-effect()`）像 Chrome 画图像与画布内容那样，画进对齐到设备像素的矩形（各边取最近的像素）；旋转、斜切或镜像的盒子不对齐。盒子的背景与边框仍按小数位置的覆盖率绘制（Chrome 也先对齐，见 11.2 的 css-19-63 记录），小数位置的 1px 边框比 Chrome 略虚。

2026-10-13 重构第 11.3d 步之二：scene 的 `background` 简写在层叠中展开为 `background-color` 与 `background-image` 两个分量，各自参与层叠；值是渐变或 `-weft-effect()` 时给图像，其余给颜色，`var()` 代换后才判断。此前简写在层叠之后才拆分，会盖过优先级更高的分量（如内联的 `background-color`）。关键帧里的简写仍在展开时拆分。

2026-10-13 重构第 11.3c 步之三：scene 的 `position: fixed`。
- 包含块：视口（scene 根），或最近的带 `transform`、`will-change: transform` 的祖先。`will-change: transform` 的盒子此后也是绝对定位后代的包含块，与 Chrome 相同。
- 布局：按绝对定位排进包含块（px 与百分比按包含块，宽高自适应时为收缩适配），再换回父元素的坐标。绘制与命中仍在它在树中的位置；z-index 仍只在兄弟之间排序（门户到 body 的浮层是 body 的子元素，不受影响）。
- 诊断（`WEFT_SCENE_PROFILE`）：
  - CSS 中它不受包含块与它之间那些盒子的 `overflow` 裁剪；scene 在树中位置绘制，会受这些裁剪。所以这些裁剪必须容纳包含块显示出的那部分（根上是视口，包含块裁剪时是它的内边距盒），否则报诊断（裁剪区域按内边距盒，带圆角时再内缩最大半径）。拖到视口外一半的窗口在与视口同大的裁剪盒里可以接受；
  - 某一轴两侧 inset 都是 auto（要用静态位置）而父元素不是包含块；
  - 在没有变换祖先的 inline-block 里。
- 验证：
  - Chrome 155 的 18 个元素 client rect 全部相同：视口与百分比、收缩适配的文字、变换与 `will-change` 的包含块（含静态的 `will-change` 盒子）、被容纳在裁剪内的固定盒子；
  - 命中与原生 pass 的像素都在固定盒子所在处；三种诊断，以及两种应接受的结构。

| 能力 | P0 探针 | v0.1 目标 | 后续 |
| --- | --- | --- | --- |
| 层叠 | UA/author/inline、important、继承与 source order | 同左，配套 CSSOM 更新 | cascade layers、复杂 at-rule |
| 选择器 | tag/class/id/universal、descendant/child | attribute 存在和等值、hover/active/focus/disabled、selector list | sibling、structural、is/where/has |
| 盒模型 | px、百分比、auto、min/max、padding/border、content/border box | em/rem/vw/vh | media 条件与更多相对单位和函数 |
| Flex | row/column、grow/shrink/basis、wrap、align/justify、gap 的淘汰性用例 | 通过的值与组合，保持 Web 默认语义 | 更宽范围必须新差分 |
| Block | 已验证受限零 margin/border normal flow 与内容高度 | 简单垂直 normal flow；普通流 block 的垂直 margin 必须为零，间距用容器 padding 等 | margin collapse、float、多列 |
| Inline | text/span 匿名行盒探针 | 空白折叠、换行、基线、纯文本与 span；不含嵌套块或 inline replaced elements | 更复杂 inline、富文本 |
| 定位 | static/relative/absolute 和 containing block | 同左；flex 内 margin 与 absolute 行为逐项验证 | fixed、sticky |
| 文本 | 固定打包中文/拉丁字体、font size、line height | font weight/style、alignment、white-space、溢出与省略的具体组合 | RTL/复杂脚本、竖排、彩色 Emoji |
| 滚动 | overflow 与 scroll extent 探针 | auto/hidden/scroll、触摸/wheel、scrollTop/Left，首版 overlay scrollbar | 浏览器式占位滚动条、scroll snap |
| 视觉 | color/background、image、基础 border、SVG DOM 探针 | border radius、overflow clip、2D affine transform、opacity group、z-index；D3 所需基础 SVG 几何、path/fill/stroke/text/viewport | 3D、复杂 shadow、mask、blend mode、高级 SVG |
| 动画 | 使用确定性时间验证 transform/opacity 与 D3 scheduler | 两者的 CSS transition/animation，以及真实 D3 transition 的 attr/style/transform 更新 | 更宽 SVG animated API、复杂合成动画 |
| 自定义属性 | 暂无 | 不作为必需项，应用用显式属性和数据绑定 | var 继承、fallback、循环处理与更宽 Typed OM |
| 不支持的布局 | grid、table、float、完整 margin collapse | 同左 | 逐项扩展 |

字体材质采用灰度 coverage 与可选 SDF，共用逻辑塑形/布局，不新增非标准 CSS 字体属性。目前只有独立 Windows 字体/GPU 探针完成单通道 SDF 的有限缩放和描边验收；CJK SDF、SDF 页面集成、移动与 MSDF 未验证，中文灰度的实际范围另见上述 workload。见 [SDF 实际边界](sdf-font-probe.md)，此说明不提升上表目标能力的实现状态。

普通 block flow 的 margin 限制只适用于该格式上下文；不能因此删除已支持的 flex item margin。构建诊断应定位到具体布局模式和值。v0.1 的 block 范围只是有约束的子集，不能称为完整 CSS Block 支持。

transform 从 none 改为有效变换可能改变 absolute containing block；opacity 会形成分组合成。它们不能被全局标为永远只影响 paint。[CSS Transforms](https://www.w3.org/TR/css-transforms-1/)、[Compositing](https://www.w3.org/TR/compositing-1/)

移动安全区域和软键盘 geometry 在宿主度量中提供。标准 env 或 media 语法只有实现并测试后才列为支持；初期可以通过显式宿主数据绑定供应用使用。

v0.1 设计默认在 linear-sRGB 中做 premultiplied-alpha 合成。CSS/PNG 输入编码、alpha 与输出转换均须一致，但半透明叠加可能不同于浏览器的编码空间混合。几何/DOM 兼容与浏览器逐像素颜色匹配分开声明，颜色探针必须有独立预期。[颜色决策](decisions.md)

## DOM 与事件

| 能力 | v0.1 目标 | 关键合同 |
| --- | --- | --- |
| 节点创建 | createElement、createTextNode、createDocumentFragment | 同一文档内稳定节点身份 |
| 树操作 | appendChild、insertBefore、removeChild、replaceChild | mutation 即时可见；移动节点保持身份 |
| 内容属性 | textContent、基础 attributes、classList、dataset、有限 innerHTML | detached 节点可继续使用；fragment 解析失败不部分写入 |
| 查询 | getElementById、querySelector/All 支持已声明 selector | 返回集合的 live/static 规则按各 API 定义 |
| 样式 | inline style 和支持属性、getComputedStyle 子集 | CSSStyleDeclaration 和 computed 值分别实现 |
| 几何 | getBoundingClientRect、client/scroll 基础几何 | flush 当前布局；结果为值快照 |
| 事件 | add/removeEventListener、dispatchEvent、capture/bubble、stopPropagation、preventDefault | 固定传播路径、listener removal 标记、默认行为规则 |
| 输入 | pointer、wheel、key、focus/blur、input、beforeinput、composition 子集 | 事件 flags、顺序和 cancelability 按事件类型测试 |
| 焦点 | focus/blur、tabIndex 与基本 focus navigation | 节点移除、View 暂停和 IME 会话的协调 |
| 包装与回收 | JS wrapper identity、listener tracing、原生引用 roots | 先通过跨语言循环与销毁探针 |
| 不在 v0.1 | Shadow DOM、custom elements、Mutation/ResizeObserver、完整 Range/Selection、HTML navigation | 不提供静默无效的 stub |

输入控件内部 selection/composition 模型会早于通用 DOM Selection API；前者不表示后者完整可用。监听器在回调期间删除节点、新增监听器和重入 dispatch 必须按 [DOM 事件规范](https://dom.spec.whatwg.org/#concept-event-dispatch) 测试。

## JavaScript 与前端框架

QuickJS-NG 只是候选 ECMAScript VM，不提供浏览器环境。console、timers、rAF、DOM、事件、资源桥接和必要 scheduler primitives 由本项目实现，全部由 owner 驱动。默认不暴露 shell、std、os、任意文件访问或网络能力。

v0.1 框架目标为固定版本 Preact 和 D3 的真实应用。D3 要覆盖原生 DOM selection/data join、SVG 图表可见绘制、坐标轴与文字、transition 及指定交互；只运行 scale 等纯数学模块或生成 SVG DOM 不能称 D3 图表支持。依赖与分级见 [第三方前端库](frontend-libraries.md)。

记录 bundle、使用到的 DOM/SVG API 与缺失项。通过固定应用不等于所有插件或 React/Vue/Angular 兼容。当前 React 19.3.0/Vite 8.3.3 的一个显式 Flex/inline CSS/单行文字页面已完成 Windows native DOM 与 GPU 三帧，含 useState 和 keyed reorder；采用 owner timer 的 scheduler fallback。该 profile 的 MessageChannel/rAF、通用 DOM、HTML parser、完整 CSS 文档加载/失效、完整输入和移动保持未验证，见 [实际 profile](react-vite-probe.md)。不能为了测试通过修改第三方库或在 JS fixture 中伪造 DOM。

2026-10-09 的 4K 窗口连续运动展示仅增加局部宿主时钟驱动的 JS `style.left` 更新，真实 native DOM/style/layout/GPU 与暂停、速度和生命周期通过自动场景；附带的 30 Hz 轨道是刻意采样的视觉参照。没有实现 CSS transition/animation、transform/opacity 合成、requestAnimationFrame/cancelAnimationFrame 或 D3 transition 的浏览器调度语义，以上 M4 目标与 React scheduler 边界不变。见 [演示合同与证据](flow-input-window-probe.md#连续运动展示2026-10-09)。

JS source/bundle 是基础交付格式。bytecode 缓存绑定 VM 版本、字节序、构建配置和 source hash，不能用跨版本缓存代替升级测试。JS engine 的 ECMAScript 支持与本项目 Web API 支持分别列账。

Gallery/rAF Foundation 历史记录：可选 rAF module 已通过 owner 批次/取消/微任务/错误/隐藏/GC/预算验证，原 React 19.2.4 与 React Spring 三例实际修改 native DOM。72 例短观察中 38 例未发现执行错误、34 例阻断；该次未运行 gallery 原生像素/交互。两例 Canvas2D 现已在可选原生 context、显式 positioned adapter 和 D3D11 中验证 64 帧/输入/缩放/退休，见 [Canvas 实际范围](gallery-canvas.md)；其余 70 例未增加像素验收，gallery 性能仍未验收。WebGL2 已于 2026-10-12 移出需求；“不在 v0.1”的完整 Canvas 仍不扩张为所有 Canvas API 的承诺。公共外部纹理首版已验证，见上方；通用宿主 viewport 与移动仍待验收。原始源码/包不分叉，详见 [gallery 范围与证据](gallery-foundation.md)。

## 诊断与范围变更

构建工具默认严格检查声明的 profile，静态可发现的未支持布局、值、组合与 API 给出错误及源码位置。第三方样式可显式采用 warning 策略，但构建报告必须列出受影响行为，不能默认静默成功。

运行时遇到动态非法 CSS，按 CSS 的无效声明规则处理，并在开发模式发出诊断；不能为了严格模式改变属性赋值的标准异常行为。有效但超出 profile 的值同样诊断且不采用近似渲染。未支持 DOM API 不导出，已有入口的非法使用遵循对应 API 错误合同。

新增能力的完成条件包括解析、computed value、布局/绘制/命中、动态失效、事件或动画语义、反例测试、兼容表与调试输出。只加 parser 分支不能算支持。

区分三种证据：规范 fixture 或浏览器差分验证语义；全量与增量差分验证更新范围；目标平台真机验证输入、渲染与生命周期。三者互不替代。

## 当前 Canvas 探针范围（2026-10-09）

2026-10-11 重构第 9.3 步：
- canvas 的 width/height 属性在脚本 DOM 与文档布局中由同一个函数解析（HTML 的非负整数规则）。`-0`、`-00` 现在与浏览器一样得到 0，此前回到默认的 300×150；其他负数仍用默认值。
- getContext 读取 options 时会运行脚本。若这段脚本已经为同一 canvas 创建了 context，现在返回那一个；此前会再建一个，并泄漏先建的那个。

可选 native Canvas2D、alpha/sRGB、width/height 同值重置、fillStyle/globalAlpha、fillRect/整数 clearRect、单个完整圆形的 beginPath/arc/fill，与 canvas onclick/onmousemove 已验证。支持 148 named colors、hex 与 legacy comma RGB/HSL；partial/multi path、stroke、transform、save/restore、渐变、图片/文字、JS ImageData、offscreen/worker 与 context loss 未实现。currentColor/现代空格颜色/其他色彩空间拒绝；负半径仅提供 IndexSizeError name，没有完整 DOMException 类。8 contexts、单轴 2048、32 MiB backing（含 resize 重叠）、65,536 pending commands 是明确 probe 限制，不是完整 Canvas 承诺或 RSS 上限。

只有 Windows D2D 软件 adapter 已执行验证。positioned profile 支持显式 block 的 canvas 固有尺寸/比例与同步几何值，CSS 尺寸不改 backing；通用 positioned hit-test 未开放。公共样式新增纯色 background 到 background-color 的展开，保留 important/var/层叠，图片/图层/位置组合拒绝。精确支持值、诊断、视觉误差和平台未验证项见 [Canvas 合同](gallery-canvas.md)。


此前仅Canvas的检查阶段（历史记录，当前scene进展见上方）：4K gallery 检查 shell：全部 72 个标签/分组/位置可浏览，两例原始 Canvas 同页运行；其余 70 例是诊断卡片，尚未新增像素支持。启用现有 Canvas 的短观察为 40 例执行未报错/32 例阻断，与早先无 Canvas 的 38/34 条件不同。修复 timer/rAF 精度不一致，不改变时钟单调性语义；完整 CSS/SVG/WebGL 与移动范围不变。见 [实际检查入口](gallery-preview.md)。

复杂 CSS 优化仅验证既有 planar `perspective(px) rotateY(deg)`、圆环/边框与动画子集的 Windows 路径，未扩充一般 CSS 3D 或自动图层合成支持；原样例/独立 full-reference 证据见 [复杂 CSS 阶段](complex-css-performance.md)。


CSS 中段 filled-border/scene-copy 优化不扩大支持范围。Windows 既有 scene 子集的软件全量参考与 184 个 GPU/透明重叠/越界/非均匀 scale 反例通过；相同 affinity 性能不能替代默认可见窗口或移动实机性能，完整条件见 [本轮记录](css-middle-performance.md)。


2026-10-09 3/4/5 优化不扩大 HTML/CSS/SVG/WebGL 功能表：现有 scene 子集增加显式源图、D2D damage、API-neutral owned native paint 与 Windows D3D11 shader adapter。unsupported 圆角/变换 clip、SVG/projective 等整 scene 回退既有 reference，`render-work.json` 报告原因。2026-10-12 起，轴对齐盒子（含镜像与缩放）的圆角溢出裁剪改由显示列表的圆角裁剪链在 D3D11 原生绘制，只有旋转或斜切盒子的溢出裁剪仍整帧回退。同日 10.5b 起，scene 的溢出裁剪按 CSS 取内边距盒与内圆角（外半径减相邻边框宽度，宽度不等时为椭圆角），原生绘制、D2D 参考与命中一致。子元素圆角与父内圆角重合处两层抗锯齿相乘，Chrome 在相同几何上有时相乘、有时只取子元素覆盖率，差异在参考容差内。同日起 scene 的外阴影（`x y [blur [spread]] color`，最多 8 层，模糊与扩展不超过 64px）也由 D3D11 原生绘制，阴影图与 D2D 参考共用一份可移植 CPU 实现；内阴影报 `WEFT_SCENE_RASTER` 诊断。10.5f 起 scene 中的 SVG（形状、描边、虚线、文字、组透明度、视口、单矩形 clip-path）由 D3D11 原生绘制，路径经可移植的 FreeType 矢量光栅，文字以字形轮廓绘制；支持范围与 D2D 参考相同，旋转下的 clip-path 与未知元素仍是可见回退。描边的斜接上限按 SVG 取 4，超过时为 bevel（D2D 参考同步修正）。10.5g 起旋转或斜切盒子的溢出裁剪、旋转下的 svg 视口与 clip-path 也原生绘制（裁剪在元素自身空间）。10.5h 起 perspective/rotateY 平面也原生绘制：内容先画进自身图像，再投影采样，scene 不再整帧回退。平面最多嵌套两层；第三层嵌套或角点在观察者身后的平面仍整帧回退，原因为 `plane nesting bound`、`plane behind the viewer`。平面内的溢出内容不画，D2D 参考端相同，Chrome 会画。10.5i 起显式图层也由原生 pass 绘制，原生路径中只有彩色字形文字仍经 D2D 参考光栅；图层里的文字与其他原生文字一样使用 FreeType 覆盖率。10.5j 起 COLRv1/COLRv0 彩色字形（含渐变与合成模式）由可移植的软件画布绘制，原生路径不再依赖 D2D。PNG/SVG 图像字形（CBDT、sbix、SVG 表）还画不了，这种文字仍走 D2D 参考光栅（Windows 默认 emoji 字体是 COLR，不受影响）；SVG 文字中的彩色字形仍整帧回退。文字复用 Windows glyph pixels，页面 SDF/一般 font atlas、Metal/Vulkan/mobile 未接入。内部 owner executor 与独立 async host 已有输入/epoch/GC/实际 retirement 验证；公开 C ABI、完整页面调度器和实际用户游戏接入仍待实现。详见 [3/4/5 实际边界](render-stages-performance.md)。


## 当前共享簇探针（2026-10-10）

此前“不支持 ResizeObserver”是早期通用 profile 范围；新可选 scene platform 已加入真实布局后 border/content size 观察器（64 observers、每个1024 targets），device pixel box、完整 resize loop 抑制和一般页面集成仍未实现。D3 Interactive/K-Line/Framer/Spring/Icons/Anime 共30原例有浏览器像素与输入对照；配合此前36例为66/72。HTMLCollection/computed-style/fragment/媒体/数字/输入、Grid 和非溢出 auto 的精确限制见 [共享簇](gallery-shared-cluster.md)。原始 Anime Throw 在两端静止单独列为原例问题；不能从66个画面通过推断该原例惯性交互或所有库 API 通过。WebGL/Lottie/移动未通过。旧历史通过计数与旧102图门槛保持。
