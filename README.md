# Weft 游戏 UI 运行时

Weft 的目标是在 Windows、Android 和 iOS 上，用同一套 HTML、CSS 和 DOM 行为构建游戏 UI，并让宿主引擎控制资源、线程和 GPU 执行。采用自研运行时，复用 JavaScript、字体和 Unicode 基础库。

实现语言为 C++20，当前处于 M0：已有可构建核心、实验 C ABI、原生 DOM/QuickJS、Flex/字体和 GPU 探针。真实 React/Vite JSX 应用已贯通 native DOM → 行内样式 → Yoga → 字体 → D3D11，计数与 keyed 重排输出三帧原生像素，并接入 Windows HWND/swapchain 主鼠标窗口。原版 D3 的 29 项 DOM 检查、33 个 Flex 场景的 452 项浏览器对照继续通过。一般 HTML parser、完整 CSS 格式上下文、通用 SVG、键盘/IME 与移动后端尚未实现；当前不是完整运行时。设计预算仍是验收目标，实际探针测量另列运行条件。

当前 Gallery 已加入 D3 Interactive 4、K-Line 1、Framer Motion 6、React Spring 3、Icons 2、Anime.js 14，共新增 30 个原样案例，连同此前 36 例为 **66/72 可绘制**。新增共享 SVG/事件/几何、布局和 JS 动画能力；原始组件与库包不改。4K/FPS 预览：`weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-cluster/interactive`，详细运行、独立浏览器对照和限制见 [共享技术簇](docs/gallery-shared-cluster.md)。原始 Anime Throw 在浏览器也静止，惯性另有同库反例验证。2026-10-13 起画廊窗口去掉两例 Three.js（WebGL 不是产品目标），换成 CRPG UI 的嵌套提示页面（烟雾为 UI 着色器效果，悬停、嵌套、钉住与拖动和 Chrome 逐帧对照），又新增一组 UI 粒子页面（篝火、宝箱飞金币、符文光环与可调密度的漂浮微粒，4K 下 24 万个粒子 GPU 约 0.2 ms），现为 **68/72 可绘制**，剩余四例 Lottie。

新增 [通用交互优化与可切换 profiler](docs/incremental-ui-profiler.md)：Gallery 侧栏/F8 开启曲线、分项 timer 与 P50/P95/P99/Max，默认关闭、全速运行。修正正常松开鼠标的误 Cancel，精确 style memo 与 owner 增量值/几何复用，诊断 footer 移至 native overlay；原始组件/库不改。当前预览：`weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/incremental-ui/interactive`。Esc 后 `python tools/analyze_gallery_trace.py <输出目录>` 合并有界双线程 trace。Release/Debug 各87/87，K-Line/多案例 Anime 仍需按游戏 UI 毫秒预算继续优化。

代码规范化（2026-10-10）：全部 C/C++ 与 HLSL 按 [代码规范](docs/coding-style.md) 重排并命名常量，Release 反汇编逐函数对照证明行为不变；后续架构重构按 [重构计划](docs/refactoring-plan.md) 分步进行。 第 1–5 步已完成：`src/base` 提供 `Diag`/`Result<T>`、共享 CSS 标量解析、代际槽表与 `OwnerThread`；测试共用 `tests/support`；平台 JS 内嵌进脚本库；源码按 `src/<模块>` 拆成 `weft::<模块>` 静态库，公开 C ABI 位于 `include/weft/`；D3D11 参考渲染器移入 `weft::render_d3d11`，后端经 `RenderBackend` concept 生成 C 函数表，shader 构建期编译内嵌。WARP 与硬件产物与重构前逐字节一致，见 [status](docs/status.md)。

此前阶段 [交互/动画优化](docs/gallery-interaction-profile.md)：K-Line单例owner P50约49→14ms；Spring Trail静态emoji图像不再逐帧重建，paint约11.59→0.094ms。这些为此前单轮/中途测量，当前对照见上方新阶段，Present FPS不能代替Owner/age/输入延迟。

