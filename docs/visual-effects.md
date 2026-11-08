# 视觉效果（`clip-path`、`mask-image`、`mix-blend-mode`）

2026-11-08 起（`weft-ingame` 分支），用户在缺口清单里选了这一项。分三个阶段：

1. `clip-path`（已完成）：形状裁剪，绘制与命中都裁剪。
2. `mask-image`（已完成）：渐变遮罩与 `url()` 图片遮罩。
3. `mix-blend-mode`（已完成）：与背后内容混合，层叠上下文隔离混合。

本文记录各阶段的范围、做法、与 Chrome 的对照证据、差别和尚未做的部分。

## 第一阶段：`clip-path`

### 范围

- **形状**：
  - `inset()`、`rect()`、`xywh()`，可带 `round` 圆角，包括用 `/` 分开的椭圆圆角；
  - `circle()`、`ellipse()`，半径可以是长度、百分比、`closest-side`、`farthest-side`，可用 `at` 指定圆心；
  - `polygon()`、`path()`，填充规则 `nonzero` 或 `evenodd`。
- **参考盒**：
  - `border-box`（默认）、`padding-box`、`content-box`、`margin-box`；
  - `fill-box` 按内容盒，`stroke-box`、`view-box` 按边框盒（HTML 元素的规则）；
  - 只写参考盒时，按这个盒子带圆角的形状裁剪。`margin-box` 的圆角像阴影扩展那样放大。
- **`-webkit-clip-path`**：`clip-path` 为 none 时生效。
- **裁剪的范围**：
  - 元素自己画的东西，包括背景、边框、阴影、轮廓和 `filter` 之后的图像，以及全部后代；
  - 命中测试同样裁剪；
  - 形状没有面积时（内缩超过盒子、半径为 0、多边形不足三个点），元素和后代既不画，也不接收命中。
- **过渡与动画**：同一种形状的数值逐个插值，沿用已有的按数值插值规则。

### 做法

- **解析与几何**：`src/scene/scene_clip_path.cpp`。
- **圆角矩形类的形状**：`inset()`、`rect()`、`xywh()`、`circle()`、`ellipse()` 和只写参考盒，都放进已有的圆角裁剪链。它们和 `overflow` 的圆角裁剪是同一种条目，所有后端的着色器原本就支持。
- **`polygon()`、`path()`**：
  - 裁剪链里新增“多边形”条目（`RoundedClip::Kind::Polygon`），顶点放在紧随其后的 `Points` 条目里，每条 9 个点。
  - 显示列表里的条目与 GPU 的裁剪数组一一对应。因此各后端只是原样上传，不需要改代码；增量补丁（`ScenePreparer::patch`）也照常工作。
- **着色器**（`scene.hlsl` 的 `clipPolygonCoverage`）：
  - 按环绕数判断内外，规则为 `nonzero` 或 `evenodd`。
  - 抗锯齿按到最近一条“分隔内外的边”的距离计算。自交多边形内部的交叉边不算：否则 `nonzero` 的五角星内部会出现暗线。
- **`path()` 的曲线**：按 0.2 个设备像素的误差折成线段（Wang 公式）。总点数上限 1024，超出时放宽误差。
- **命中**：`scene_geometry` 给元素加一层裁剪。圆角形状用 `Rounded`，多边形用 `Fill` 轮廓。
- **样式与重画**：
  - `clip-path` 在布局签名里算作只影响绘制的属性；
  - 带 `clip-path` 的元素有变化时整页重画，因为形状可能露出或藏起溢出部分；
  - `filter` 的内容图像和显式图层的图像里去掉 `clip-path`，由绘制它们的条目来裁剪，避免裁两次。

### 证据

2026-11-08，macOS 26.6，Apple M5，Debug，Chrome 156（无头）。

- **工具与夹具**：`tools/browser_clip_path_reference.mjs`，结果放在 `tests/fixtures/clip_path`。
  - 页面有 34 个用例。每个是一个带边框、内边距、背景的盒子，里面有一个溢出的子元素。
  - 用例覆盖上面的每种形状和参考盒，也覆盖圆角、阴影、旋转、透明度、嵌套、外层 `overflow` 裁剪和 `-webkit-` 前缀。
  - 录下页面截图，以及 1666 次按压各自到达的元素。
