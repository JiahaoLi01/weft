# 文字方向与复杂文字

2026-11-07 起（`weft-ingame` 分支），scene 页面可以排从右到左的文字（阿拉伯文、希伯来文）与需要塑形的文字（阿拉伯文的连写、天城文的合体字、泰文的上下标），并按 Chrome 的规则处理双向混排。第一阶段管"字怎么排"（行内格式化上下文）；第二阶段（同日）管"盒子怎么排"（块、弹性、网格、绝对定位、滚动）、从右到左行上的省略号和文本框；第三阶段（同日）让文字跨行内元素的边界塑形。本文记录三个阶段的范围、做法、与 Chrome 的对照证据和尚未做的部分。

## 第一阶段：文字

范围只是行内格式化上下文里的文字。

- **CSS**：
  - `direction`：`ltr`、`rtl`，继承。
  - `unicode-bidi`：`normal`、`embed`、`isolate`、`bidi-override`、`isolate-override`、`plaintext`，不继承。
  - 两者都进了 scene 的样式表，宽松页面也不再忽略它们（`writing-mode` 仍被忽略，只有横排）。
- **HTML**：
  - `dir="ltr"`、`dir="rtl"`：设方向，并且除 `bdi`、`bdo`、`output` 外都是隔离（Chrome 的 `HTMLElement` 表现属性）。
  - `dir="auto"` 和没有 `dir` 的 `bdi`：方向取元素文字里第一个强方向字符（跳过 `bdi`、`script`、`style`、`textarea` 和带 `dir` 的子孙），没有时为 `ltr`。它在层叠里作为表现属性生效，所以子孙会继承这个方向，与 Chrome 相同。
  - `bdi` 是隔离，`bdo` 是隔离加覆盖（`isolate-override`，Chrome 现在的 html.css），`textarea[dir=auto]` 是 `plaintext`。
  - 只认小写的属性值（严格样式表没有不区分大小写的属性选择器）。
- **排版**：
  - 段落的基础方向来自根的 `direction`；`unicode-bidi: plaintext` 时每段（强制换行之间）取第一个强方向字符的方向。
  - 行内元素的 `unicode-bidi` 在它前后插入双向控制字符，与 Blink 的 `EnterInline` 一致（`isolate-override` 是 FSI 加覆盖）。强制换行前后会关闭再重开这些上下文（Blink 的 `AppendForcedBreak`）。
  - 文字按双向层级切成段，每段按自己层级的方向单独塑形。
  - 每行按层级重排（UAX #9 L2）：行内盒按重排后视觉上连在一起的范围拆成几片，盒的行左边缘放在最左一片，行右边缘放在最右一片；从右到左的盒子起始边在右侧。
  - `text-align: start/end` 按行的方向；溢出的从右到左行向左溢出。
- **字体**：
  - 塑形前按文字系统切分（UAX #24），每段告诉 HarfBuzz 方向和文字系统，阿拉伯文、希伯来文、泰文、天城文等走各自的塑形器。
  - 系统后备字体按文字系统挑，排在页面字体之后、一般后备之前：
    - macOS 用 CoreText 的 `CTFontCreateForString`，与 Chrome 在 macOS 上的做法相同；
    - Windows 用 DirectWrite 在 Windows 10/11 上给出的字体表（Segoe UI、Nirmala UI、Leelawadee UI 等）；
    - Android 用 fonts.xml 里字体的语言标签（如 `und-Arab`）。

### 做法

依赖：[SheenBidi 3.0.0](../third_party/sheenbidi/README.md)（Apache-2.0，Unicode 17），提供双向算法与文字系统切分。HarfBuzz 不做这两件事，项目规定不自研 Unicode 数据或算法。上游测试在本机全部通过：BidiTest 与 BidiCharacterTest 共 91,707 行，0 错误。自己写的只有 UAX #9 L2 在一行各段层级上的重排，Blink 也是这样做的（`IndicesInVisualOrder`）。

