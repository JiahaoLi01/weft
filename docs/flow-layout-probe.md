# Inline、Unicode、滚动与圆角探针

2026-10-08 的 M0 阶段交付。新的 `layout/flow_page`（属 `weft::layout`）组合 block/Flex/absolute 与普通 inline 文本；旧 auto/positioned profile 和旧浏览器参考保持各自的边界。公开入口是实验 C++ 值接口，尚非稳定 SDK、一般 HTML loader 或完整 CSS 引擎。

## 实际输入与排版

`build_flow_page` 接收 `DomSnapshot`、共享 computed style（或作者 CSS）、viewport 和固定 `FontSet`，返回拥有数据的 `FlowPage`。它包含 DOM 元素几何/inline client rects 和无 GPU、Node/JSValue 的绘制值。UI owner 做全量布局；Yoga 只计算盒约束，自己的 inline adapter 计算最终行盒、固有尺寸与文字。渲染之前 reset/GC 后，快照仍能绘制。

接受显式 scope 根 `position:relative` 的 block/Flex，以及 div/p/section/button/i/b/span/ul/li/br、blockified 明确尺寸 img。纯 inline 内容形成测量叶子，block 与 inline 相邻时生成匿名 block 盒，DOM 身份和树不变。零 margin/border、px padding、原有受限百分比/absolute calc 和直接 isolated parent 的 z 顺序继续有效。

一般文本和嵌套 span 的字体大小、行高、颜色、矩形背景、基线对齐及跨行分片已实现；支持 `normal`、`nowrap`、`pre-line`、br、left/center/right。跨文本节点折叠空格，去行首/尾空白，NBSP 不被空格算法吞掉；中文 LF segment-break transform、组合字符、标点约束、ZWSP/WJ 使用同一路径计算 min/max-content、测量和最终行绘制。inline 默认值由 UA profile 保留，i/b 的字重/斜体尚需显式 reset，否则诊断。

字体仍为固定 Roboto Regular → Noto Sans CJK SC，LTR Latin/Han/Common/Inherited。同字号候选整行跨颜色/节点塑形，再按源分片，不在 paint 边界重新独立塑形。如果源边界跨字素或塑形 cluster/连字，当前严格 profile 给出 `WEFT_INLINE_PROFILE`，例如 `"a" + combining mark` 或 `"of" + "fice"`；尚未实现这种共享 glyph 的多样式归属。这也是一般 inline 当前的重要限制。

新行盒读取 OpenType OS/2：USE_TYPO_METRICS 时用 typo，否则用 Win ascender/descender，按逻辑 DPR 1 四舍五入后计算 strut/half-leading。明确 line-height 不被 CJK fallback 的 ink bounds 自动抬高；旧 text/auto 的浮点 metrics 接口没有改动。字体 ink 可以按 CSS 明确行高越过行盒。

## Unicode 依赖与边界

固定未经修改的 libunibreak 6.1，完整 commit、archive hash、61 文件和许可见 [依赖记录](../third_party/libunibreak/README.md)。UAX #14 提供可断/强制断点，UAX #29 过滤字素内部机会；UTF-8 解码与验证复用 HarfBuzz。没有自研 Unicode、JS VM 或字体塑形。