- **`clip_path_browser_contract`**（可移植）：
  - 1666 次按压到达的元素与 Chrome 全部相同。
  - 另加一段过渡检查：`inset(0)` 到 `inset(50px)`，线性 1 秒，中点是 `inset(25px)`。这一段按规范计算，不是录制的。
- **`clip_path_browser_reference_metal`**（只在 Apple 平台）：
  - 34 格全部通过：每格像素差超过 48 级的不超过 0.2%（实际最多 35 个，多数为 0 到 20 个），平均差都不到 0.4 级。
  - 改一个被多边形裁剪的子元素的颜色后，增量补丁画出的结果与全新渲染逐字节相同。
- **变异检查**：
  - 把 `evenodd` 当成 `nonzero`：2 格失败；
  - 抗锯齿不排除内部交叉边：自交五角星有 68 个像素超差（改后 12 个）。
- **全量 `ctest -j8`**（`build/macos-img2`）：83/91，失败的仍是原来那 8 项 fixture 检查。

### 与 Chrome 的差别

- **抗锯齿**：着色器按距离计算覆盖率，Chrome（Skia）按面积计算，边缘像素会差几十级。差得最多的是两种位置：形状边与盒子边重合处（两条抗锯齿边的覆盖率相乘），以及椭圆与盒子边相切处。
- weft 场景样式默认 `box-sizing: border-box`，这是 weft 原有的设计，与 Chrome 不同。测试页面显式写了 `content-box`。

### 尚未实现与未验证

- 以下情况报诊断，元素不裁剪地画：
  - `url()` 引用 SVG `<clipPath>`；
  - `margin-box` 配百分比外边距；
  - `content-box` 配百分比内边距；
  - px、百分比及两者的 `calc()` 以外的单位（如 em）。
- 跨行的行内元素不裁剪，也不报诊断。
- D2D 参考光栅不画 `clip-path`：遇到时整帧失败（明确拒绝，而不是不裁剪地画）。
- **未验证**：
  - Windows：
    - D3D11、D3D12、Vulkan 共用这里的着色器和裁剪上传，没有在本机运行；
    - 着色器改动只由 DXC 编译过（Metal 与 Vulkan 走这条路），D3D11 用的 fxc 没有编译过；
    - 只在 Windows 上构建的 `view_css_profile_contract`，期望已从 `inset(1px)` 改为仍不支持的 `url(#c)`，没有运行。
  - Android；iOS 只在 iPad 上看过示例页，见“iPad 真机”。

## 第二阶段：`mask-image`

2026-11-08（`weft-ingame` 分支）。先做了渐变遮罩，随后（用户确认）做了 `url()` 图片遮罩；图片遮罩要给组的合成多绑定一张纹理，改了四个后端。

### 范围

- **遮罩层**：
  - 渐变：`linear-gradient()`、`radial-gradient()`、`conic-gradient()`、`repeating-linear-gradient()`、`repeating-radial-gradient()`，写法与背景相同，每层最多 8 个色标；
  - 图片：`url()`，PNG、JPEG、WebP 与 `data:` URL，与背景图读法相同；每个元素的遮罩最多一层图片，可与渐变层混用；读不到的图片是透明层（与 Chrome 一样，元素因此看不见）；
  - `none` 是透明层。最多 8 层。
- **每层的属性**：
  - `mask-size`（`auto`、长度、百分比、`cover`、`contain`；图片的 `auto` 是自然尺寸，只给一边时另一边按比例，`cover`、`contain` 保持比例；渐变没有固有尺寸，`auto`、`cover`、`contain` 都是定位区的大小）；
  - `mask-position`；
  - `mask-repeat`（`repeat`、`no-repeat`、`repeat-x`、`repeat-y`，两个值分别给 x、y）；
  - `mask-origin`（默认 `border-box`）、`mask-clip`（默认 `border-box`，可为 `no-clip`）；
  - `mask-mode`（`alpha`、`luminance`、`match-source`；渐变与图片的 `match-source` 都是 `alpha`）；
  - `mask-composite`（`add`、`subtract`、`intersect`、`exclude`）。
  - 各列表按层数循环取值。
