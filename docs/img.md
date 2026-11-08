# 图片元素（`<img>`）

2026-11-08 起（`weft-ingame` 分支），scene 页面支持 `<img>`。这一项分三个阶段：

1. `<img>` 元素（已完成）：排版、`object-fit` 绘制、脚本接口。
2. 格式与来源（已完成）：JPEG、WebP（Chrome 同款的 libjpeg-turbo、libwebp）、`data:` URL。
3. 缩放质量（已完成）：mip 与 `image-rendering`。

本文记录三个阶段的范围、做法、与 Chrome 的对照证据、差别和尚未做的部分。

## 第一阶段：`<img>` 元素

### 加载

- `src` 按页面的基准解析，经宿主的资源回调（`weft_view_set_resource_reader`）同步读取。
- 与 `background-image: url()` 共用 `ViewImages`：每次加载每个路径只读一次，格式（PNG）与上限相同。
- 读不到、不是 PNG 或超出上限都是 scene 诊断，图片按"没有图片"排。
- `srcset` 不读：发一条警告，画 `src`。
- macOS 与 iOS 的示例宿主（`samples/macos-host/engine.hpp`）补上了资源回调，此前它们读不到页面文件里的图片。

### 排版（`src/layout/flex.cpp` 的 `ImageSizing`、`replace_image`、`measure_image`）

- **自然尺寸**：图片的像素，一像素一个 CSS px。没有图片（没有 `src`，或读不到）时为 0 × 0，与 Chrome 对没有 `src` 的 `<img>` 一致。
- **属性**：`width`、`height` 属性按 HTML 的尺寸值规则映射为 `width`、`height` 属性（数字为 px，带 `%` 为百分比，`"20px"` 取 20），两个都是数字时再给 `aspect-ratio: auto w / h`。它们是表现属性，低于任何作者规则（`src/style/style.cpp`）。
- **尺寸**：按 CSS 2 §10.3.2、§10.6.2 和 §10.4 的表算。
  - 一个方向给定时，另一个方向按比例换算。
  - 两个方向都是 auto 时取自然尺寸，再按表在 min/max 之间保持比例。
  - `aspect-ratio`：单写比例时覆盖自然比例，按 `box-sizing` 指定的盒子算；`auto && <比例>` 只在没有自然比例时用，按内容盒算。
  - `box-sizing` 两种取值都支持。
- **各种上下文**：
  - 行内：原子行内盒，基线在下外边距边，`vertical-align` 照常。按它在包含块里的尺寸先算好，百分比按包含块宽度解析。
  - 块级：不拉伸；`auto` 外边距照常居中或推到一侧。
  - flex 项：行方向的基准尺寸，在高度确定时（自身 px 高度，或容器高度确定时的拉伸高度）按比例换算，否则取内容宽度。自动最小尺寸取去掉自身 `width` 后的宽度（自然宽度，或由确定高度换算），再与 px `width` 和百分比上限取小。列方向对应处理高度。
  - 网格项：`normal` 对齐按 start（替换元素不拉伸），格子宽度作为百分比的基准。
  - 绝对定位：左右（或上下）两侧都有 inset 时保持自身尺寸，不在两侧之间拉伸；`auto` 外边距在两侧之间居中（见下面的补丁 0007）。

### 绘制（`src/scene/scene_display_list.cpp`、`scene_background.cpp` 的 `scene_object_layout`）

- 图片画在背景和边框之上、内容盒里，按内容盒的内圆角裁剪。内容盒对齐设备像素，与背景图一样。
- `object-fit` 的五个值：`fill`、`contain`、`cover`、`none`、`scale-down`（取 `none` 与 `contain` 中较小的一个）。
- `object-position` 与 `background-position` 语法相同。
- 它是显示列表里一个"贴合图片"项，D3D11、D3D12、Vulkan、Metal 的 pass 都用背景图那一套着色器画。
- 带百分比内边距的 `<img>` 不画图片（诊断），因为内容盒在绘制时解析不了百分比。

### 脚本（`src/script/script_dom.cpp`、`src/script/js/scene_platform_bootstrap.js`）

- `HTMLImageElement`（`instanceof` 可用）与 `new Image(width, height)`。
- 属性：
  - `src`：取值是解析后的路径，没有 `src` 时为空串；赋值即设属性。
  - `currentSrc`、`alt`、`naturalWidth`、`naturalHeight`、`complete`。
  - `width`、`height`：渲染时取内容盒的宽高（四舍五入）；不渲染时取属性的整数部分，再没有则取自然尺寸。