- `src/text/bidi.*`（库 `weft::bidi`）：
  - `bidi_levels` 求每个字节的层级。结果再按 ICU `ubidi_setPara` 的方式调整（Blink 的层级来自 ICU）：段末与分隔符前的空白和控制字符取段落层级；被 X9 移除的控制字符取后一个字符的层级。
  - `bidi_visual_order` 做 L2 重排。
  - `script_runs` 切文字系统；`bidi_first_strong_rtl` 给 `dir=auto` 用。
- `src/text/font.*`：`Font::shape` 和 `shape_with_fallback` 可以接受一段的方向和文字系统（`ShapeSegment`）。从右到左的段，字形按视觉顺序输出。不给段时保持原来的从左到右限制（旧的 `FontSet` 路径）。
- `src/scene/scene_text.cpp`：
  - `SceneFont` 带基础方向和是否覆盖。
  - `SceneText::shape` 先按层级切段并重排，段内再按单词和文字系统切分。
  - 后备字体按段的文字系统匹配（`match_font_faces` 的 `script` 参数，`FontCatalog::script_fallbacks`）。
- `src/scene/scene_inline.cpp`：
  - 收集阶段插入控制字符（`Item::Kind::Control`）。
  - 求出段落层级后，按层级拆分文字项，再按层级方向塑形。
  - 有双向内容（或有从右到左的行内盒）时，每行按视觉顺序放置（`place_visually`）。只有 Blink 会给盒片段的行内盒才占位：有外边距、边框、内边距、背景、阴影、轮廓或相对定位，或者子盒有外边距、垂直对齐或字体度量不同。没有这些的盒子 Blink 会剔除，不参与重排。
  - 文字片段带 `bidi` 与 `rtl` 标记，绘制时按同一方向重新塑形（`scene_text_line` 写入 `direction` 与 `unicode-bidi: bidi-override`）。
- `src/style/style.cpp`：`dir=auto` 的方向作为表现属性参与层叠，记忆键里包含它。

对照中发现、并照 Chrome 改了的换行行为。这些和双向文字无关，从左到右的页面同样受影响：

- 一串空格之后总可以换行，即使 UAX #14 不允许（LB14 至 LB17，如 `( x`、`[ x`），对应 Blink 的 `BreakSpaceType::kAfterSpaceRun`。
- 原子内联之后总可以换行；紧贴在文字之后的原子内联，前面也可以换行（`CanBreakAfterAtomicInline`、`CanBreakAfter`）。
- 双向控制字符不占换行机会：换行发生在它之后的开标签前，它留在前一行（`LineBreakBidiControlEnter`）。不在同一字节位置的换行机会会传给下一个能接的单元。
- `<br>` 之后紧跟的闭标签和退出控制字符留在 `<br>` 那一行（Blink 的行尾项）。
- 行尾空格之后的闭标签（行内盒的结束内边距与边框）不计入是否放得下，可以溢出；段落末尾不适用。

### 证据

`tools/browser_bidi_reference.mjs` 在 Chrome 156（macOS）上录制，产物是 `tests/fixtures/scene_bidi_reference.json`；`scene_bidi_browser_contract`（`tests/scene_bidi_tests.cpp`）用同一批字体文件与之对照。字体都是仓库里 HarfBuzz 自带的测试字体，以 data URL 嵌进页面：Roboto、Amiri（阿拉伯文）、Noto IKEA Hebrew、Kanit（泰文）、Noto Sans Devanagari。这个测试不依赖系统字体，可以在任何平台运行。

- **用例**：29 个手写用例和 200 个随机用例（固定种子）。
  - 手写用例覆盖各种 `dir`、`unicode-bidi`，`bdi`、`bdo`，嵌套隔离，括号镜像，阿拉伯数字，`lam-alef` 连字与附加符号，以及泰文、天城文与混合文字。
  - 随机用例是各文字系统的词，混合数字和标点，放进随机方向、随机 `unicode-bidi`、带或不带边缘的 span、`bdi`、inline-block 和 `<br>` 里，块的方向、对齐和宽度也随机，多数会换行。