- **简写与前缀**：`mask`、`-webkit-mask` 简写（多层，各部分任意顺序，一个盒子同时给 origin 与 clip，第二个给 clip）；`-webkit-mask-*` 是同名属性的别名；`-webkit-mask-composite` 的旧写法 `source-over`、`source-out`、`source-in`、`xor` 换成 `add`、`subtract`、`intersect`、`exclude`。
- **作用对象**：元素自身和后代合成后的整组，在 `filter` 与 `clip-path` 之后、`opacity` 之前，与 Chrome 一样。半透明处不会透出被遮挡的内容（逐项相乘会透出）。
- **命中**：遮罩的值不影响命中；但元素和后代的命中被裁到边框盒（矩形，不带圆角），有 `no-clip` 层时不裁。这是对照 Chrome 的结果。

### 做法

- **解析**：`src/scene/scene_mask.cpp`。渐变复用背景的解析（从 `scene_gradient` 拆出 `scene_gradient_image`），按定位区和 `mask-size`、`mask-position` 算出图块。
- **样式**：`mask-*` 八个属性进入 scene 的属性表。`mask`、`-webkit-mask` 简写在样式模块里展开成各属性的列表（`mask_parts`），`-webkit-mask-*` 在展开时改名。
- **亮度**：每个色标的值是 Rec. 709 权重（0.2126、0.7152、0.0722）乘编码后的 sRGB 值、再乘 alpha。这是拟合 Chrome 截图得到的（Skia 的亮度滤镜），按线性光或 BT.601 权重都对不上。色标之间按预乘值线性插值，与 Chrome 插值颜色再取亮度一致。
- **图片层**：
  - 场景节点多了 `mask_image`：构建元素时按背景图的方式读 `mask-image` 里的 `url()`。
  - 遮罩条目记下这张图在显示列表资源里的序号；准备阶段把它做成页面图片（带 mip 的预乘编码 sRGB，见[图片元素第三阶段](img.md)），挂在那一项上（`ScenePrepared::mask_images`）。
  - 调度里这类绘制带上 `Extra::Mask`，后端把图片绑到效果纹理的第一个槽（t3，粒子精灵也用这个槽）；场景着色器在这个槽上声明 `maskImage`。Metal、D3D12、Vulkan 照抄精灵的绑定，D3D11 在组合成和图像项绘制前绑定；Vulkan 改为每个描述符集都写效果槽（没有时写空白纹理）。
  - 着色器按图块采样：取 alpha，或按亮度权重取亮度；采样规则与页面图片相同（一个图块覆盖绘制区时用 mip、偏 0.5 级，重复铺开时只用原图）；不重复的轴在边缘纹素中心截止，重复的轴跨图块环绕。
  - 不重复的轴上，图块按设备像素取整（左右边各自四舍五入），与 Chrome 画图片时对齐像素一致；元素旋转、斜切或镜像时不取整。
- **绘制**：
  - 有遮罩的元素总是成为一个隔离组；组合成那一项的裁剪链末尾加一个“遮罩”条目（`RoundedClip::Kind::Mask`），各层的参数放在随后的条目里，每层 36 个浮点数。
  - 着色器（`scene.hlsl` 的 `clipMaskCoverage`）从最底层往上逐层算值，按各层的运算合成：`add` 为 source-over，`subtract` 为 source-out，`intersect` 为 source-in，`exclude` 为 xor。合成后的值乘到组的覆盖率上。
  - 条目与 GPU 的裁剪数组一一对应，各后端原样上传，后端代码未改；增量补丁照常工作。
  - `filter` 之后的图像、显式图层是单个绘制项，遮罩直接加在它们自己的裁剪链上；它们的内容图像里去掉 `mask-image`。
- **命中**：`scene_geometry` 给有遮罩的元素加一层边框盒裁剪（有 `no-clip` 层时不加）。
- **重画**：带遮罩的元素有变化时整页重画（`no-clip` 的遮罩可能露出溢出部分）。D2D 参考光栅遇到 `mask-image` 整帧失败。

### 证据

2026-11-08，macOS 26.6，Apple M5，Debug，Chrome 156（无头）。