- `decode()`：加载完成后兑现；没有图片时以 `EncodingError` 拒绝。
- `onload`、`onerror` 作为事件处理属性。
- 加载状态（HTML 的 update the image data，简化为一帧内加载完）：
  - 设了 `src` 后 `complete` 为 false。
  - 图片在下一帧的定时器之后、动画帧回调之前加载，然后触发 `load` 或 `error`。事件不冒泡、不可取消，祖先的捕获监听会收到。顺序与 Chrome 记录的相同：微任务、0 ms 的 `setTimeout`、`load`。
  - 换 `src` 时，旧的自然尺寸保留到新图加载完。
  - 去掉 `src` 后 `complete` 为 true，自然尺寸为 0。
  - 空 `src` 触发 `error`。
- `setAttribute`、`removeAttribute` 与属性赋值都会触发加载（React 用 `setAttribute` 设 `src`，用 `addEventListener` 监听 `load`）。

### 顺带修的通用布局问题

这几处不只影响图片，所有页面都会受影响：

- **块级与行内级子元素混排**：块容器里同时有块级和行内级子元素时，每段连续的行内级子元素放进一个匿名块盒（CSS 2 §9.2.1.1），各成一个行内格式化上下文。此前它们各自按块排：两张行内图片会上下叠放，也没有行盒的 strut（图片下方那几 px）。混排块里挨着行内元素的空白文本也保留了，图片之间的空格与 Chrome 一致。
- **Yoga 补丁 0004**（`third_party/yoga/patches/0004-skipped-flex-bounded-basis.patch`）：只测量不排版的那一遍，按子项被 min/max 夹过的基准尺寸累加主轴长度。此前一个带 `height` 和 `max-height` 的块会把父元素撑高。
- **Yoga 补丁 0005**（`0005-minimum-over-maximum.patch`）：最小尺寸大于最大尺寸时以最小为准（CSS 2 §10.4、§10.7），此前 Yoga 取最大。
- **flex 项在交叉轴上有 `auto` 外边距时不拉伸**（CSS Flexbox 1 §8.1）。此前 Yoga 按拉伸后的尺寸算基准，例如列方向里一张 `margin-right: auto` 的图片被拉满宽度、高度按满宽换算。
- **Yoga 补丁 0006**（`0006-flexible-lengths-loop.patch`，`CalculateLayout.cpp`）：按 CSS Flexbox 1 §9.7 解析弹性长度。先把不能伸缩的项、以及基准被 min/max 往反方向夹住的项冻结在假想主尺寸；剩余空间按其余项未夹过的基准算，按伸缩系数分配，再按 min/max 夹住；按总违例的正负冻结相应的项，循环到全部冻结。它修了两类问题：
  - **`flex: 1` 从 0 增长**：此前 Yoga 用被最小尺寸夹过的基准算剩余空间。400px 的行里两张 `flex: 1` 的图片（自然宽 40、16），weft 排成 212、188，Chrome 是 200、200。
  - **宽度失控**：此前 Yoga 第一遍把冻结项的缩小系数从总和里逐个减掉，本该是 0 的总和剩下一点浮点残差（如 +3.8e-6，符号还是反的），第二遍拿剩余空间除以它。几项一起缩到最小尺寸时，这些项会长到 1e8 px 以上，例如外层 flex 容器把一个 `width: 94px` 的 flex 项压到 30px 时，里面 `width: 100%` 的子元素宽 2.8e8。换成普通 `div` 也一样。新的循环每轮只对未冻结的项从头求和，都是非负项，不会再出现这种残差。
- **Yoga 补丁 0007**（`0007-absolute-auto-margins.patch`，`AbsoluteLayout.cpp`）：绝对定位盒子在一个方向上两侧都有 inset 时，`auto` 外边距分掉 inset、其他外边距和自身尺寸之外剩下的空间（CSS 2 §10.3.7、§10.3.8、§10.6.4、§10.6.5）。两侧都是 `auto` 时各取一半；水平方向剩余为负时起始边取 0，另一侧取全部（与 Chrome 一致，竖直方向允许两侧都为负）；只有一侧是 `auto` 时它取全部，可以为负。此前 Yoga 当作 0，`left: 10px; right: 10px; margin: 0 auto` 不会居中。适配层原先对左右都有 inset、宽度确定的盒子去掉结束侧的 inset（过约束时 CSS 2 §10.3.7 忽略它），现在只在左右外边距都不是 `auto` 时这样做。