单通道 SDF 已接入 FreeType 字体适配器和 D3D11 独立探针：同一 atlas 字形的多倍率缩放、描边、导数抗锯齿和资源退休通过。scene 文字（含中文、TTC 与可变字体）的 field 已在 CPU 上按 scene 布局重建验证。小字号保留灰度路径；SDF 页面材质、移动 GPU 与 MSDF 待验证，见 [SDF 证据与合同](docs/sdf-font-probe.md)。

Windows 构建：`cmake --preset windows-msvc` → `cmake --build --preset windows-debug --parallel 4` → `ctest --preset windows-debug`。依赖源码已随项目固定，正常构建不联网；完整步骤见 [构建与探针](docs/building.md)。

图形 API 已通过实验 C ABI 接口注册：通用层管理能力、句柄与提交/完成协议，D3D11 reference 和 React 窗口已接入。2026-10-11 起渲染器只有显示列表一条绘制路径：运行时的帧以不透明显示列表穿过 C ABI，宿主的四边形帧在适配器里转成显示列表。Vulkan、原生 Metal 可各自扩展 native pass，不要求经同一图形 API 翻译。2026-10-12 起有 Vulkan 渲染器与它的 C ABI 适配器（与 D3D11 共用规划和着色器源码；宿主录制、提交并报告完成，两帧在途；在本机 NVIDIA 与 AMD GPU 上验证；Windows 上可在 Vulkan 交换链窗口里运行 4K CaseCheetah 页面，双击 `tools/cheetah-flow.bat` 打开）；Android arm64 已能交叉编译与链接（NDK r30，未在设备上运行，见 [构建](docs/building.md)）；具体移动后端尚未实现，见 [后端 API](docs/render-backend-api.md)。

后续以用户持续开发的 CaseCheetah UI 为真实 workload，继续 D3D11。已只读捕获固定样本，未修改原游戏；原样 NineSlice.tsx、五条原始 CSS 和三张 PNG 已在自己的 DOM/CSS/DX11 输出三帧，528 项浏览器几何对照通过。新 positioned profile 的边界、运行方法与完整 UI 的后续缺口见 [CaseCheetah 样本](docs/casecheetah-workload.md)。

已增加固定 Roboto → Noto CJK 中文回退，原样 AlertBar/Zustand/store/双向 UE 数据桥在预览 CSS 下输出八帧 DX11 像素，1,336 项浏览器几何和状态对照通过。桥只捕获本地数据；完整原游戏 CSS、完整 UI 和移动仍待验收，见 [中文与状态组件](docs/cjk-alert-probe.md)。

新增公共 `weft::style`：positioned 和 Flex 页面共享作者层叠、属性选择器、主题变量继承/回退/循环、computed-value 失效及 rgba。原始 operations.css 的主题块和展开规则已在八帧 AlertBar 中运行，2,672 个浏览器颜色分量与 1,336 个几何字段通过；明确尺寸适配仍保留。最终 Debug/Release 37/37，新增范围和 React 强制 GC 7/7，见 [公共样式实际合同](docs/style-cascade-probe.md)。

新增自动布局接口：受限 block/Flex/absolute、内容高度、中文固有宽度、1–4 值 padding 和 ASCII 多行。原样 AlertBar 已解除内部固定尺寸，48 个 mutation 场景及八帧 DX11 通过浏览器几何对照。最终 Debug/Release 各 42/42，自动布局与 React GC 6/6。范围与复现见 [自动布局合同](docs/automatic-layout-probe.md)。

新增 flow profile：嵌套 inline/span 与匿名行盒、Unicode 断行、同步 DOM 几何与程序/wheel 滚动、椭圆圆角及嵌套裁剪。固定 libunibreak，27 个 mutation 场景和原样 React 长列表的八帧浏览器对照通过；同样内容已由自己的 DX11 输出像素。当前仍有字重/bidi、跨源 ligature、inline-block、实体滚轮/触摸、完整 scrollbar 和移动缺口，实际范围和可运行场景见 [flow 合同](docs/flow-layout-probe.md)。