- **工具与夹具**：`tools/browser_mask_image_reference.mjs`，结果放在 `tests/fixtures/mask_image`。
  - 48 个用例。盒子与第一阶段相同：不透明的子元素压在不透明的盒子上，用来检验整组相乘。
  - 渐变用例 30 个，覆盖上面每种渐变、属性、模式、运算、简写与前缀，以及透明度、圆角、旋转、`clip-path`、嵌套遮罩。
  - 图片用例 18 个，覆盖自然尺寸平铺、`contain`、`cover`、单边尺寸、拉伸、缩小（单块与平铺；含 1 px 条纹的细节图）、亮度、不透明图的 alpha、`repeat-x`、图片与渐变混用、`exclude` 掉图片、旋转和 `data:` URL。
  - 工具会生成遮罩图片（圆环图标、宽椭圆、亮度图、棋盘格、条纹）。
  - 录下截图和 2352 次按压到达的元素。
- **`mask_image_browser_contract`**（可移植）：2352 次按压中 2350 次到达的元素与 Chrome 相同。另外 2 次列为已知差别：它们在旋转盒子的边外不到 1 像素处，Chrome 对不带遮罩的旋转盒子也会算作命中（这是变换后盒子命中的既有差别，与遮罩无关，见下）。
- **`mask_image_browser_reference_metal`**（只在 Apple 平台）：
  - 48 格全部通过。除圆角与带 `clip-path` 的两格外，每格超过 48 级的像素都是 0；图片各格的最大差多为 1 级（细条纹缩小一格最大 14 级）。
  - 改两层遮罩元素里子元素的颜色后，增量补丁画出的结果与全新渲染逐字节相同。
- **变异检查**：
  - 交换 `subtract` 与 `intersect` 的运算：这两格失败；
  - 图片遮罩一律不用 mip：条纹缩小一格有 1200 个像素超差。
- **对齐像素**：未做取整前，左上角落在 55.5 px 的 17 px 图片图块与 Chrome 差得多（Chrome 把它放在 56 px）；取整后最大差 2 级。
- **全量 `ctest -j8`**（`build/macos-img2`）：85/93，失败的仍是原来那 8 项 fixture 检查。`render_metal_shader_contract` 登记了 `maskImage` 的绑定位置。

### 与 Chrome 的差别

- 遮罩的绘制区边缘（`mask-clip` 的盒子）按裁剪的方式抗锯齿；盒子边落在整像素上时与 Chrome 相同，其余情况没有对照。
- 命中：在旋转盒子的边外不到 1 像素处，Chrome 仍算命中盒子，weft 按盒子的精确边判断。这与遮罩无关，是变换后盒子命中的既有差别。

### 尚未实现与未验证

- 以下情况报诊断，元素不加遮罩地画：
  - 一个元素的遮罩里有两层以上图片；
  - `mask-repeat` 的 `space`、`round`；
  - `margin-box` 作为 `mask-origin` 或 `mask-clip`；
  - px、百分比及两者的 `calc()` 以外的单位。
- 简写里用 `var()` 时整条声明报错（与 `flex` 简写相同）。
- `mask-border`（`-webkit-mask-box-image`）。
- 锚点元素里面的遮罩：锚点元素内的组合成不带裁剪链，遮罩不生效。
- 跨行的行内元素不加遮罩。
- SVG 图片（包括 `url(#mask)` 引用 SVG `<mask>`）不能作遮罩，与背景图一样不解码 SVG。
- **未验证**：
  - Windows：着色器改动没有用 fxc 编译过；D3D11、D3D12、Vulkan 的遮罩图片绑定（照抄精灵的写法）与 Vulkan 每个描述符集写空白效果槽的改动，都没有编译、没有运行；
  - Android；iOS 只在 iPad 上看过示例页，见“iPad 真机”。

## 第三阶段：`mix-blend-mode`

2026-11-08（`weft-ingame` 分支）。

### 范围