这些补丁的原因、哈希链和退出条件记在 `third_party/yoga/metadata.json` 和 `third_party/manifest.json`。

### 证据

2026-11-08，macOS 26.6，Apple M5，Debug，Chrome 156。

- **工具与测试**：工具是 `tools/browser_img_reference.mjs`，用本地 HTTP 服务提供图片和 Ahem 字体，Chrome 与 weft 用同样的相对路径。夹具在 `tests/fixtures/img`（6 张 PNG、`script.js`、`chrome-results.json`）。测试是 `scene_img_browser_contract`（`tests/scene_img_tests.cpp`），可移植。文字用 Ahem，它的三套度量相同，行内图片的纵向位置在各平台都可比。
- **排版**：24 个手写用例和 120 棵随机树，共 144 个用例、1,313 个元素盒。
  - 135 个用例逐个元素一致（x、y、宽、高），最大差 1/64 px。
  - 9 个已知差别单独列出，不算失败（见下）。
  - 补丁 0006、0007 加了 4 个用例（"flexible lengths"、"flex items shrunk to their minimum sizes"，以及右到左和 flex 容器里的 `auto` 外边距），原先"flex grow from zero"和"absolute centered by auto margins"两个已知差别不再列为已知，后者扩充为 15 个盒子。用补丁前的 Yoga 跑同一份夹具，这些用例都不一致，"flex items shrunk to their minimum sizes"的宽度到 1.5e8 至 2.6e8 px。
  - 另用两批专找这类嵌套的随机树（300 棵和 400 棵，`width: 100%` 的子元素放在被压缩的 px 宽 flex 项里，未进夹具）对照 Chrome：补丁 0006 之前 400 棵里有一棵宽度失控（7.2e9 px），之后两批都没有；与 Chrome 不一致的检查项（每个盒子的 x、y、宽、高各算一项）从 818、1,359 处减到 705、1,266 处。剩下的没有逐一归因。
- **绘制**：35 个 `object-fit`、`object-position` 用例（四个方向的放置、圆角、内边距与边框、超出内容盒）。从 Chrome 截图里取四个色块的范围，与显示列表给出的图片位置相比，图片边缘与色块分界都在 1 px 以内。
- **脚本**：`script.js` 的 34 步与 Chrome 逐步一致。其中一步是下面的有意差别，测试里写明了 weft 的预期值。
- **全量 `ctest -j8`**：77/85，失败的仍是原来那 8 项 fixture 检查。加上补丁 0006、0007 后（`build/macos-metal`，此时已含第二阶段的 `image_decode_browser_contract`）为 78/86，失败的仍是这 8 项，`dependency_integrity` 通过。
- **Metal**：`examples/img`（macOS 示例按键 8）截图正常：自然尺寸、`width` 属性、圆角头像、五种 `object-fit`、行内徽章、`max-width: 100%`、`object-position`，以及 `load` 事件里读到的自然尺寸。

### 与 Chrome 的差别

有意的：

- 换 `src` 后、`load` 之前，weft 已按新图排版和绘制（图片同步读自宿主的文件），Chrome 在新图到达前仍显示旧图。

已知的，测试里单独列出：

- **9 棵 flex、网格嵌套在 flex、网格里的随机树**（random 51、65、72、74、96、107、108、110、113）：
  - 嵌套 flex 容器的自动最小尺寸是适配层的近似；
  - 父元素本身是行方向 flex 项、又被它的容器压缩时，它的 px 宽度不是最终宽度。子元素的百分比 `width`、`max-width` 和自动最小尺寸由适配层在排版前定下，不能按最终宽度算。例如 random 113 里 94px 的 flex 项被压到 30px，`max-width: 50%` 的图片 Chrome 是 15px，weft 是 66px。换成普通 `div` 也一样。
  - 补丁 0006 之前 random 113 里还有一个宽 2.8e8 的元素（上面的“宽度失控”），现在没有了。

其他：

- 读不到的图片：Chrome 画破图图标和 `alt` 文字，weft 什么都不画，只报诊断。

### 尚未实现