上游数据是 line 15.0 / grapheme 15.1。原始数据测试断行 7,619 通过、35 个明确跳过，字素 1,187 通过、0 跳过；数字 regex tailoring/XX 的跳过来源保留在上游 test_skips.h。CSS `line-break:auto` 另允许折叠 SP 后、ASCII solidus 前的断点（`words / more`），不改上游表；空格断点使用拥有该空格的 inline white-space，非空格跨节点断点使用最近公共 inline ancestor 的 white-space。依据为 [CSS Text 3](https://www.w3.org/TR/css-text-3/)，这些有界规则不表示完整 CSS Text 兼容。

soft hyphen 的条件替代字形、可见 C0 控制字符合成、字典断词/语言配置、bidi/复杂脚本、pre/pre-wrap/break-spaces、hyphens/overflow-wrap/word-break、编辑/caret、emoji 字体绘制尚未实现，会拒绝不支持输入。字素测试中的 emoji ZWJ/旗帜边界不构成彩色 emoji 渲染支持。

## 滚动与同步几何

元素滚动偏移由 native DOM owner 按节点代际保存，DOM snapshot 复制值。`ScriptDom::install_layout_host` 安装只接收快照、返回 identity/几何值的 owner 回调；每次 JS geometry read 调用最新全量布局并验证 epoch/generation、重复身份、有限坐标和一致 scroll extent。回调期间不允许调用此 VM 的 JS/DOM，`WEFT_LAYOUT_REENTRY` 有反例。未安装 host 时明确 `WEFT_GEOMETRY_HOST`；脱离文档或 host scope 外目标返回零几何。

已导出 `getBoundingClientRect()`、`getClientRects()`、clientWidth/Height、scrollWidth/Height、scrollTop/Left。rect 是拥有值的普通对象，client rects 是快照 Array；尚无 DOMRect/DOMRectList 的完整原型、item/toJSON。scroll setters 先做 JS 数值转换、再取得新几何并 clamp，避免 valueOf 重入导致失效 iterator。hidden 允许程序滚动，auto/scroll 另允许 wheel 默认行为。内容减少同步夹紧；祖先滚动平移后代绘制和 client rects；嵌套裁剪约束 scroll extent，计算包含末尾 padding。

`dispatch_wheel(weak identity, deltaX, deltaY)` 接受像素 delta，派发 bubbling/cancelable trusted WheelEvent；preventDefault 阻止默认滚动，listener/微任务更新后重新验证身份与布局。按轴选择能移动的最近 auto/scroll 祖先，到边缘时向上链；同一 delta 在一个祖先部分消耗后，本 profile 不继续分发余量。hidden 不接受 wheel 默认滚动。

scroll 事件当前通过 QuickJS job 在 owner 微任务检查点合并派发（不 bubble、不 cancel），不是完整 HTML rendering-task 调度时序。旧三参数 wheel API 的坐标/modifier/time 默认零；后续阶段已增加完整值事件、flow 分片/圆角命中和 HWND 消息输入，见 [输入窗口合同](flow-input-window-probe.md)。实体滚轮、触摸惯性、占位 scrollbar 和可见 overlay scrollbar 仍未验收。布局采用无占位滚动条合同，浏览器参考隐藏 scrollbar 以比较该合同。`visible/clip` 与另一个可滚动轴的 CSS computed 转換生效；仍为 clip 的组合严格拒绝。没有 RTL/负向滚动、overscroll、scrollIntoView 或平滑滚动。

## 圆角与图形接口

共享 style 支持 border-radius 1–4 值及 `/` 椭圆轴、四个 corner longhand 的 px/% 和 var/层叠；IACVT 不回退到较早声明。长度解析后按 CSS 相邻半径总和共同缩小重叠半径。background 和明确尺寸图片用自己的圆角，overflow hidden/auto/scroll 才把圆角加入后代 clip chain，多个祖先取交集。

绘制快照只携带 `RoundedClip` 值。backend 准备阶段将边缘覆盖合并进灰度字形或 PNG 资产，保留透明度、线性 premultiplied 采样和 draw 顺序；全包含凸区域直接走原 quad。独立 mask 16×16 area sampling，尺寸不超过 4096、单 mask 不超过 8 MiB、深度不超过 16、附加 mask 累计 32 MiB；reference renderer 另限制整包 32 MiB/512 assets。源素材和 mask 的 CPU 临时存储另计，未做生产缓存或性能预算。

实验 render backend C ABI 仍为 v1，未给无 struct_size 的 draw 增加字段；圆角准备不依赖 DX11 类型，后续 Vulkan/Metal 可消费同样有序 coverage/image 内容。实际 GPU 目前仅 Windows D3D11。rounded SDF 字形准备、inline 圆角/非零 padding/border、transform、opacity group、inline-block/replaced、inline 内 block、float/grid/table/margin collapse 均未实现或明确诊断。

## 验证与复现

同一份 [fixture.js](../tests/fixtures/flow-layout/fixture.js) 在原生 DOM 与实际 Chromium 运行：1 个 showcase 加 8 个确定种子随机场景，3 轮 width/font/text/append/remove/radius/scroll 变更，共 27 场景。固定浏览器 JSON 有源码/字体 hash。比较 1,860 个 geometry 字段和 952 个 client fragment 字段，最大分别 1 px、0.850128 px；元素/分片数量必须完全相同。新 profile 的 1 px 容差包含逻辑 1× Win 字体度量和实际浏览器 DPR 1.005 量化，旧 goldens 的更小容差没有放宽。

原样固定 AlertBar/React/Zustand/store/双向 UE bundle 未修改，只用作者 flow CSS adapter 和外部滚动演示按钮；固定 57 文件样本不从仍在修改的原游戏自动刷新。8 帧浏览器状态与原生一致：1,336 geometry 字段最大 0.299126 px、2,672 色分量最大 0.0000003922；8 个 scroll offset 最大差异 0.348259 px（容差 0.5，浏览器 DPR 量化）。原游戏和真实 UE 服务未执行。

测试另覆盖 canceled wheel、祖先滚动、detached 节点仍有效/零几何、旧 epoch、错误线程、callback 重入/stale identity、scroll valueOf 数值转换、16 轮 VM reset/GC 无 native root 泄漏、非法 UTF-8、nowrap/NBSP、br/Unicode 强制换行、跨源 grapheme/ligature 拒绝及无效 radius/不支持布局。

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-debug --parallel 10
ctest --preset windows-debug --output-on-failure
cmake --build --preset windows-release --parallel 10
ctest --preset windows-release --output-on-failure
.\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --flow-hardware artifacts/local/flow-debug/hardware
.\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --cheetah-flow-hardware artifacts/local/cheetah-flow-debug/hardware
ctest --test-dir build/windows-gc-stress -C Debug -R 'flow|unicode|script_dom_lifecycle|page_react' --output-on-failure
```

WARP 与 browser diff 已注册 CTest；hardware 必须另执行，Release 同理。强制 GC 构建方法见 [building](building.md)，普通 script 预算保持 2 秒；真实 React instrumentation 继续使用原有 30 秒预算。实际全量次数、日志、源/二进制/产物 hash 见 [status](status.md) 和 `artifacts/local/flow-{debug,release,gc-stress}/run-metadata.json`。

DX11 flow 四帧包含滚动、嵌套椭圆、裁剪字形、原 PNG 及透明叠加，独立 CPU readback 对照 119,840 样本；另以 64×64 独立椭圆积分比较 8,000 texel，最大 5/255。436 textures 都等实际 host query 完成退休。原样 React 八帧另有 858 textures、568 CJK fallback glyph，完成/调试层均验证。GPU vs CPU 参考通过不表示浏览器逐像素相等；浏览器截图和原生 PNG 单独保存。

```powershell
python tools/build_cheetah_flow.py
python -m http.server 4176 --bind 127.0.0.1 --directory F:/Projects/CaseElephant
```

打开 `/tests/fixtures/flow-layout/`，点击“更新布局”观察换行和滚动夹紧；打开 `/examples/cheetah-flow/`，切换 13 项、展开并点“滚动展开列表”，接收回执验证保留偏移。browser 页面只用于参考；`--flow-warp` / `--cheetah-flow-warp` 输出的是自己的 DOM/layout/font/DX11 像素。

Android/iOS 配置、编译、链接、模拟器/真机功能与性能均未执行。没有 CPU/GPU/内存尾部指标，不把 Debug/Release 的验收时长当性能结论。flow 命中与 Windows wheel 消息已在后续 [输入窗口阶段](flow-input-window-probe.md) 接通；实体输入/触摸及移动仍待验收，下一步扩大真实档案 UI 与 D3 SVG 绘制。