- **模式**：`normal` 之外的 16 种，`multiply`、`screen`、`overlay`、`darken`、`lighten`、`color-dodge`、`color-burn`、`hard-light`、`soft-light`、`difference`、`exclusion`、`hue`、`saturation`、`color`、`luminosity`，以及 `plus-lighter`。公式取自 Compositing and Blending 1，作用在编码后的 sRGB 值上，与 Chrome 一致。
- **混合的对象**：元素和后代合成后的整组，与它最近的祖先层叠上下文在它之前画下的内容混合。元素的透明度、遮罩、`clip-path` 先作用于组，再混合。
- **隔离**：层叠上下文是隔离组，里面的混合看不到外面。按 Chrome 的规则判断层叠上下文：
  - `position: fixed` 或 `sticky`；
  - 定位元素、flex 项或网格项的 `z-index` 不是 `auto`；
  - `opacity` 小于 1；
  - `transform`、`translate`、`rotate`、`scale`、`filter`、`backdrop-filter`、`clip-path`、`mask-image` 不为 none；
  - `mix-blend-mode` 不是 normal；
  - `isolation: isolate`；
  - `will-change` 写了上面这些属性之一。
- **页面的根**：帧的根元素不隔离，根这一层的混合与渲染目标里已有的内容混合。页面不透明时与 Chrome 相同；页面透明、叠在游戏画面上时，会与游戏画面混合（浏览器里页面本身是隔离的，没有这种情况）。

### 做法

- **混合元素**（`src/scene/scene_blend.cpp`、`scene_display_list.cpp`）：
  - 混合的元素总是成为一个组，组结束项（`EndGroup`）带上混合模式（`paint::Item::blend`）。
  - 组结束项的裁剪矩形收缩到子树的绘制范围，用来限定要复制的背景区域。
  - `filter` 之后的图像和显式图层是单个绘制项，外面包一层组再混合。
- **隔离组**：一个层叠上下文里直接含有混合的后代时（不穿过嵌套的层叠上下文），它也成为组。组从透明开始，混合只看到组里的内容。
- **规划与调度**（`scene_pass.cpp`、`scene_schedule.cpp`）：
  - 混合组的合成记录带上模式和背景区域（设备像素）。
  - 组结束时，先把要合成到的目标在该区域里复制一份（与读背景的 effect 共用复制机制），再画合成。
  - 目标不可读时，背景按透明处理，结果等于 normal。
- **着色器**（`scene.hlsl` 的 `blendComposite`）：
  - 从复制的背景取色，用混合函数算出“混合后的源色”：(1 - 背景 alpha) × 源色 + 背景 alpha × B(背景色, 源色)。
  - 输出预乘后，管线照常做 source-over，结果就是规范的混合结果。所以不需要新的混合管线。
  - `plus-lighter` 是相加封顶，同样换算成 source-over 能得到的输出。
- **后端**：复制好的背景绑在效果纹理的第二个槽（t4），场景着色器在那里声明 `blendBackdrop`。
  - Metal、D3D12、Vulkan：调度的绘制带 `backdrop`，后端从复制链取纹理绑定。D3D12、Vulkan 顺带把精灵的分支改为只在 `Extra::Sprite` 时进入。
  - D3D11：在组合成前用已有的复制函数复制，再绑定。
- **增量更新**：含混合元素的部分不做增量补丁（它所在层叠上下文的隔离组不在这一部分里），退回整页重建。
- D2D 参考光栅遇到 `mix-blend-mode` 整帧失败。

### 证据

2026-11-08，macOS 26.6，Apple M5，Debug，Chrome 156（无头）。

- **工具与夹具**：`tools/browser_mix_blend_mode_reference.mjs`，结果放在 `tests/fixtures/mix_blend_mode`。
  - 每格的背景是一条渐变，上面横着一深一浅两条带子。
  - 盒子与前两个阶段相同（带边框、内边距和溢出的子元素），用全部 17 种模式画在上面。
  - 另有组合用例：透明度、半透明背景、圆角、旋转、遮罩、`clip-path`、嵌套混合。
  - 隔离用例：`isolation`、`opacity`、`z-index`、`transform` 包裹层（隔离），以及普通的 `position: relative` 包裹层（不隔离）。
  - 共 29 格。
- **`mix_blend_mode_browser_reference_metal`**（只在 Apple 平台）：
  - 29 格全部通过，多数格每个像素差不超过 1 到 4 级。`saturation`、圆角、旋转和 `clip-path` 几格只在边缘抗锯齿处有差别（超过 48 级的最多 8 个像素）。
  - 改混合元素里子元素的颜色后：增量补丁被拒绝（原因“a part that blends”），整页重建的结果与全新渲染逐字节相同。
- **变异检查**：
  - 去掉隔离组：`isolation`、`z-index`、`transform` 包裹层三格失败（`opacity` 包裹层本来就是组）；
  - 交换 `multiply` 与 `screen` 的公式：四格失败。