- 格式与 `data:` URL 见第二阶段；SVG 图片仍不支持。
- mip 与 `image-rendering` 见第三阶段。
- `srcset`、`sizes`、`<picture>`、`loading="lazy"`，`alt` 文字的显示，`usemap`。
- 文档页面里的 `<img>`；D2D 参考光栅不画 `<img>` 的图片。
- `aspect-ratio` 只作用于 `<img>`，其他元素报诊断。
- **未验证**：
  - Windows：D3D11、D3D12、Vulkan 的 pass 走背景图同一段代码，但没有在本机运行；只在 Windows 上跑的 Chrome 行内、flex 与绝对定位对照可能受匿名块和 Yoga 补丁 0004 至 0007 影响，未验证。
  - Android、iOS。

## 第二阶段：格式与来源

2026-11-08（`weft-ingame` 分支）。页面图片（`<img>` 与 CSS `url()`、`border-image-source`）现在能解码 JPEG、WebP，也能用 `data:` URL。

### 依赖

- **libjpeg-turbo 3.1.4.1** 与 **libwebp 1.6.0**：Chrome 用的就是这两个库（Chromium 156 自带 libjpeg-turbo 3.1.0、libwebp 1.6.0 之后的提交）。
- 源码完整、未修改，逐文件哈希记在 `third_party/manifest.json`；来源、许可证、构建选项见 `third_party/libjpeg-turbo/README.md`、`third_party/libwebp/README.md`。
- 两个压缩包的签名当时一并取得，但本机没有 gpg，**签名未验证**，只核对了 SHA256。
- libjpeg-turbo 的上游 CMake 拒绝被 `add_subdirectory`，`cmake/WeftThirdParty.cmake` 照抄它的源文件清单自建 `weft_jpeg`，配置头由它自己的模板生成；不带 SIMD（x86 上需要 NASM）。libwebp 用上游 CMake，只构建 `webp` 与 `webpdemux`。

### 做法（`src/view/view_images.cpp`、`src/view/view_image_codecs.cpp`）

- **识别格式**：看文件头，与 Chrome 嗅探图片一样，不看扩展名，也不看服务器或 `data:` URL 声明的类型。PNG（lodepng）、JPEG、WebP 解码；GIF、SVG、BMP、ICO、AVIF 报诊断并说明是哪种格式。
- **JPEG**：设置照 Blink 的 `JPEGImageDecoder`：
  - 精确整数 IDCT（`JDCT_ISLOW`）、平滑上采样、块平滑；灰度、RGB、YCbCr 直接输出 RGBA；CMYK、YCCK 按 Adobe 反相 CMYK 换成 RGB，公式与 Blink 相同。
  - 应用 Exif 方向（APP1 的 1 到 8），自然尺寸随之互换，与 Chrome 默认的 `image-orientation: from-image` 一致。
  - 不读 ICC 配置，颜色当作 sRGB。
  - 不带算术编码（Chromium 也没有）。12 位样本和算术编码的 JPEG，Chrome 给出尺寸、什么也不画；weft 一样，画成透明并报诊断。
- **WebP**：有损、无损、带透明都解码，用 libwebp 的默认选项。动画 WebP 显示第一帧（Chrome 播放前画的就是这一帧），不播放，报诊断。
- **`data:` URL**：按 Fetch 标准处理。第一个逗号之后的内容先百分号解码，类型以 `;base64` 结尾时再做宽松的 base64 解码（忽略空白，补位可省）。上限与文件相同；诊断里只引用 URL 的前 64 个字符。
- **只有尺寸**：文件头读得出尺寸、数据却解不出来时，保留尺寸、画成透明，与 Chrome 一致，同时报诊断。
- macOS、iOS 示例宿主装了资源回调；`examples/img` 加了 JPEG（渐进式）、带透明的 WebP 和一个 `data:` URL 的徽章。

### 证据

2026-11-08，macOS 26.6，Apple M5，Debug，Chrome 156。

- **夹具**：`tools/image_decode_fixtures.mjs` 用这两个库另行构建的 `cjpeg`、`cwebp`、`img2webp` 生成（不进入 weft 的构建），共 31 张，放在 `tests/fixtures/image_decode`：
  - JPEG：4:2:0、4:2:2、4:4:0、4:4:4、低质量、灰度、RGB、渐进、重启标记、算术编码、无损，8 种 Exif 方向，以及 libjpeg-turbo 自带的 4 张（含 12 位和算术编码）。源图 97 × 61，横竖都有不完整的 MCU。
  - WebP：有损、有损带透明（透明通道无损与有损两种）、无损、无损带透明、两帧动画，以及 libwebp 自带的 `test.webp`。
