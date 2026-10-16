# 前端库与 D3 / React 验收合同

更新日期为 2026 年 10 月 7 日。用户已要求支持 D3 等第三方前端库；这改变了早期的能力优先级。SVG 图形绘制属于必须完成的产品能力，不能以运行比例尺函数或生成 DOM 树代替可用图表。此文件细化验收层级，实际平台证据以 [status](status.md) 为准。

## 支持的含义

同一份固定版本、未经修改的库代码，在 Weft 自己的 JavaScript VM、原生 DOM、样式/布局、绘制和输入系统中工作。正常运行不委托 WebView，不使用 jsdom，不把测试专用 JavaScript DOM 模拟器带入运行时。库可以沿用其标准 Web API 调用方式；我们负责补齐依赖接口与语义。

兼容性按真实 workload 声明，不承诺任意浏览器库和全部插件。对 D3 的工作拆分是验收进度，不能据此把用户要求缩减成纯计算库支持。

| 层级 | 必须证明的行为 | 本阶段证据 |
| --- | --- | --- |
| L0 脚本与计算 | 未修改 D3 bundle 能加载；比例尺、数据和形状生成函数正确运行 | Windows x64 Debug 的 QuickJS-NG 原生探针与 Chrome 参考均通过固定 fixture |
| L1 原生 DOM 集成 | HTML/SVG namespace、selector、keyed join、节点身份、排序、移除、属性、文字、事件，以及 axis/line 创建的图形树 | Windows x64 Debug 原生 DOM 29/29；所有报告字段与 Chrome 参考相同 |
| L2 图形与交互 | SVG 图表经过 native SVG/style/paint → FramePacket → GPU；图形命中、变换、裁剪、文字正确；动态更新后画面一致 | 尚未实现或验证，v0.1 必需 |
| L3 调度与动画 | D3 transition 的宿主时间、计时器、rAF、插值、取消、重新绑定、暂停恢复 | 尚未验证，后续扩展 workload |
| L4 专项库行为 | D3 drag/zoom/brush 的坐标转换、指针/触摸、焦点及可访问性；更宽 SVG/Canvas、资源和模块 API | 按专项 fixture 增加并列出已验证范围 |

L1 通过可以声明“D3 7.9.0 固定 DOM 探针通过”。只有 L2 在目标平台完成后，才可以声明这一份 D3 图表应用可用。完成 L2 不代表 transition、drag、zoom、全部 d3-shape 曲线或所有 SVG 功能已经支持。Preact 的真实应用另建固定 bundle、接口清单和验收，不能从 D3 的结果推断其他框架兼容。

## 固定依赖与来源