- **全量 `ctest -j8`**（`build/macos-img2`）：86/94，失败的仍是原来那 8 项 fixture 检查。`render_metal_shader_contract` 登记了 `blendBackdrop` 的绑定位置。

### 与 Chrome 的差别

- 页面的根不隔离，见上面“页面的根”。
- 边缘抗锯齿的差别与前两个阶段相同。

### 尚未实现与未验证

- `background-blend-mode`（背景各层之间的混合）。
- 跨行的行内元素不混合。
- 锚点元素里的混合没有测试：锚点元素里的组合成用的是整个视图的区域，结果应当正确，但复制的区域大。
- 每个混合元素多一个组目标；Metal 在苹果 GPU 上直接读目标颜色，不再复制；D3D11、D3D12、Vulkan 仍要做一次区域复制（见“组的开销”）。含混合元素的部分改动时整页重建。
- **未验证**：
  - Windows：D3D11、D3D12、Vulkan 的背景复制与绑定（以及 D3D12、Vulkan 精灵分支的改写）没有编译、没有运行；着色器改动没有用 fxc 编译过；
  - Android；iOS 只在 iPad 上看过示例页，见“iPad 真机”。

## iPad 真机（2026-11-08）

用户把设备连上后要求在 iPad 上测已做的部分。设备是 iPad Pro 13 英寸（M4，iPad16,5），iPadOS 26.6.2。App 用 Release 构建，横屏 2752 × 2064 px，屏幕 120 Hz。

后来的 Metal System Trace 显示，这台设备的 GPU 一直被 Xcode 的设备状态（Device Conditions）固定在最低性能档（`Minimum`，`Is Induced: Yes`）。所以本节和下一节里 iPad 上的时间，都是在最低档下测的。

### 示例页

- `examples/effects` 是新加的示例页，iOS 与 macOS 的示例宿主都能打开（`--page effects`，或右上的 Next page）。
- 内容：
  - clip-path 七格：内缩圆角、圆、椭圆，非零与奇偶两种多边形，`path()`，一个点按就开合的过渡；
  - mask-image 七格：渐变、平铺、两层求交，图片按透明度与按亮度；
  - mix-blend-mode 七种，混在一张图上；
  - 图片缩到 1/4、1/8，以及放大后平滑与像素化各一格。
- 点中裁剪或遮罩过的形状时，页面上方会写出点中了哪个。

### 对照

- **做法**：`xcrun devicectl device capture screenshot` 截下 iPad 的屏，与 macOS 上同一页的离屏渲染逐像素比较。macOS 用 `weft_macos_demo --shot`，同样 1376 × 1032 逻辑 px、2 倍。比较时不算左上的统计行和右上的按钮。
- **结果**：
  - 所有效果的区域逐像素相同。
  - 只有 4 行 13 px 的文字不同：字形一样，基线比 macOS 高 1 到 2 个设备像素，是 iOS 系统字体与 macOS 的纵向度量不同。
  - 连拍的三张截图之间完全相同。
- **一次没有复现的差别**：
  - 重装后最初两张截图，彼此之间、与 macOS 之间，凡是有颜色的地方都差 1 到 9 级。
  - 差别是对颜色的整体映射：像素化图片里整块纹素一起变色，深色背景不变。
  - 重新启动后没有再出现。原因没有查清，推测在显示一侧，未确认。

### 性能问题与修正

- **现象**：第一次打开时只有 61 fps，GPU 每帧 24.5 ms。
- **定位**：在 macOS 上把页面各部分分开计时，遮罩那部分占了绝大多数。
  - 每个加遮罩的元素，组合成时都在整个目标上逐像素算遮罩：它的合成范围用的是继承来的裁剪，这里就是整页。
  - 这样每个元素每帧多约 1.6 ms（M5，这个尺寸）。混合的元素已经把合成范围缩到绘制范围，所以没有这个问题。
- **修正**（`scene_display_list.cpp`）：
  - 所有隔离组都只在子树的绘制范围内合成，周围留 `list_cull_guard_px`。组目标在这个范围以外是透明的，在那里合成本来就不改变底下的内容。
  - 这包括遮罩、半透明、`isolation`、带背景滤镜的组。
  - 绘制范围定不下来的子树仍按裁剪合成：滤镜、锚点元素、粒子、透视。