- **工具与测试**：`tools/browser_image_decode_reference.mjs` 让 Chrome 用 `<img>` 解码、按自然尺寸画到画布上读回像素。测试是 `image_decode_browser_contract`（`tests/image_decode_tests.cpp`），可移植。
- **结果**：
  - 31 张的尺寸全部一致；不透明像素逐字节相同，半透明像素按预乘值比较相差不超过 1 级（Chrome 的画布存的是预乘值），共 249,525 个像素。
  - 32 个 `data:` URL（每张图的 base64 一个，再加一个百分号编码、声明为 `text/plain` 的 PNG）解出与文件相同的像素；8 种错误的 `data:` URL 和非图片字节都报出对应的诊断。
- **全量 `ctest -j8`**（单独的构建目录 `build/macos-img2`，含并行的 Yoga 修正任务已登记的补丁）：78/86，失败的仍是原来那 8 项 fixture 检查；`dependency_integrity` 通过。
- **Metal**：`examples/img` 的截图里 JPEG、WebP 与 `data:` URL 都正常，没有诊断。
- **iOS 模拟器**：两个库与示例页一起编译、打包通过，没有运行。

### 与 Chrome 的差别与尚未实现

- 动画 WebP 不播放，只显示第一帧；GIF、SVG 图片、AVIF、BMP、ICO 不解码。
- 不做颜色管理：带非 sRGB ICC 配置的图片，颜色与 Chrome 不同。
- 截断的 JPEG：libjpeg-turbo 用灰色补齐缺的行；Chrome 的表现没有对照。
- **未验证**：
  - Windows：MSVC 下 libjpeg-turbo 的配置值按上游的 MSVC 分支填写，没有编译过。
  - Android：本机没有 NDK。
  - iOS 真机：2026-11-08 在 iPad 上看过示例页，见第三阶段的“未验证”。

## 第三阶段：缩放质量（mip 与 `image-rendering`）

2026-11-08（`weft-ingame` 分支）。页面图片（`<img>`、CSS `url()` 背景、`border-image`）缩小时不再闪烁，`image-rendering` 生效，过滤方式与 Chrome 相同。

### Chrome 怎么画

先用 `tools/browser_image_scale_reference.mjs` 让 Chrome 156 画了 38 个用例（下面“证据”），再用几种采样模型逐像素拟合截图（拟合脚本是一次性的，不入库），得到下面的规则。按这些规则算出的图与 Chrome 的截图，平滑用例平均差不到 1 级、最大 4 级；其他模型（只用原图、最近一级、不带偏移的三线性、三次插值）在细条纹上差几十级。

- **过滤在编码后的 sRGB 值上做**，用预乘值，不转成线性光。此前 weft 用 sRGB 纹理采样，过滤在线性光里，1 px 黑白条纹缩小后偏亮（混合后约 188，Chrome 是 128）。
- **mip 各级**：每级是上一级的一半（向下取整，至少 1），一个纹素是它盖住的 2 × 2 个纹素的平均，四舍五入；奇数边方向取 3 个，权重 1、2、1（Skia 的做法）；用预乘值。
- **选级**：三线性。级别是 log2(每个设备像素的纹素数) 减 0.5（Skia `SkMipmap::ComputeLevel` 的 -0.5 偏移），不大于 0 时只在原图上双线性。例如 256 px 画成 100 px，原图占 14%、第 1 级占 86%。
- **放大**：双线性，不是三次插值。`auto`、`smooth`、`high-quality`、`-webkit-optimize-contrast` 画法相同。
- **`pixelated`、`crisp-edges`**：在原图上取最近的纹素，放大缩小都如此，不用 mip。属性可继承。
- **背景图**：一块图块盖住整个绘制区时按图片画，用 mip；图块重复铺开时 Chrome 按图案画，不用 mip，只在原图上双线性（256 px 的图按 50 px 平铺，逐像素与“原图双线性”吻合）。

### 做法