- **比较什么**：
  - 每个元素的 client rects：行内盒的各片段；Chrome 剔除的盒子比较每行的范围。
  - 每个文本节点的片段：每行、每个层级各一个矩形，比较起点和宽度，宽度反映塑形结果。
  - 每个单独成簇的字符的位置与前进宽度：检验重排后的顺序，以及绘制时用的方向。
- **结果**（2026-11-07，macOS 26.6，Apple M5，Debug）：
  - 229 个用例全部一致：859 个元素矩形、1,294 个文字片段、6,042 个字符，最大水平差 0.029 px。
  - 另外两个种子各 200 个随机用例，共有 4 个仍有差别，约 1%，见下文。
- **垂直方向**：按"在第几行"比较。scene 的行高度量取 Chrome 在 Windows 上的值（字体的 OS/2 Win 或 typo 度量），这份参照来自 macOS 的 Chrome（hhea 度量），两边高度差的是字体度量，与双向无关。
- **全量 `ctest -j8`**：74/82，失败的仍是原来那 8 项 fixture 检查。新的换行规则没有改变其他已有的 Chrome 对照。
- **系统字体**：`examples/bidi` 在 macOS 示例里（按键 7）用系统字体正确显示阿拉伯文连写、希伯来文、`bdi`、`dir=auto`、泰文和天城文合体字。截图由 `weft_macos_demo --shot … --page bidi` 生成。

## 第二阶段：盒子、省略号与文本框

范围与做法：

- **块、弹性、绝对定位**（`src/layout/flex.cpp`）：块和 flex 都经 Yoga 排，每个盒子按自己的 `direction` 设 Yoga 方向（`YGNodeStyleSetDirection`）。于是：
  - 从右到左容器里，定宽的块级子项靠右；`margin-left: auto` 把它推向右边、`margin-right: auto` 推向左边；过约束时忽略 `margin-left`（CSS 2 §10.3.3）。
  - flex 的行从右排起，`justify-content` 的 `flex-start`、`start` 在右；`row-reverse` 从左排起；列方向的交叉轴起点在右。
  - 绝对定位的盒子没有水平 inset 时，静态位置在右；`left`、`width`、`right` 都给定时，按**包含块**的方向忽略一侧（从右到左忽略 `left`，CSS 2 §10.3.7）。Yoga 用的是父盒的方向，包含块在更上层时会选错，所以适配器先按包含块的方向去掉被忽略的那个 inset。
  - 物理边（外边距、内边距、边框、inset）不随方向翻转。
- **网格**：按从左到右算出轨道，从右到左时把每个网格区域在内容盒里镜像（列线从右边起），网格项在区域里由 Yoga 按网格的方向排（`justify-self: start` 在右，外边距仍在原来的物理一侧）。绝对定位子项的网格线区域同样镜像，`auto` 取对应一侧的内边距边。
- **滚动**：从右到左的滚动容器，内容向左溢出；`scrollLeft` 从 0（起点在右）到 −(scrollWidth − clientWidth)，与 Chrome 一致。滚轮、触摸平移、键盘、`scrollIntoView` 与空间导航都按这个范围（`ElementGeometry::scroll_left_min/max`）。
- **省略号**（`text-overflow: ellipsis`，`src/scene/scene_inline.cpp` 的 `truncate`）：照 Blink 的 `LineTruncator`，按视觉顺序从行尾一侧（从右到左时是左边）处理。与行同方向的片段保留它的逻辑开头，反方向的保留逻辑结尾；从右到左的行上，截短后的片段贴着原来的右边，省略号在它左边。
- **文本框**（`src/scene/scene_field.cpp`、`src/view/view.cpp`）：
  - 输入框的文字铺满内容盒，由 `text-align` 放（UA 表改为 Chrome 的 `text-align: start`），从右到左时靠右；溢出向左，横向滚动从 0 到负值。
  - 光标停在后一个字符的前缘（从右到左的字符是右边）。两个层级交界处，光标站在层级较低的一侧；段落开头和结尾在视觉行的起点和终点（Blink 解析光标位置的规则）。
  - 选区按被选字符的实际位置画，混排时可能是几段。选中的换行画在行尾一侧（从右到左在左边）。
  - 左右方向键：光标按文本框的方向逻辑移动（从右到左时左键是下一个字符，与 Chrome 相同，文字里夹着拉丁文也是）；收起选区时去视觉上那一侧的端点。
  - 每个字形记下自己的方向（`SceneGlyph::rtl`），有从右到左文字的行另存每个字符的范围（`SceneLine::extents`）。