- **结果**：
  - iPad：120 fps，GPU 每帧 6.8 ms。作为参照，同尺寸的 `examples/img` 是 4.7 ms。
  - macOS 离屏（Debug，240 帧）：这一页每帧从 15.2 ms 降到 4.2 ms，输出逐像素不变。
- **测试**：`mask_image_browser_contract` 新加一段，检查 5 个组的合成范围。
  - 其中 4 个按绘制范围合成：遮罩、半透明、混合、旋转后的遮罩。
  - 另一个组里有带滤镜的子元素，按整页合成。
  - 把修正退回去时，这段报错：“group 0 composites over 0, 0, 800, 600”。

### 尚未做

- **每个组的固定开销**：已在 Metal 上去掉，见下面“组的开销”。
- **没有点按测试**：页面上的点按没有在真机上试过，点中的规则由 macOS 上的 clip-path 与遮罩点中测试覆盖。

## 组的开销（2026-11-08）

用户在 iPad 测试之后要求去掉组的开销。

### 问题

遮罩、半透明、`isolation`、混合等元素都成为组。原来每个组这样画：

- 结束父目标的渲染通道；
- 在一张整屏大小、按深度复用的组目标里画组的内容；
- 再回到父目标合成；
- 混合组合成前还要再结束一次父目标的通道，复制背景。

在苹果的分块渲染 GPU 上，每次结束和恢复都要把整个目标写回内存、再读回来。

在 Mac（M5，窗口 2560 × 1440，120 Hz 垂直同步）上分开量了临时的测试页：

| 页面 | GPU/帧 | 不加效果的同样内容 |
|---|---|---|
| 7 个遮罩 | 7.81 ms | 2.47 ms |
| 7 个混合 | 6.05 ms | 2.29 ms |

每个组多出 0.5 到 0.8 ms，组本身的范围很小，开销几乎都在通道切换上。

### 做法

- **先画组**（`SceneScheduleOptions::hoist`，`scene/scene_schedule.cpp`）：
  - 组里面不读任何目标时（没有背景滤镜、没有给效果用的复制、没有要复制背景的混合组），就在它合成进去的那个目标开始之前，把它先画进一张只有它合成范围大小的图片。
  - 合成范围是组结束记录的 `outline`：现在每个组都按合成范围（设备像素）写入这个字段，原来只有混合组写。
  - 图片的边长向上取整到 64 像素，但不超过 pass 本身的尺寸。
  - 外层的组先分配，所有先画的图片合计不超过一个 pass 的面积（`hoisted_group_budget`），超出的组照原来的方式画。
  - 先画的组里面如果还有组，里层的先画。
  - 读背景的组照原来的方式画，但它里面不读背景的子组仍可以先画。
- **目标的原点**（`scene.hlsl` 的 `View`）：
  - 常量块从 16 字节加长到 48 字节，加入当前目标在 pass 设备像素里的原点，以及组合成时采样的图片位置（`sampled`）。
  - 顶点着色器减去原点；像素着色器的像素位置加回原点。所以记录、裁剪、遮罩、背景采样都仍按 pass 的设备像素计算。
  - 组的合成记录带一个标记（`mode.z`），有 `sampled` 时，按它的位置逐像素取组图片。
- **混合组读目标颜色**（`SceneScheduleOptions::fetch_blends`）：
  - 苹果 GPU 上，混合组的合成用一个单独的像素着色器变体（`WEFT_BLEND_FETCH`），直接读目标里已有的颜色（framebuffer fetch，MSL 里是 `[[color(0)]]`），不再复制背景，也不打断父目标。
  - HLSL 里写成 `SubpassInput`，由 spirv-cross 的 `--msl-framebuffer-fetch` 翻译成 MSL 2.3。
  - 管线建不出来时（iOS 模拟器自称是苹果 GPU，但建不出这条管线），混合组仍复制背景。
- **只有 Metal 打开这两项**：
  - D3D12、Vulkan 不打开，调度结果逐条和原来相同。
  - D3D11 不经过这套调度。
  - 这三个后端只是把 View 常量写满 48 字节，新字段都是 0；D3D11 的常量偏移改用 16 字节为单位。