2026-10-11：上述 page、positioned、auto profile 已并入 flow，成为唯一的文档格式化器 `layout_document`；各自原有的浏览器对照都在统一路径上按原阈值通过，见 [重构计划](docs/refactoring-plan.md)。同日第 8.2 步起，文档与 scene 共用一个 Yoga 适配器（`layout_boxes`）。

flow 命中与 HWND 滚轮已接入：命中复用真实绘制顺序、inline 分片和圆角链；原样 React 长列表在自己的消息队列中完成滚动、选中及窗口缩放。3,150 个浏览器命中点中 3,130 个完全一致，20 个边界点显式限定在每轴 2 CSS px；隐藏窗口和细分/取消滚轮已自动验证，实体设备与触摸仍未验收。可见样例、运行命令和证据见 [flow 输入与窗口](docs/flow-input-window-probe.md)。

可见 flow 窗口现默认最大化，使用 3840×2160 离屏目标与按设备字号重新生成的中英字形；右上角和标题栏显示每秒更新的真实 Present FPS。逻辑布局与两级 letterbox 输入映射保持一致。4K 原样 React 场景、idle 合成和实际 query 退休已有自动验证，见 [4K/FPS 范围与运行](docs/flow-input-window-probe.md#4k-与-fps2026-10-08)。

4K 滚动已加入有界字体塑形、字形/圆角像素和 D3D11 immutable texture 缓存；DOM 未变化时复用完整几何，滚动变化仍重算几何。缓存阶段的固定 120 帧滚动采样中位耗时从 155.257 ms 降到 15.384 ms，48 帧随机缓存/全量像素对照一致，见 [滚动优化与测量条件](docs/flow-input-window-probe.md#4k-滚动优化2026-10-09)。

4K 窗口现默认最多两帧在途：独立 constant buffer/query、按实际完成退休 UI/HUD 资源，只在队列满、截图、缩放和退出时等待。RTX 5090 / MSVC Release、同源固定 120 帧滚动的全完成吞吐从同步 64.585 提高到 170.980 帧/秒；隐藏脚本场景不是实体窗口 FPS 保证。Debug/Release 全量各 57/57、强制 GC 范围 11/11，随机两帧/同步像素对照最大差异 0；同步路径保留，renderer C ABI v1 不变，见 [两帧流水线](docs/flow-input-window-probe.md#两帧流水线2026-10-09)。

可见 4K flow 样例现默认带连续运动区：金色滑块每个 owner tick 更新，绿色滑块刻意按 30 Hz 采样；Pause/Resume 与 Speed 0.5x/1x/2x 可直接比较观感。JS 按高精度宿主时间改变 DOM `left`，经过原生样式、布局和 D3D11；最小化/遮挡恢复不追赶隐藏时间。它是独立演示，不宣称 CSS animation/transition 或通用 rAF 已实现，静态 golden 和原始 React bundle 不变。验证与运行见 [连续运动](docs/flow-input-window-probe.md#连续运动展示2026-10-09)。

此前 CSS/D3 阶段（历史记录）的 [4K Gallery 检查窗口](docs/gallery-preview.md)：`weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/render-stages/interactive`。保留全部 72 个案例/10 组，默认 CSS，侧栏跳转与滚轮浏览。**26 个原样 CSS + 8 个原样 D3 已在 native DOM/布局/Windows GPU 矢量 raster/D3D11 中通过，连同两例 Canvas 共 36 例实时绘制；36 例继续显示缺口。** 34 例有 121 帧调度、102 张真实 Chrome 对照、悬停/点击、几何和卸载证据，见 [CSS/D3 实际范围](docs/scene-gallery.md)。使用固定 Tailwind utility 编译适配器，不是完整原始 Tailwind/Grid 页面；文字、几何与命中已是可移植实现（HarfBuzz/FreeType），D2D 只作为 Windows 参考 raster；可见 scene 现通过 D2D GPU 目标与 linear 外部纹理租约合成，CPU 路径保留为正确性参考；固定 4K 基准与限制见 [性能阶段](docs/gallery-performance.md)。翻转平面 GPU 投影、圆环边框和动画计划的后续优化见 [复杂 CSS 阶段](docs/complex-css-performance.md)，边框 filled geometry 与布局复用复制优化、默认/相同 affinity 的完整对照见 [CSS 中段阶段](docs/css-middle-performance.md)。

新增 [owner 热路径优化](docs/owner-hotpath-performance.md)：task/rAF 直接调用、子树 revision 避免无变化快照、数值 paint 与有界静态绘制参数复用。相同 affinity 的三组 4K 中段 CPU P50 6.724→5.738ms、吞吐 140.74→163.04 FPS；主要收益来自调度/快照，scene/诊断字符串仍有成本，未达到 2ms 目标。Debug/Release 全量各76/76，最终相关与 GC 范围各8/8。

新增 [显式源图 / 原生 GPU pass / 独立 UI owner](docs/render-stages-performance.md)：接续方向 3、4、5，C++20 owned 数值 pass 与输入/publication 队列；D3D11 shader 绘制受限 CSS，复杂裁剪/SVG 保留 D2D reference。原始 34 scene + 2 Canvas VM 在同一 worker 创建和销毁，render 只读完整不可变值；4K HUD 分别显示 Present FPS 和 OWNER 更新 Hz。内部 C++ adapters 已接入 gallery，外部 C ABI threading、实际游戏与移动 adapter 仍待验收。此阶段的旧产物保留；当前预览命令见上方共享技术簇。

## 几分钟审阅入口

新增 CRPG gallery 验收基线：只读固定 72 个原样案例与原依赖，离线构建完整页面和分组诊断。可选 owner rAF 已通过生命周期/预算/微任务验证，原 React Spring 三例实际改变 native DOM；短观察 38 例未发现执行错误、34 例阻断，不表示绘制通过。两例原样 Canvas2D 粒子/烟花现已完成原生 64 帧、输入/缩放、Chrome 有界视觉回归与 D3D11 两帧退休，可运行 4K/FPS 窗口，见 [Canvas 实际范围与运行](docs/gallery-canvas.md)。公共外部纹理 lease 已通过 API 扩展与 D3D11 两帧 GPU 合成/实际退休验证，见 [外部纹理合同](docs/external-texture-lease.md)；ANGLE/WebGL2 继续待实现，见 [gallery 实际范围与复现](docs/gallery-foundation.md)。

| 决策 | 采用方案 | 原因和代价 |
| --- | --- | --- |
| 产品范围 | 明确声明的 HTML CSS DOM 子集，逐步扩大 | 限制正确性和维护成本；每项差异必须有诊断与测试 |
| 平台 | Windows 开发验证，Android 和 iOS 为同级产品目标 | 移动 GPU 和中文输入早于前端框架扩展；需要真机和 macOS 验证 |
| 核心语言 | C++20，版本化 C ABI 对外 | 易于接入不同引擎；所有权、错误和线程必须有明确合同 |
| 线程 | DOM、JS、样式和布局由单个 owner 线程持有 | 降低首版同步复杂度；图片等纯数据任务可交给宿主 worker |
| 布局 | 自研格式上下文调度，Yoga 3.2.1 候选仅计算 Flex | CSS 适配与一处已记录补丁通过 33 场景；尚非完整 CSS 布局 |
| 渲染 | 不可变 FramePacket，renderer 准备后由宿主执行 UI pass | 录制与实际提交分开；资源等 GPU 完成后退休 |
| 颜色 | v0.1 设计默认 linear-sRGB 合成 | Windows 探针已验证；50% 白叠黑约 188，Chrome 为 128，记录视觉边界 |
| 文本 | 固定 HarfBuzz 11.2.1、FreeType 2.13.3、libunibreak 6.1、Roboto/Noto CJK | 有界 LTR 中英回退、inline/Unicode 换行与灰度页面通过；bidi、编辑和生产缓存待实现 |
| 脚本 | 固定 QuickJS-NG 0.17.0 验证；设计为每个 View 独立 VM | Windows 原生 DOM/GC 探针已通过；移动构建与框架性能仍需实测 |
| 前端库 | 原版固定 D3、React；Preact 保留待验收 | D3 DOM 和受限 React 页面像素通过；SVG、调度、输入按 workload 扩展 |
| 正确性 | 全量重算参考路径与增量路径差分 | 优化不能改变布局、绘制顺序、命中和事件行为 |
| 日常研发 | AI 按小阶段实现、测试、记录证据 | 用户只审阅有影响的取舍和可运行结果 |

## 首个完整场景

一份包含中英文文本、图片、Flex、绝对定位、按钮、滚动和圆角裁剪的页面，与真实 D3 生成的柱状/折线图表，在 Windows GPU 示例中形成完整链路。随后在 Android 和 iOS 验证触摸、中文组合输入和暂停恢复，再扩大固定版本 Preact/D3 测试应用的动画与交互。

长列表的逻辑数据量与实际 DOM 量分开记录。首版由应用组件做虚拟化，核心保留通用 DOM 和滚动语义。

## 详细设计

- [架构与运行合同](docs/architecture.md)：数据结构、更新、线程、渲染、文本、资源和调试。
- [兼容范围](docs/compatibility.md)：目标能力、分阶段边界和诊断规则。
- [实现与验收计划](docs/implementation-plan.md)：风险探针、里程碑、测试、预算和回退条件。
- [关键取舍及修改条件](docs/decisions.md)：线程、布局、JS 和颜色的决策依据。
- [前端库分级验收](docs/frontend-libraries.md)：D3 的实际证据与基础 SVG、动画和交互的退出条件。
- [Flex 与字体探针](docs/layout-text-probes.md)：浏览器差分、Yoga 补丁和实际文本边界。
- [Inline/滚动/圆角](docs/flow-layout-probe.md)：Unicode 依赖、同步几何、裁剪与实际 React 场景。
- [真实 GPU 探针](docs/gpu-probe.md)：D3D11 提交、资源退休、颜色和 direct/RTT readback。
- [SDF 字体探针](docs/sdf-font-probe.md)：距离编码、padding、缩放/描边、质量边界与复现。
- [Flow 命中与滚轮窗口](docs/flow-input-window-probe.md)：绘制顺序/分片裁剪命中、HWND wheel 与原样 React。
- [原生窗口与鼠标](docs/native-window-probe.md)：布局命中、capture 取消、swapchain/resize 与运行步骤。
- [渲染后端 API](docs/render-backend-api.md)：function table、native pass、能力、宿主提交、失败与设备世代。
- [React / Vite 页面](docs/react-vite-probe.md)：同一 JSX 的浏览器/原生构建、真实 GPU 像素与兼容边界。
- [构建与探针](docs/building.md)：C++20 构建、依赖校验和机器可读报告。
- [代码规范](docs/coding-style.md)：`.clang-format` 格式、一行一声明、命名常量头与反汇编等价验证。
- [架构重构计划](docs/refactoring-plan.md)：现状诊断、目标模块与静多态边界、分步迁移和待确认取舍。
- [当前证据和下一步](docs/status.md)：实际完成情况和平台验证状态。

## 必须先证明的事情

1. Yoga 的测量和 CSS 尺寸适配能处理嵌套 Flex、换行、基线和百分比；不通过就替换算法，不让 DOM 依赖 Yoga。
2. 快照丢帧、热重载、对象销毁和异步资源晚到时，没有失效引用或 GPU 提前释放。
3. 移动端真实页面的 GPU 路径和中文输入能工作；桌面或模拟器结果不能代替真机证据。
4. 增量更新结果与全量重算一致，并且在静态页面上不重复执行布局和绘制列表重建。

研发可以在缺少某个平台环境时推进独立部分，但该平台的验收必须持续保持未验证。