证据（2026-11-07，macOS 26.6，Apple M5，Debug，Chrome 156）：

- **盒子**：新工具 `tools/browser_rtl_box_reference.mjs`，参照 `tests/fixtures/scene_rtl_box_reference.json`，测试 `scene_rtl_box_browser_contract`。
  - 16 个手写用例：块与百分比外边距、嵌套方向、flex 行、换行、列、`order`、绝对定位、静态位置、网格、网格绝对定位、inline-block、滚动容器与设定的 `scrollLeft`、文字块。
  - 150 棵随机树（块、flex、网格，随机方向、尺寸、外边距、对齐与绝对定位子项），每棵先跑它的从左到右孪生树。孪生树和 Chrome 一致的才比较从右到左版本，这样比出来的差别只来自方向。101 棵参与比较，全部一致；另 49 棵的从左到右版本本来就和 Chrome 不同（见下）。
  - 共 2,994 个元素盒（逐个比 x、y、宽、高）和 8 个滚动容器（`scrollLeft`、`scrollWidth`），最大差 1/64 px。inline-block 的 y 不比较，它取决于字体度量的平台差别。
- **省略号**：`tools/browser_bidi_reference.mjs` 新增 6 个用例，从 Chrome 截图里取省略号（红色）和保留文字（黑色）的墨迹范围，weft 用同样的字形位图算出对应范围，相差在 2 px 以内。把从右到左的截断改坏，会出现 16 处失败。
- **文本框**：新工具 `tools/browser_rtl_field_reference.mjs`，夹具在 `tests/fixtures/rtl_field`，测试 `scene_rtl_field_browser_contract`（在 View 里跑，可移植）。
  - 6 个文本框（从右到左的希伯来文、阿拉伯文、混排、拉丁文，从左到右里夹希伯来文，从右到左的 textarea）。
  - 30 个光标位置和 18 个选区：Chrome 截图里绿色光标所在的列与蓝色选区的列段，误差 1 px 以内。
  - 48 次方向键之后的 `selectionStart` 与 `selectionEnd`。
  - 全部一致。
- **全量 `ctest -j8`**：76/84，失败的仍是原来那 8 项 fixture 检查。
- **截图**：`examples/bidi` 增加了从右到左的 flex 行、截断的标题和输入框，macOS 系统字体下显示正常。

对照中顺带发现、与方向无关的已有差别：随机树的从左到右版本有 49 棵和 Chrome 不同。

- 块级外边距不折叠（margin collapsing 未实现）。
- 宽度为 auto 的块带 `auto` 外边距时，Chrome 把 auto 当 0 并铺满，weft 收缩到内容宽。
- 网格项比轨道宽时的 `justify-items: end` 等对齐，以及网格项里的 `auto` 外边距。
- 网格项内部的绝对定位后代够不到网格项之外的包含块（网格项在自己的 Yoga 根里排）。
- 静态位置不跟在前面的流内兄弟之后（垂直方向，原有限制）。

这些都没在这次改。

## 第三阶段：跨元素塑形

做法（照 Blink 的 `InlineNode::ShapeText`）：

- **一起塑形**（`src/scene/scene_inline.cpp`）：同一段落里相邻的文本项，字体相同（字族、字号、字重、斜体、数字变体、`letter-spacing`；行高不影响塑形）、双向层级也相同时，拼成一段交给 HarfBuzz。
  - 中间的开、闭标签不打断，除非它在那一侧有外边距、边框或内边距，或者 `vertical-align` 不是 baseline（Blink 的 `ShouldBreakShapingBeforeBox`、`ShouldBreakShapingAfterBox`）。
  - 双向控制字符、原子内联和 `<br>` 会打断。
  - 塑形结果按字符所属的文本项切开，连字的字形归它第一个字符所在的项。