### 证据

2026-11-08，macOS 26.6，Apple M5，Debug。

- **调度测试** `scene_schedule_contract`：
  - 不打开新选项时，原有的逐条命令测试和 2000 个随机 pass 都不变。
  - 新加逐条命令的用例：简单的先画组、嵌套、含背景滤镜的组里先画子组、混合组复制背景与读目标两种、预算。
  - 新加 2000 个随机 pass 的先画组模式，检查器像一次只画一个目标的后端那样执行命令，并检查：
    - 每个批次只画一次、画进它所属的组的目标、同一目标里按批次顺序；
    - 组图片画完才合成；
    - 复制读的是读取者的目标，且不是先画的图片；
    - 图片的范围和预算正确；
    - 混合组要么复制、要么读目标。
  - 统计：1236 个组先画，3015 个组照原来的方式画，727 个组混合。
  - 变异检查：读背景只标给最内层的组、漏画里层的先画组、不扣预算，三种都被抓到。
- **Metal 的 Chrome 对照**：clip-path、mask-image、mix-blend-mode、图片等像素测试全部通过。
  - 合成时故意忽略组图片的原点：遮罩、clip-path、混合三个测试失败。
  - fetch 读到的颜色故意置零：混合测试失败。
  - 两次变异都说明测试确实走了新路径。
- **画面**：`examples/effects` 的离屏渲染与改动前逐像素相同。
- **全量 `ctest -j8`**：87/95，失败的仍是原来那 8 项 fixture 检查。
- **Mac 上的时间**（同上的测试页，120 Hz 垂直同步；GPU 低负载时频率会降，同一页面重复测量相差约 0.5 ms）：

| 页面 | 原来 | 只先画组 | 再加读目标 | 不加效果的同样内容 |
|---|---|---|---|---|
| 7 个遮罩 | 7.81 ms | 3.16 ms | 3.19 ms | 2.34 ms |
| 7 个混合 | 6.05 ms | 3.88 ms | 2.57 ms | 2.57 ms |
| effects 整页 | 7.19 ms | 6.61 ms | 5.82 ms | — |

- **iOS 模拟器**：先画组正常；混合组走复制背景的退路，effects 页显示正常。
- **iPad 真机**（GPU 被设备状态固定在最低档，见“iPad 真机”）：同一时段先后装旧调度和新调度的版本，effects 页各录 8 秒 Metal System Trace。

| 每帧 | 旧调度 | 新调度 |
|---|---|---|
| 父目标的渲染通道 | 15 | 1 |
| 组的渲染通道 | 14（整屏） | 14（组的范围） |
| 背景复制 | 7 | 0 |
| GPU 片元执行 | 6.10 ms | 5.99 ms |
| GPU 顶点执行 | 0.30 ms | 0.20 ms |
| GPU 复制（blit） | 1.00 ms | 0.02 ms |
| 合计 | 7.40 ms | 6.21 ms |

  - 新路径在真机上生效：通道切换和复制都没有了。
  - 省下的主要是混合组复制背景的那约 1 ms。iPad 上通道切换本身不贵，片元执行几乎没变。Mac 上量到的是命令缓冲从开始到结束的时长，含编码器之间的空隙，所以差别更大。
  - 示例统计的“GPU 时间”（同样是命令缓冲的起止时长）两版都约 6.7 ms：GPU 锁在最低档时看不出差别。
  - 剩下的大头是页面本身的着色：同一档位下 img 页也约 5 ms。这和组无关，没有做。

### 尚未做与未验证

- 每个先画的组仍是一次单独的小渲染通道。遮罩组每个还剩约 0.1 ms，含遮罩本身的计算。
- 读背景的组（里面有背景滤镜、读背景的效果）仍按原来的方式画。
- D3D11、D3D12、Vulkan 仍按原来的方式画组，混合组仍复制背景。
- **未验证**：
  - Windows：三个后端 View 常量的改动、D3D12 根常量数、D3D11 的偏移单位，都没有编译、没有运行；着色器改动没有用 fxc 编译过。
  - iPad 在正常 GPU 性能状态下的时间（今天只在被固定的最低档下测过）。
  - Intel Mac：没有苹果 GPU 家族，混合组应该走复制背景，没有运行。
