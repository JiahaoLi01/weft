# 开发者工具

2026-11-10：定下分模式方案，完成构建开关、两版共用的公开接口和共享库；第一阶段（协议通道和元素面板）已能用 Chrome 自带的 DevTools 前端查看 Weft 页面；第二阶段（热重载、资源走开发连接、和 Chrome 对比）已完成。尚缺的内容见[现状](#现状)。

## 怎么用

开发版的宿主设好环境变量启动（宿主代码不用改），stderr 会打印前端地址，粘贴到 Chrome 的地址栏：

```sh
WEFT_DEVTOOLS_LISTEN=9230 ./game
# [weft devtools] open devtools://devtools/bundled/devtools_app.html?ws=127.0.0.1:9230/devtools/page/1-1
```

检查页面用到了哪些引擎不支持的 CSS，可以放进 CI，有问题时退出码为 1：

```sh
node tools/devtools_report.mjs 127.0.0.1:9230
```

改页面不用重新构建、安装游戏。把本机的页面文件夹交给运行中的游戏，之后存盘即生效（见[热重载](#热重载)）：

```sh
node tools/devtools_watch.mjs 127.0.0.1:9230 path/to/page
```

把游戏里现在这份文档放进 Chrome 再排一遍，列出盒子不一样的元素（见[和 Chrome 对比](#和-chrome-对比)）：

```sh
node tools/devtools_compare.mjs 127.0.0.1:9230
```

游戏跑在别的设备上时，监听地址写 `0.0.0.0:9230`，工具里把 `127.0.0.1` 换成设备的地址。IPv6 地址加方括号，例如 `[fd2a:71e3:3854::1]:9230`。

## 目标

工具优先回答四个问题：这个元素为什么长这样、这一帧为什么慢、游戏和 UI 之间发生了什么、改完多久能看到。

页面本来就能在 Chrome 里跑，纯 JS 逻辑问题在 Chrome 里查就够了，所以优先做只在 Weft 里才出现的问题。验收标准沿用 [M5](implementation-plan.md#m5-开发者交付)：不读核心代码，就能定位未支持的 CSS、错误尺寸和慢更新。

## 两版二进制

引擎出两版，头文件和导出的函数完全相同：

| | 发布版 | 开发版 |
|---|---|---|
| 构建选项 | `WEFT_DEVTOOLS=OFF`（默认） | `WEFT_DEVTOOLS=ON` |
| 开发者工具的代码 | 没有，`src/devtools` 不参与编译 | 有，用到时才启动 |
| `weft_devtools_*` | 只返回 `WEFT_UNAVAILABLE` | 可用 |
| 用途 | 发给玩家 | 开发和测试 |

两版都是优化构建，和编译配置（Debug、Release）是两回事。开发版要能测性能，所以不能是未优化的 Debug。

### 怎么选

- **有源码**：配置时打开 `-DWEFT_DEVTOOLS=ON`。
- **只有二进制**：换文件。两版文件名相同（`weft.dll`、`libweft.dylib`），导出相同，宿主不用重新编译，也不用重新链接。
- **静态链接**（iOS）：链接时选哪一版的库。

用什么条件选哪个文件是集成方的事，比如按游戏的打包配置。Weft 不假设宿主是什么，宿主代码里不需要 `#if`：调用 `weft_devtools_start` 在发布版里只是拿到一个错误码。

### 开发版怎么打开

开发版默认不监听端口，要么由宿主调用，要么不改宿主、只设环境变量：

```sh
WEFT_DEVTOOLS_LISTEN=9230 ./game            # 只允许本机连接
WEFT_DEVTOOLS_LISTEN=0.0.0.0:9230 ./game    # 允许别的设备连接
```

```c
weft_devtools_start(runtime, NULL);         /* 本机，从 9230 起第一个空闲端口 */
```

环境变量写的端口被占用时往后找空闲端口，结果打印到 stderr；地址写错或监听失败也打印到 stderr，runtime 照常创建。需要确切端口时用 `weft_devtools_get_info`。

### 为什么不是别的做法

- **一个二进制加运行时开关**：工具代码会进玩家的包，热路径上也要多判断。
- **核心库加一个可选的工具 DLL**：核心里仍要留调用点，两个 DLL 之间还要维持一套内部 ABI，而工具要读 DOM、样式、布局这些内部结构。

两版的代价是每次发布多构建一次，以及开发版不小心发出去的风险。后者靠两点控制：开发版默认不监听，打包脚本可以用 `weft_get_build_info` 检查拿到的是哪一版。

### 发布版里剩下什么

`weft_get_build_info`、三个 `weft_devtools_*` 空函数，以及错误码 `WEFT_UNAVAILABLE` 和它的文字。没有线程，没有 socket 调用，也没有工具相关的字符串。

以后加进引擎内部的记录（CSS 来源、失效原因、trace）都要放在 `WEFT_DEVTOOLS` 后面。这个宏由 `weft_add_library` 公开给所有模块，内部结构可以按它变化，凡是链接模块的目标看到的值都一样。

两版必须用相同的后端选项构建（`WEFT_RENDER_VULKAN` 等），否则导出列表不同，就不能互换。

## 协议

走 Chrome DevTools Protocol（CDP）：

- 元素、Console、Sources、Performance 用开源的 Chrome DevTools 前端，固定版本。面板跑在开发机上，不用在游戏里画一套 UI。
- Weft 特有的内容走自定义的 `Weft.*` 协议域，配一个单独的页面，再加一个给 CI 和脚本用的 CLI。
- socket 在引擎开发版里，宿主什么都不用写。这是因为不能预设宿主是什么，只有 DLL 的人也写不了。将来遇到不能用 socket 的平台，再加一个由宿主收发消息的接口。
- 网页发起的 WebSocket 只接受 `devtools://` 和本机来源。否则开发者打开的任意网站都能连上这个端口，在游戏里执行脚本。

默认只监听本机。这个端口以后能在游戏里执行 JS，所以监听其他网卡必须明确要求。架构里“没有默认网络请求”说的是页面发起的请求，不受影响。

## 功能

**这个元素为什么长这样**：实时 DOM，在游戏画面上点选元素，计算样式，盒模型高亮，现场改样式，强制 `:hover` 等状态；命中的 CSS 规则带原始文件和行号；被丢弃的声明划掉并写明原因，加一份能放进 CI 的整页兼容报告；和无头 Chrome 对比，按元素列出尺寸和位置的差异；每段文字实际用的字体。

**这一帧为什么慢**：串起输入、JS、样式、布局、绘制准备、提交和 GPU 完成的时间线；失效原因和强制同步布局的警告；叠加显示重绘区域、组的范围和 backdrop 复制，开销归到具体元素；图集、图片缓存、字体、JS 堆、detached 节点的账目。

**游戏和 UI 之间发生了什么**：`weft.engine` 的事件、调用和视图模型，可以发假事件；焦点在四个方向上的去向，手柄模拟；输入事件的命中路径；锚点和外部纹理的状态；暂停、单步、慢放动画，模拟视口、缩放比、语言和 RTL。

**改完多久能看到**：CSS 热替换并保留页面状态；JS 重载到新的 VM，失败时保留旧页面；资源走开发连接，设备上的页面直接读开发机的文件；帧捕获和回放。

**JS 调试**：先做 Console、求值和带 source map 的堆栈，断点和单步放最后。QuickJS-NG 0.17 有行号表，但没有调试钩子，需要打补丁。

不做 Network、Lighthouse、Application 这些面板，Network 换成资源请求列表。

### 顺序

1. 协议通道和元素面板：点选、命中规则及来源、未支持诊断、Console。（已完成，尚缺的见[现状](#现状)。）
2. 热重载、远程资源、和 Chrome 对比。（已完成。）
3. 时间线、失效原因、渲染叠加、资源账目。
4. 桥接、焦点、时间控制。
5. JS 断点、React DevTools、捕获回放。

## 热重载

页面的内容由工具经开发连接送进引擎，宿主不参与，所以设备上的游戏不用重新构建、签名、安装，宿主也不用为此写代码。

| 改了什么 | 引擎做什么 | 页面的状态 |
|---|---|---|
| 样式表、关键帧 | 用新样式表重新做场景 | 保留：DOM 和脚本状态不动。CSS 动画和过渡从头开始 |
| 脚本、`page.json` | 在一个新的 View 里加载新页面，成功后换上去 | 重新开始。`sessionStorage`、`localStorage` 和页面时间延续 |
| 图片、字体 | 重新读页面的文件 | 保留 |

**失败时保留原来的**，原因显示在 Console 和工具的输出里：

- 新样式表做不出场景：样式表不换。
- 新页面加载失败（脚本抛错、样式表出错、做不出第一帧）：旧页面照常运行，脚本状态不变。旧页面还在等的引擎调用、已经排队的事件和视图模型字段都还在。

实现上，新页面在另一个 View 对象里试加载，它和旧 View 共用宿主给的函数、读取器、剪贴板和存储。引擎通道在试加载前做检查点，失败时恢复。只有 C API 一级（`weft_view_*`）才有这个保护。

**页面顶替的范围**：

- 工具送来的页面顶替宿主加载的同一页，按宿主给的 `base` 判断。宿主再次加载这一页（比如重新进入关卡）时，加载的仍是工具送来的版本；加载别的页不受影响。
- 游戏切到别的页面期间，工具送来的样式表、页面和文件只是存下来，不会套到正在显示的那一页上，`Page.reload` 重载的也是正在显示的那一页。宿主切回来时用存下来的版本。
- 存下来的页面没有试加载过。切回来时如果它加载失败，改为加载宿主自己的页面，失败原因进 Console。
- 工具断开后，宿主下一次加载恢复成它自己的内容。断开本身不触发重载。
- 工具送来的文件按解析后的路径覆盖宿主的读取器，没送的仍由宿主读取。所以包里旧的图片会被本机的新图片盖住，文件夹里没有的资源仍从包里读。
- 在 Styles 面板里改规则只是临时的，下一次重载或推送就没了。

**监视工具怎么确定页面**（`tools/devtools_watch.mjs`）：

- 文件夹里有 `page.json` 时按它来，格式和示例宿主用的一样。
- 没有时按 View 已加载的页面来：每个脚本按名字在文件夹里找，样式表按 `base` 的文件名找，关键帧找 `<样式表名>-keyframes.css` 或 `keyframes.css`（也可以用 `--keyframes` 指定）。文件夹里没有的保持 View 现有的。
- 文件夹下的图片和字体全部送过去，上限 256 MB。
- 配合打包器时，监视打包输出的目录即可。
- 启动时文件夹的页面顶替游戏正在显示的那一页。如果那一页的样式表文件名和文件夹里的不一样（游戏停在别的界面上），监视工具先等游戏切到这一页；`--any-page` 直接顶替当前这一页。两个页面的样式表同名时分辨不出来。
- 只打印启动之后页面说的话，之前留在 View 里的 Console 输出不重放。

**没有做的**：脚本状态不迁移（要保留的状态放进 `sessionStorage`）；没有模块级的热替换（React Fast Refresh 那一类）。

## 和 Chrome 对比

回答“Chrome 里是对的，游戏里不对”：`tools/devtools_compare.mjs` 取游戏里**现在**这份文档，在无头 Chrome 里重建，再逐元素比较 `getBoundingClientRect()` 和 Weft 给的盒子。

- **比的是同一份文档**：元素、属性、文本取自引擎当前的 DOM，文本框的值、滚动位置、hover / active / focus 状态一起带过去。Chrome 里按节点重建，不解析 HTML，也不跑页面的脚本，所以游戏驱动出来的状态原样保留，不需要在 Chrome 里模拟游戏。
- **Chrome 的环境**：和仓库里的 Chrome 参考脚本一致。场景的 UA 规则里比 Chrome 多的那条（`* { box-sizing: border-box; border-style: solid; border-width: 0 }`）放在最低优先级的层里给 Chrome，`--chrome-defaults` 可以去掉它；场景根元素在 Chrome 里和在 View 里一样，是视口大小、子元素外边距不穿透、绝对定位以它为参照的盒子。
- **只列元素自己的差异**：跟着父元素移动的、被前一个兄弟推开的、跟着父元素拉伸的都只计数。同类兄弟差异相同时合并成一条。元素命中的规则里有引擎不支持的声明时，一并列出。
- **不比的**：正在动的元素（脚本动画，以及 Chrome 里还在跑的 CSS 动画和过渡）；像素和颜色；`canvas` 的内容；`env(safe-area-inset-*)`（Chrome 里是 0，报告会提示）。
- `--out <目录>` 留下 `chrome.html`（同一份文档，可以在 Chrome 里打开，和游戏并排用 DevTools 看）和 `report.json`。有差异时退出码为 1。

两边都用系统字体。页面的图片和字体从 `base` 所在的文件夹读；`base` 不是本机路径时（比如设备上的包），用 `--folder` 指定本机的页面文件夹。

## 协议里的 Weft 域

| 方法 | 作用 |
|---|---|
| `Weft.getCompatibilityReport` | 样式表里引擎不支持的声明（行号、受影响的元素数）、解析时丢弃的内容、运行时丢弃的内容 |
| `Weft.getPage` | 页面最近一次加载用的脚本、选项和 `base`，以及现在的样式表和关键帧 |
| `Weft.setPage` | 用给出的页面顶替并重载；没给的部分沿用现在的。返回 `{loaded, error}`，或者 `{deferred: true}` |
| `Weft.setStyleSheet` | 只换样式表和关键帧，页面状态保留。返回 `{applied, error}`，或者 `{deferred: true}` |
| `Weft.setResources` | 送页面的文件（相对 `base` 的路径加 base64 内容），默认随即重新读取 |
| `Weft.enable` | 返回 View 现在显示的页面（`base`、有没有被顶替、顶替的那一页是不是正在显示），之后每次加载页面发 `Weft.pageChanged` |

`deferred` 表示游戏现在显示的是别的页面，送来的内容已经存下，等宿主加载那一页时生效。

`Runtime.discardConsoleEntries` 让这个连接不再收到页面之前说的话。View 会为晚连上的工具保留最近 1000 条。
| `Weft.getSnapshot` | 现在的文档：元素、属性、文本、状态和每个元素的盒子，以及视口和样式表 |

标准域里和这一阶段有关的：`CSS.setStyleTexts`、`CSS.setStyleSheetText`、`CSS.setRuleSelector`、`CSS.addRule`、`CSS.createStyleSheet`、`CSS.styleSheetChanged`、`Page.reload`。一页只有一张样式表，前端新加的规则追加在它的末尾。

## 现状

### 第二阶段已完成

- **样式热替换**：`View::inspect_restyle` 先用新样式表把场景做出来，成功才换；图片和字体随之重读。
- **在前端里改样式表**：改值、加声明、用复选框停用声明（写成注释，再读回来仍是可勾选的停用项，行内样式也一样）、改选择器、加规则、在 Sources 面板里保存整张表。
- **页面重载**：`Weft.setPage`、`Page.reload`，失败时保留旧页面。
- **资源走开发连接**：`Weft.setResources`。
- **工具**：`tools/devtools_watch.mjs`（监视并推送）、`tools/devtools_compare.mjs`（和 Chrome 对比）。

### 第一阶段已完成

- **通道**：`GET /json/version`、`/json/list`，WebSocket `/devtools/page/<id>`，可以同时连多个客户端。网络读写在单独的线程上，协议处理在 owner 线程上，每次 `weft_view_advance` 之后处理一次。宿主不推进 View 时工具收不到回复。
- **DOM**：DOM 树和实时变化（节点增删、属性、文字）；改属性、删节点；`querySelector`；`$0`。空白文本节点不显示，和 Chrome 一样。
- **样式**：命中的规则按层叠顺序列出，带文件名和行列号，包括 `@layer`、`@media`、嵌套规则和用户代理样式；继承的规则；计算样式；盒模型；行内样式可以编辑（样式表的编辑在第二阶段）。
- **未支持的 CSS**：Styles 面板里标为无效，Console 里给出警告和行号，`Weft.getCompatibilityReport`（`tools/devtools_report.mjs`）列出整页情况和受影响的元素数。
- **Console**：`console.*`、未捕获的异常、表达式求值。工具连上之前的输出也会显示（每个 View 保留最近 1000 条）。
- **高亮和点选**：高亮画在页面之上，脚本、层叠和命中测试都看不到它。点选模式下指针和触摸都交给工具，页面收不到；宿主通过 `weft_view_hit_test` 判断是否转发输入的，也会收到全部输入。

CSS 的来源信息只在开发版里保留。行号指向页面加载的那份样式表，打包过的页面就是打包后的文件；source map 还没有接。

### 尚缺

第一阶段留下的：

- 强制 `:hover` 等状态，`::before`、`::after` 的样式，每段文字实际用的字体。
- 控制台里展开对象：结果只有一层预览，`Runtime.callFunctionOn` 还没实现。
- `CSS.getBackgroundColors`：Styles 面板拿它算颜色对比度，没有时报一次错，其余照常。
- 嵌套规则的选择器不能在前端里改：显示的是展开后的选择器，和文件里写的不同。它的声明可以改。
- source map：行号指向页面加载的那份样式表，打包过的页面就是打包后的文件。

第二阶段留下的：

- 脚本状态的迁移和模块级热替换。
- 监视工具只认 `page.json` 或已加载页面的脚本名，不认识宿主自己描述页面的方式，也没有接打包器。
- 对比只比盒子，不比像素、颜色和计算样式。

### 验证

环境：macOS 26.6，Apple M5，Release 构建。

第一阶段：

- 用 Chrome 自带的前端实测过 Elements、Styles、Console（`tools/devtools_frontend_check.mjs`）。
- 用 Metal 真实渲染确认过高亮的四层盒子和标签。
- `devtools_protocol` 通过真实 socket 测试协议，并把每个元素命中的规则（顺序、声明、值、`!important`、行列号）和盒模型与 Chrome 自身 DevTools 域的结果对比（`tools/browser_devtools_reference.mjs` 录制）。

第二阶段：

- `devtools_protocol` 新增：改声明后盒子变化而脚本状态不变；改完再读规则与编辑返回的一致；停用声明的往返（样式表和行内样式）；保存整张表；改选择器，无效的选择器被拒绝且样式表不变；加规则；严格模式下出错的样式表被拒绝、原表保留；推送文件后图片出现；快照。
- `devtools_reload`（经 C API，和宿主的用法一样）：通道检查点的恢复；工具的页面在新 View 里加载，`sessionStorage` 和页面时间延续；抛错的页面、样式表出错的页面都不加载，旧页面的脚本状态不变，它发出的引擎调用之后仍能完成，Console 里有错误；宿主重新加载同一页时被顶替，加载别的页不受影响；`Page.reload`；只换样式表；工具的文件盖住宿主的读取器；工具断开后宿主的页面恢复。
- 端到端（`tools/devtools_watch_check.mjs`，共 29 项）：一个没有窗口的宿主 `weft_devtools_host`（`tests/devtools_host.c`，只用 C API，可以按指令在两个页面之间切换）加监视工具，直接改文件夹里的文件。
  - 存 CSS 后几毫秒换好样式，脚本状态不变；存 JS 后重载；写坏的 JS 不加载，旧页面保留，报出文件和行号；改好后加载；新加的图片送达，页面不重载。
  - 游戏切到另一页期间存的 CSS 和 JS 只是存下，那一页的样式表和状态不变；切回来后都生效。存下的 JS 写坏了，切回来时加载的是游戏自己的页面，并打印原因。
  - 游戏停在另一页时启动的监视工具先等着，切回来才推送。
- 同样的流程在带窗口的示例程序上也跑过：监视工具推送，窗口关掉再打开后自动重连。
- Chrome 157 自带的前端：在 Styles 面板的数据模型上改值、停用声明、改选择器都成功，面板显示更新后的规则。
- 对比工具：协议测试页 0 处差异；根元素带外边距、内边距、边框和绝对定位子元素的页面 0 处差异（Chrome 里的根元素和 View 里的一致）；专门写的外边距页面列出 3 处，正是不折叠的那几处。
- 监听 `0.0.0.0` 时 IPv4 和 IPv6 都能连上。

Windows（2026-11-11，Windows 11，Visual Studio 2022 的 MSVC 14.44，Release，`WEFT_BUILD_SHARED=ON`）：

- 两版都编过。发布版的 `weft.dll` 不导入 `WS2_32.dll`，开发版导入；两版导出同样的 79 个名字（发布版里三个 `weft_devtools_*` 空函数一样，被链接器的 `/OPT:ICF` 合成一个，名字都在）。
- `devtools_mode`、`devtools_binary` 两版都过。`devtools_protocol`、`devtools_reload` 在 Windows 上也构建（测试这一端的 socket 走 Winsock，`tests/support/devtools_client.hpp`）并通过。`tools/devtools_watch_check.mjs` 29 项全过。
- `devtools_protocol` 起初在 Windows 上失败：签出把 `tests/fixtures/devtools/page.css` 换成了 CRLF，测试按 LF 找文本，Chrome 录下的偏移也是 LF 的。`.gitattributes` 把这个目录定为 LF。
- 在 Unreal 里（weft-unreal，UE 5.8.4，D3D12）：Chrome 157 自带的前端连上游戏里的设置页，Elements 里是页面的 DOM，选中元素后 Styles 列出命中的规则和行号，Console 里有页面抛出的错误；游戏画面上画出高亮的盒子和标签。前端调了两个 Weft 没有的方法：`Runtime.callFunctionOn`（见尚缺）和 `CSS.getBackgroundColors`（Styles 面板算对比度用），各报一次错，面板照常显示。

### 用对比工具发现的差异

对比工具在示例页上找到的引擎和 Chrome 的差异：

- **flex 项目收缩过头**（2026-11-10 已修，见 [status](status.md) 的“flex 项目的自动最小尺寸”）：项目里是固定宽度的块时，Weft 会把项目收缩到比这个块还窄；Chrome 的 `min-width: auto` 不允许。最小复现：300px 宽的 flex 行里放三个内含 140px 块的项目，Weft 各 92px，Chrome 各 156px。修之前示例页 effects、animate、img 在 800×500 的视口里都因此不同；修之后再比，最小复现和 effects 页都是 0 处差异。
- **外边距不折叠**（未修）：相邻块之间、父子之间都不折叠（第一阶段已记录）。
- **`system-ui` 的文字宽度不同**（未修）：touch 示例页的标题 Weft 66.77px，Chrome 62.34px。原因没有查。animate、img、touch 三页剩下的差异（12、3、3 处）来自这一项和上一项。

### 未验证

- Windows：`WEFT_BUILD_SHARED` 默认仍关（打开后编过、测过）。监听 `0.0.0.0` 没有在 Windows 上试（会弹防火墙的询问）。
- Linux 和 Android：链接脚本没有编译过。
- iPad：开发版示例已经构建成功，二进制里有工具代码。但 iPad 处于锁定状态，无线安装一直失败，所以设备上的监听、从 Mac 推送页面、和设备上的页面对比都没有跑过。