- **页面图片的纹理**（`src/scene/scene_native.cpp` 的 `ScenePreparer::page_image`）：预乘的编码 sRGB，格式 `Bgra`（与文字图片相同），带 `scene_image_mips` 生成的各级，单独缓存。effect 与粒子的纹理不变。
- **着色器**（`src/render/shaders/scene.hlsl` 的 `fittedTexture`）：
  - 页面图片按上面的规则采样：`pixelated` 取最近纹素；一块图块时用 `SampleGrad`，导数乘 2^-0.5；图块重复时用第 0 级。采到的值直接是预乘的编码色。
  - 宿主纹理照旧：线性光，按导数选级。
  - 导数改用记录里的仿射逆矩阵计算，不再用 `ddx`/`ddy`。图片边缘的像素四元组里有被丢弃的像素，Metal 上 `ddx`/`ddy` 在那里没有定义。有了 mip 之后，边缘那一列会选到 1 × 1 级，对照时发现。
  - 记录里的 `stops[3].z` 表示页面图片，`stops[3].w` 表示 pixelated（`src/render/scene_pass.cpp`）。
- **样式**：`image-rendering` 加入 scene 的属性表，可继承，只认 CSS 的关键字。此前在宽松模式里被静默忽略。`paint::Image::pixelated` 对背景图、`border-image` 和 `<img>` 都生效。
- **上传**：
  - Metal 上传全部级别。
  - D3D11 也上传全部级别（`D3D11_USAGE_IMMUTABLE` 的初始数据）。
  - D3D12、Vulkan 只上传第 0 级：缩小时仍在原图上双线性，会闪烁，但过滤空间与 Chrome 一致。

### 证据

2026-11-08，macOS 26.6，Apple M5，Debug，Chrome 156（无头）。

- **工具与夹具**：
  - `tools/browser_image_scale_reference.mjs` 生成测试图（1 px 黑白竖条与 2 px 红蓝横条、棋盘格、半透明条纹、8 × 8 像素画），连同 `examples/img` 的风景图一起放在 `tests/fixtures/image_scale`。
  - 它还写出建页面的 `page.js` 和各用例的位置 `cases.json`，并用 Chrome 截图存为 `chrome.png`。
  - 用例共 38 个：`<img>` 从原尺寸缩到 1/16，覆盖各个级别和级间；放大；5 个 `image-rendering` 关键字及继承；单块、平铺和 pixelated 的背景图。
- **`image_scale_browser_reference_metal`**（`tests/image_scale_metal_tests.mm`，只在 Apple 平台）：
  - 同一页面在 View 里由 Metal pass 画出、读回，逐用例与截图比较。
  - 38 个用例全部通过：除下面说的平局像素外，每个通道相差不超过 4 级，多数不超过 2 级。
  - pixelated 的图里，像素中心恰好落在两个纹素之间的“平局”像素单独计数，不算失败：3 个用例共 174 个。
  - 变异检查：去掉 -0.5 的偏移后有 7 个用例失败。
- **`scene_image_mips_contract`**（可移植）：取整、奇数边的 1-2-1 权重、逐通道、只有一列、1 × 1 与尺寸不符。
- **全量 `ctest -j8`**（`build/macos-img2`）：81/89，失败的仍是原来那 8 项 fixture 检查。

### 与 Chrome 的差别

- **pixelated 的平局像素**：Chrome 取哪一个纹素，取决于它光栅流水线里浮点运算的舍入。同一列、同一行里两种取法都有，无法逐像素复现。weft 取右边、下边的那个。
- **只对照了无头 Chrome**：它的截图与 Skia 软件光栅的规则吻合。开着 GPU 光栅的 Chrome 选级和过滤可能略有不同，没有对照。

### 尚未实现与未验证

- D3D12、Vulkan 只有第 0 级（见上）。
- 宿主纹理不按这些规则画：仍在线性光里过滤，不带 -0.5 的偏移。它们不是网页图片，没有对应的 Chrome 行为可对照。
- 文档页面、D2D 参考光栅仍不画页面图片。
- **未验证**：
  - Windows：D3D11 的多级上传和三个 Windows 后端共用的着色器改动没有编译，也没有运行。共享的 HLSL 在本机由 DXC 编译通过（Metal 与 Vulkan 走这条路），D3D11 用的 fxc 没有编译过。
  - Android。
  - iOS 只在 iPad 上看过示例页（2026-11-08，iPad Pro 13 英寸 M4，见[视觉效果](visual-effects.md)的“iPad 真机”）：
    - `examples/img` 与 macOS 的渲染相比，图片逐像素相同；
    - 第二行起整体低 1 个逻辑像素：第一行里行内图片下面的空间取决于字体的下降值，iOS 与 macOS 的系统字体不同；
    - `examples/effects` 里缩小与像素化的几格逐像素相同。
