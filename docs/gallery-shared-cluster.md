# Gallery 共享技术簇（2026-10-10）

> 2026-10-11 重构第 8.3a 步：d3_interactive-00 的径向标签原来把 SVG 无单位角度当作弧度。改用统一的 transform 文法后与 Chrome 参考的平均通道差 0.32 → 0.075，偏差超过 64 的像素 38,910 → 1,024。见 [重构记录](refactoring-plan.md#第-8-步记录2026-10-11)。

> 2026-10-11 重构第 8.2 步：cluster 场景的 Grid、reverse/wrapped Flex 与单行混合 inline 改由文档与 scene 共用的 Yoga 适配器计算；210 个采集的像素与几何逐字节不变。见 [重构记录](refactoring-plan.md#第-8-步记录2026-10-11)。

本轮扩大的是 Windows 原生 scene 探针及检查宿主。原始 TestGallery 和固定 React/D3/Framer Motion/React Spring/Lucide/Anime bundles 保持不变；没有将浏览器截图、浏览器计算样式或原始 JS 绘制结果注入 native renderer。

| 新分组 | 原例数量 | 共用能力 |
| --- | ---: | --- |
| D3 Interactive | 4 | SVG CTM/命中、window/document 事件、hover/click、brush、wheel zoom/pan |
| K-Line | 1 | SVG 路径与裁剪、同步几何、文字、有限 HTML tooltip、宿主整数格式化 |
| Framer Motion | 6 | 主鼠标 pointer/hover/tap、rAF/timer、实际 computed style/offset、ResizeObserver |
| React Spring | 3 | 同一 owner 时钟、React scheduler、动态文字与 CSS 数值 |
| Icons | 2 | SVG stroke cap/join、真实 namespace/品牌检测、Framer 动画 |
| Anime.js | 14 | DOM/CSS 属性检测、offset/矩阵、拖拽/吸附/轴约束、SVG morph/dash、Flex/Grid 与 FLIP |

64 个 scene 加两例 Canvas，共 66/72 原例可绘制。WebGL 两例和 Lottie 四例仍为诊断卡片。目录保留全部 72 标签、十组和原始尺寸（2026-10-13 起窗口去掉 webgl-02/03，另加嵌套提示页面、UI 粒子页面与半透明窗口三组，见 [当前状态](status.md)）。真实场景 VM 全部在同一 UI worker 创建、访问和关闭，render 消费不可变完整 owned 快照；每卡最多三份 backing，资源按实际 submission completion 退休。没有并行 DOM 或自动图层提升。

## 可运行入口

```powershell
cmake --build --preset windows-release --parallel 8
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-cluster/interactive
```

4K/FPS/OWNER Hz、侧栏、滚轮和 F5 保持。Scatter 卡片内滚轮缩放，其他区域滚动目录；拖拽可在松开前离开卡片，取消时清理 owner 输入状态。新增 scene 的隐藏 rAF 暂停，timer 继续 pump；恢复不重放每个隐藏帧。这是 inspection host 的明确策略，不等于浏览器隐藏页面节流或通用游戏调度器。

## 明确的范围

可选 `src/script/js/scene_platform_bootstrap.js`（构建时内嵌）使用 authoritative native DOM。`getComputedStyle` 是每次调用的 owned 读回，不是完整 live CSSStyleDeclaration；HTMLCollection 暂为快照。同步 geometry/offset/CTM 读取先计算最新布局，内部 layout/measure 不进入 JS，ResizeObserver 在布局结束后交付。支持主鼠标 PointerEvent 品牌、capture/bubble、handleEvent、once、AbortSignal 和 passive；未开放原生触摸、多点、通用 pointer capture、键盘/IME。

HTML fragment 仅支持 div/span/b/strong/br 与带引号的 id/class/style，64KiB、256 tokens、16 深度，解析失败不改变 DOM；它不是完整 HTML parser。矩阵为六参数 affine。media query 仅固定 primary mouse/hover/reduced-motion profile，未知查询诊断。整数 locale 使用 Windows NLS、en-US 默认 safe integer 格式，其他数字/locale/options 诊断；没有伪造 Intl。新包采用实际 JS 动画 fallback，未实现 WAAPI。

scene 布局支持此次需要的 reverse/wrapped Flex 与 grow/shrink/basis、等宽 1fr Grid（1–8 tracks）、有限单行混合 inline 和 min/max 百分比。Grid 有独立列/行计算；不把 Yoga Flex 当作一般 Grid。inline geometry 暂报告 line box，K-Line 的 b/strong glyph bounds 和通用 inline baseline/换行不宣称通过。`overflow:auto` 限于内容在布局 viewport 内且无 scroll offset 的情况，绘制/命中裁剪一致；需要内部滚动时明确报错。完整 scene scrolling/scrollbar、Grid spans/auto tracks/absolute items 未实现。

SVG 增加 viewBox/preserveAspectRatio、g/text/shapes、平滑 S/T、fill/stroke opacity、evenodd、cap/join/dash、单个 user-space rect clipPath 和真实填充/描边命中。mask/filter、复杂 clipPath 和完整 SVG 文本仍拒绝。新增外阴影由有界 D2D reference 绘制，native shader subset 整体回退；不静默忽略。阴影上限八个/blur64px、32 项/16MiB 缓存；inset 未实现。

新簇的 QuickJS VM 栈上限使用已有配置允许的 2MiB；旧 CSS/D3 profile 保持 1MiB。Debug 的 React/Framer 深层调用在 1MiB 下曾触发 RangeError，库的 Promise 处理将其表现为提前完成。两配置的检查 EXE 实际 PE host stack reserve 均为 8MiB；真实引擎接入必须验证 owner 线程栈余量，这不是公共 runtime 自动调整宿主线程栈的承诺。

## 连续滚动到 Sunburst 的资源修复

此前侧栏跳转检查没有覆盖一路连续滚动的纹理驻留。离屏卡片的 caller lease 和三个 backing 一直保留；原 34 小卡片三版本约 391,440,384 bytes，再加 Sunburst 首份 76,608,000 bytes 为 468,048,384。下一份就超过原 512MiB import 上限，返回 `WEFT_RENDER_BUSY=8`。这不是 SVG JS 异常或显卡 device removal。

检查宿主现在每帧先计算完整可见集，非阻塞回收离屏且 `last_serial <= completed_serial` 的 backing；尚在途的继续保活。回到旧卡片时重新创建完整源图，clip/内容/纹理身份随新版本发布，帧包保持自包含。预算仍为 128 个 lease/512MiB，没有扩大上限。每个释放槽的 painter 工作计数累加保存，避免回收造成统计丢失。native 透明组 scratch 预算也随已完成槽销毁归还；140 次 1MiB 透明组重建反例检查未完成时保活、完成后归零和始终走 native，避免累计预算导致后续卡片无故回退。

新增 resource regression 使用真实原卡片尺寸：旧策略稳定复现 card34 的预算拒绝，完成和失败 producer 清理后导入为零；新策略遍历 64 scene 并重返大卡片、同时更新两个大卡片，共 206 次真实提交/完成，验证未完成版本不能回收、重返像素与最终零驻留。Release hardware 实测峰值四槽/306,432,000 imported bytes。该合成样例只验证资源协议与尺寸，不代替原图渲染验收。

另有完整 HWND/owner 场景：固定三行 wheel profile，50 次 queued WM_MOUSEWHEEL 到 scrollTop=3600 的 Sunburst，再 50 次回到 CSS，120 帧/120 次真实 completion。Sunburst 的当前 clip 与返回 CSS 对独立 Chrome 像素验收，保存两张 4K 截图。Release hardware 已通过，峰值 32 个驻留槽/228,713,472 imported bytes；这些字节只统计导入 linear payload，不是总 VRAM。

```powershell
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --scene-surface-retirement-hardware artifacts/local/gallery-cluster/surface-retirement-hardware
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-scroll-test-hardware artifacts/local/gallery-cluster/scroll-hardware
```

## 独立验收及边界

原样 30 例每例 301 个 60Hz 逻辑帧，24/63/90/120/180/240/300 帧共 210 张 Chrome155、DPR2 基准。参考使用原始 Tailwind CSS；native 使用固定 utility adapter（180 utilities/29 keyframes）。全部输入计划 50 次操作固定，无外部网络。

离线 SHA-256 verifier 覆盖 679 个参考/输入/style hash。native/browser 210 组 bridge state、DOM 顺序与 leaf text 精确对照；HTML div/button 几何字段 4,408 个，通常 .1px，轴约束 .3px，图标循环允许单帧 spring phase 的 3px。inline glyph bounds 没有混入此通过计数。

密集像素采用原有 channel mean≤3 / soft IoU≥.75；旧 34 例/102 图及其门槛未变。稀疏粒子额外用双向一像素 RGB coverage≥.95、颜色能量比≥.9、mean<.1；空白和八像素移位反例必须拒绝。渐隐近空白采样不单独证明功能，每例还要拒绝有内容帧的空白候选。

原始 `anime-11 Draggable: Throw` 在 Chrome 和 native 都没有位移，原因与原例 generic percentage translate 初始化组合有关；不将“双方静止”当作惯性交互通过。原例仍不修改，detail 明示 ORIGINAL ISSUE；独立使用同一个固定 Anime UMD 和有效轴变换，实测 x=200→250（拖动）→271.966（释放初期）→426.471（惯性停止），并验证容器边界及卸载。

GPU 新例验收每张图使用三份实际 backing，共 630 次 submission/completion，覆盖 native shapes/glyphs 和完整 D2D fallback。窗口检查对新卡片用独立 GPU source 读回检查 shell composite；内容正确性由上面的独立 Chrome source oracle负责。旧卡片继续 CPU raster oracle。窗口不是将 GPU 自己与自己比较后宣称 browser 像素支持。

```powershell
ctest --preset windows-release --output-on-failure
ctest --preset windows-debug --output-on-failure
python tools/verify_gallery_cluster.py
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-cluster-gpu-hardware artifacts/local/gallery-cluster/gpu-hardware
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-test-hardware artifacts/local/gallery-cluster/window
```

2026-10-10最终运行计数、交互性能和seal见 [status](status.md)、[交互阶段](gallery-interaction-profile.md) 与 `artifacts/local/interaction-profile/`；之前 `artifacts/local/gallery-cluster/` 仅保留探索产物。早期全量失败及 debugger/修复日志保留，不将探索失败当作通过证据。所有移动阶段（配置到真机）、实际引擎 C ABI、实体输入到显示时延、长时间运行与相同负载 Ultralight 对照均未验证。