- **上下文**：每次塑形都把段落里前后各 5 个字符作为 HarfBuzz 的上下文（`ShapeContext`，`constants::text::shape_context_characters`）。HarfBuzz 本身两侧各只保留 5 个（`hb_buffer_t::CONTEXT_LENGTH`），所以这和 Blink 传整段等价。即使塑形被打断（带内边距的 span、不同字号），阿拉伯字母也按两侧的字母取连写形。
- **绘制用布局的字形**：一起塑形的文字，每行记下布局时的字形（`SceneLine::run`、`SceneNode::text_run`）。文字图像、Direct2D 光栅和文本框直接用这组字形，不再单独塑形，跨边界的连字和字距才画得出来。没有一起塑形的文字，带着上下文（`SceneNode::text_context`）重新塑形。原生场景的缓存键包含这两项。
- **整段塑形，不再按词切**（`src/scene/scene_text.cpp` 的 `words`）：
  - 之前每个空格和制表符都单独塑形，仿的是 Blink 旧的 `CachingWordShaper`。
  - LayoutNG 实际按整段塑形，`HarfBuzzShaper` 只按文字系统、emoji 和方向切分。所以字体里空格与后一个字母的字距对（Roboto 的 " T"）在 Chrome 里生效。
  - 现在只有 CJK 表意文字和符号仍各自单独塑形。

证据（2026-11-07，macOS 26.6，Apple M5，Debug，Chrome 156）：

- **用例**：`tools/browser_bidi_reference.mjs` 新增 6 个，比较方式与第一阶段相同（元素矩形、文字片段、逐字符位置与宽度）。
  - 被变色 span 切开的阿拉伯词，包括带 tatweel 的；
  - 被带内边距的 span 切开的阿拉伯词（塑形断开，只靠上下文连写）；
  - 被大字号 span 切开的阿拉伯词；
  - 跨 span 的拉丁字距（AV、WA、Y、" To"）；
  - 单独放在 span 里的阿拉伯附加符号；
  - 被 span 切开的天城文。
- **结果**：241 个用例全部一致，包括 867 个元素矩形、1,315 个文字片段和 6,097 个字符，最大水平差 0.029 px。
- **反向检查**：
  - 关掉一起塑形和上下文，6 个新用例里有 5 个失败，共 103 处，最大差 26 px。天城文用例的 span 恰好落在音节边界上，不受影响。
  - 只恢复按空格切词，" To" 的空格宽 0.38 px，后面的字母随之偏移。
- **其他对照**：盒子方向、文本框和字形缓存的对照仍全部一致。全量 `ctest -j8` 为 76/84，失败的仍是原来那 8 项 fixture 检查。

## 与 Chrome 的差别与尚未实现

已知差别（另两个种子里的 4 个随机用例）：

- 原子内联前后紧挨双向控制字符时，换行位置可能不同。Chrome 的 ICU 断行和 libunibreak 在 CB 与 CM 的组合上不一致。
- 从右到左的带边框行内盒里含 `<br>` 时，Chrome 有时把盒的起始边单独拆成一片。
- 冒号、货币符号与折叠空格交界处的换行（UAX #14 LB25 的实现差别，libunibreak 本身也跳过 34 项这类测试）。

尚未实现：

- **字距与语言**：从右到左时 `letter-spacing` 的位置，以及 `lang` 对塑形的影响（如塞尔维亚文字形、乌尔都文数字）。
- **泰文、老挝文、高棉文、缅甸文按词典断行**：需要词典数据（ICU 或 libthai），要先定依赖。现在这些文字只在空格处换行。
- **文本框**：混排文字里方向键按逻辑而不是视觉移动；从右到左输入框溢出时的滚动只在本机推理过，没有对照 Chrome。
- **表格列序**、**竖排**（`writing-mode`）。
- **未验证**：Windows（包括只在 Windows 上跑的 Chrome 行内与文本框对照：新的换行、输入框排版和整段塑形都可能影响它们，整段塑形会改变带空格字距对的字体的宽度）、Android 与 iOS；Windows 与 Android 的按文字系统后备字体只写了代码。