`third_party/d3/d3.js` 直接来自 npm 官方发布的 `d3@7.9.0` 包 `package/dist/d3.js`，587,043 字节，未经裁剪、替换或兼容性补丁。UMD 通过 source script 加载到 View；它是探针与样例依赖，不作为 C++ 核心强制内嵌代码。正常构建无需联网。[固定发布包](https://registry.npmjs.org/d3/-/d3-7.9.0.tgz)、[发布元数据](https://registry.npmjs.org/d3/7.9.0)

| 项目 | 固定值 |
| --- | --- |
| 发布版本 | 7.9.0 |
| 发布元数据的 gitHead | `1f8dd3b92960f58726006532c11e9457864513ec` |
| 发布 tarball SHA256 | `7e36605710a2ba54846797c8c6d888911341b215ae53257dc32a49a0b824355e` |
| 原始 UMD SHA256 | `0c0d58be2106c14e242fc75bc09a17467800630751580665f6e9552a0c064ea2` |
| 修改与构建 | 无；直接使用上游发布字节 |
| 主要许可 | ISC；原始 LICENSE 与 bundle copyright 保留 |

`metadata.json` 记录来源、归档/文件 hash、registry integrity、构建方式与许可。`upstream-package.json` 和官方固定 commit 的 `upstream-yarn.lock` 保留来源证据。依据发布 lock 收集 37 个传递 runtime 模块的许可；每个下载包都同时核验 npm metadata 与发布 lock 的 SHA512 integrity。`bundled-licenses.json` 记录各模块版本、URL、SHA256 与 license 原始文件 SHA256，`BUNDLED-LICENSE-NOTICES.txt` 保留原许可文本；这份保守归因集不包含开发依赖。[固定源码](https://github.com/d3/d3/tree/1f8dd3b92960f58726006532c11e9457864513ec)、[原始许可](https://raw.githubusercontent.com/d3/d3/1f8dd3b92960f58726006532c11e9457864513ec/LICENSE)

升级 D3 时同时换固定 bundle/许可/hash，先让浏览器和 native 重跑同一脚本；不允许改 D3 源码来绕过缺失 DOM 行为。

### d3-sankey（2026-10-16）

`third_party/d3-sankey/d3-sankey.js` 来自 npm 官方发布的 `d3-sankey@0.12.3` 包 `package/dist/d3-sankey.js`（11,993 字节，未改）。它是 UMD，在 `d3.js` 之后加载，挂到同一个 `d3` 全局上，调用 d3 的 `min`、`max`、`sum` 与 `linkHorizontal`（它的包声明 d3-array 1–2、d3-shape 1；d3 7.9.0 内含 d3-array 3、d3-shape 3，这几个函数的行为相同）。用户（游戏的经济面板要画资源流向的桑基图）提出。

| 项目 | 固定值 |
| --- | --- |
| 发布版本 | 0.12.3 |
| 发布元数据的 gitHead | `bb7233ea0004669878d922e55fe83084ef3cb555` |
| 发布 tarball SHA256 | `6bd8cdb143d4088ff38179def70954419868d4b95f136c168ffef2dad004ff22`（npm 的 SHA512 integrity 与 shasum 均已核对） |
| 原始 UMD SHA256 | `cd92c492d3696b990e0de84e0aacb7fd5176c6c8d0ce2c52822f730993b7bd81` |
| 修改与构建 | 无；直接使用上游发布字节 |
| 许可 | BSD-3-Clause；原始 LICENSE 保留 |

验收（L0–L2，这一份固定 workload）：
- 共享脚本 [d3_sankey_probe.js](../tests/fixtures/d3_sankey_probe.js)：12 个节点、12 条连线的一个月资源流向，`d3.sankey()`（`nodeId`、`sankeyLeft`、`nodeWidth`、`nodePadding`、`extent`）布局，画成连线（`sankeyLinkHorizontal` 的 path，按值宽的半透明描边）、节点（rect）与标签（svg text，`dy`、`text-anchor`）；7 项检查。
- [tools/browser_d3_sankey_reference.mjs](../tools/browser_d3_sankey_reference.mjs) 在 headless Chrome 里跑同样三个脚本，记下报告（每个节点的 x0/x1/y0/y1/value/depth、每条连线的宽与两端、svg 节点树）与三个脚本的 SHA256：[d3-sankey-reference.json](../tests/fixtures/d3-sankey-reference.json)。
- `d3_sankey_browser_contract`（`weft_d3_sankey_tests`）：weft 的 QuickJS 与原生 DOM 跑同样三个脚本，报告与 Chrome 的逐值相同（数字按 double 相等，字符串逐字节：每条 path 的 `d` 与 `stroke-width`）；脚本的散列由构建给出，与参考记录的不同时失败。
- 绘制：同一棵 svg 是 `scene_paint_reference` 的 `sankey` 用例（2×），原生 D3D11 WARP 对 Chrome 155 的截图均差 0.98 级、差 32 级以上 1.6%、5 × 5 平均后最大差 32 级（都在小字的字形与曲线边缘的抗锯齿），D2D 参考光栅相近，Vulkan 与 D3D11 至多差 2 级。

## 共享 fixture 与反例

[d3_probe.js](../tests/fixtures/d3_probe.js) 是浏览器和 native QuickJS 共用的完整测试脚本。依次加载 `d3.js` 和该脚本即可执行；脚本完成值是 `JSON.stringify(report)`，另留下 `globalThis.caseElephantD3Report`。任何断言失败都会抛带检查名的 Error。C++ runner 需要把失败视为测试失败，不能只检查 bundle 加载无异常。

它通过真实 D3 方法完成以下行为：

- `select`、`create("svg")`、`append` 在 HTML 与 SVG 中创建节点，并验证命名空间和查询后的 wrapper identity。
- `scaleBand`、`scaleLinear` 的输出真正写入 SVG 几何属性，并验证未知 domain 与逆变换。
- 以 key 从 `a,b,c` 更新到 `c,d,a`。新增 d 必须插入旧 a 前，旧 c 必须移动；a/c 保持对象身份，b 脱离树但仍可读取数据、写属性。HTML legend 同时做相同更新。
- D3 自己的 `__data__` 和应用自己的对象 expando 跨查询保留；节点身份不能仅靠内容一致蒙混通过。
- `axisBottom` 创建 SVG tick、domain path，再做 keyed 更新；验证 tick 身份、顺序、exit、文字及 path。`d3.line` 生成实际 `d` 属性。
- SVG `viewBox` 大小写、SVG 后代创建、xlink namespaced 属性的设置/读取/移除，以及 HTML/SVG `textContent`。
- D3 `.on` 和 `.dispatch` 的 listener 替换、移除、datum、detail、`this`、target/currentTarget、冒泡与 preventDefault。只运行回调而缺少事件身份和传播的实现无法通过。

报告中 `svg_order`、`html_order`、`axis_order` 必须都是 `["c","d","a"]`；`svg_heights` 是 `[120,180,80]`，`event_calls` 是 `[1,1,3]`，`line_path` 是 `M75,80L200,20L325,120`。29 项检查是明确结果，不能当作完整 DOM 测试计数。script 动态返回 `native_rendering_verified:false`，提醒任何 runner 这份脚本没有检验像素输出。

后续 native/JS 生命周期测试还要针对 `node.__data__`、`node.__axis`、`node.__on`、应用 expando、listener closure 与 detached subtree 的互相引用，验证可达对象存活和不可达循环回收。包装对象身份必须贯穿 GC，不能用每次查询新建对象的方法临时通过树结构测试。

### 当前浏览器参考

2026-10-07，在 Windows 上使用 Chrome **154.0.8037.98** 的 headless 模式打开 [d3_chart_reference.html](../tests/fixtures/d3_chart_reference.html)，同脚本的 **29/29** 断言通过。此结果验证 fixture 及独立浏览器预期；浏览器负责画出的图不能算本项目的原生渲染证据。

运行命令如下；profile 放在临时目录，避免触及用户浏览器配置：

```powershell
& 'C:\Program Files\Google\Chrome\Application\chrome.exe' `
  --headless=new --disable-gpu --no-first-run --no-default-browser-check `
  '--user-data-dir=C:\Users\JiahaoLi\AppData\Local\Temp\caseelephant-d3-reference-20261007\profile' `
  --dump-dom 'file:///F:/Projects/CaseElephant/tests/fixtures/d3_chart_reference.html'
```

此次保存的参考报告为 `artifacts/local/d3-reference/report.json`，对应的 `reference-dom.html` 和 `browser-stderr.txt` 一并保留。浏览器 profile 仍在上述临时目录；运行结果已复制到项目产物目录，生成产物不纳入源代码。参考页面也可直接在浏览器打开，展示更新后的柱状图、折线和轴，并显示逐项报告。这个页面的外围样式只是浏览器检查页面的呈现，不暗示 native CSS 已实现。

### 当前原生验证

同日 Windows x64 Debug、Release 和 QuickJS 每次分配前强制 GC 的 Debug 构建，`d3_native_dom` 测试均 **29/29** 通过。`artifacts/local/d3-native-debug/d3-native-report.json` 保存共享脚本报告，`d3-native-dom.json` 保存原生节点树；`browser-comparison.json` 记录与 Chrome 报告的直接 JSON 对照。Release 和强制 GC 的报告保存在相邻的 `d3-native-release`、`d3-native-gc-stress` 目录。三种配置的对照均包括 fixture/version、全部检查名称与计数、SVG/HTML/axis 顺序、柱高、文字、path 字符串、事件调用计数和渲染验证标记，**12 个字段完全相同**。构建/CTest 的完整命令由 [status](status.md) 统一记录。

这次验证仅达到这一份固定 workload 的 L0/L1：HTML、SVG 都由 C++ native 节点与 QuickJS wrapper 提供。原生输出中包含真实 D3 创建的 rect、g、line、path、circle、text 和 HTML legend；存储的几何和视觉属性仍未经过 native SVG/style/paint/GPU 解释。Windows GPU、Android、iOS、transition 和真实指针命中保持未验证。

## 原生 SVG 必须完成的架构工作

SVG 与 HTML 共享 DOM、JS wrapper、事件和资源身份。外层 SVG viewport 接入 HTML/CSS 的尺寸与布局；SVG 内部的图形节点使用 SVG 用户坐标系统，不能把 `g`、`rect`、`path` 当作普通 Flex 子项。native 查询与 mutation 是同一份 DOM 的行为，SVG 派生绘制结构可以重建，但不能成为第二份节点身份来源。

| 路径 | L2 验收要求 |
| --- | --- |
| viewport | `svg` width/height、`viewBox`、`preserveAspectRatio`、裁剪与缩放；独立 viewport/resize fixture |
| 基础形状 | `g`、`rect`、`circle`、`line`、`path`、`text`；fill/stroke、透明度、绘制顺序；D3 常见图表的 path 命令及曲线逐项验证 |
| 路径与线条 | 填充 winding、闭合、stroke 宽度、caps/joins 与 miter limit；解析/展平/填充/描边分别测试，不以包围盒替代图形 |
| 变换 | SVG transform list、组继承、viewBox → viewport → HTML 位置 → surface 的合成；SVG attribute 与 CSS transform 的规则分别实现 |
| 样式与文字 | presentation attributes 与 author/inline CSS 的层叠、继承、currentColor；`text-anchor`、基线与必要 `dy` 单位和宿主字体测量/塑形 |
| 裁剪引用 | `defs`/`clipPath` 与 `clip-path` 引用，矩形及路径裁剪，引用/ID/几何变化的失效；引用不可形成悬空原生指针 |
| 动态更新 | D3 attr/text/data/exit 的 mutation 必须进入 style/geometry/text/paint 失效；尺寸、变换、clip 引用变化与全量结果差分 |
| 命中与事件 | 以支持的 SVG 图形/裁剪和逆变换命中，遵循已声明 pointer-events；事件目标来自当前 DOM，渲染线程不访问 Node 或 JSValue |
| 渲染提交 | 将不可变几何、文字、变换和 clip 组织为 FramePacket；CPU/GPU 资源按宿主 submission serial 退休，Windows 与移动后端消费同一 IR |

DOM 接受 `path` 的 `d` 或 `clip-path` 属性只证明属性可存储。图形解析、样式、绘制、命中、动态更新和平台检查都完成后才能登记为支持。L2 首个图表验收至少覆盖柱状图/坐标轴、折线或面积图、扇区图、文字和裁剪缩放，以及修改数据和 viewport 后的图形。曲线和圆弧是独立风险项，必须保留渲染反例和误差阈值。

后续 D3 transition 不可导出静默无效的 timer/rAF stub。owner 提供确定性时间推进、真实任务/微任务合同与宿主驱动的 requestAnimationFrame；SVG transform 插值依赖的标准 geometry API 要真实实现并验收。D3 pointer/drag/zoom 需要 `getScreenCTM`、逆矩阵/点变换、SVG 与 HTML 坐标差异及实际触摸事件，不能从本次人工 dispatch 推断它们可用。

本文件只规定新增必需能力和验收方式；实际实现状态与平台验证仍须逐项记录。当前脚本未使用 CSSStyleDeclaration、classList、计时器、fetch、Canvas、getBoundingClientRect、SVG geometry、transition、drag、zoom 或 brush；这些接口保持独立的待实现/待验证项。

## React / Vite 实际页面

固定 React 19.3.0/react-dom 19.3.0/scheduler 0.28.0 的原版 npm 字节由 Vite 8.3.3 构建，同一 JSX 入口在实际浏览器和自己的 native DOM/layout/字体/GPU 运行。25 个元素的初始、useState 计数、keyed reorder 三帧原生像素通过，300 个几何字段与独立 browser 参考比较，keyed 节点身份保留、unmount/reset 后绘制快照仍有效。

本次只声明这个受限页面可渲染。CSSOM 为 inline declaration 反映，scheduler 使用宿主 timer fallback；没有 MessageChannel/rAF、普通 HTML/CSS 文档加载、完整焦点/输入或移动证据。D3 仍只达到前述 L0/L1，没有 SVG 像素；Preact 尚未实测。完整来源、profile、构建和 GPU 证据见 [React 页面探针](react-vite-probe.md)。

React 页面现另接入 [Windows 主鼠标窗口](native-window-probe.md)。它用 layout hit 和实际 native MouseEvent 更新原版 React，再经 persistent D3D11 device/flip swapchain 绘制；原有直接 dispatch runner 保留。自动 HWND message 证据不等于实体鼠标或完整 DOM 输入验收。

## 官方行为参考

- [D3 selection](https://d3js.org/d3-selection/selecting)：document-order 查询与后代选择。
- [D3 data join](https://d3js.org/d3-selection/joining)：key、enter/update/exit 与节点上的 datum。
- [D3 namespaces](https://d3js.org/d3-selection/namespaces)：HTML/SVG 元素和 namespaced attributes。
- [D3 events](https://d3js.org/d3-selection/events)：listener 和 dispatch 的 Web API 依赖。
- [SVG 2](https://www.w3.org/TR/SVG2/) 与 [DOM](https://dom.spec.whatwg.org/)：原生语义依据；D3 源码不能替代规范。
