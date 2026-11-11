# Weft 实现与验收计划

实现按可运行的完整链路逐步扩大。每阶段交付代码、自动验证、可复现命令和状态记录；在 [status](status.md) 中区分设计、编译、运行和真机结果。M0 对应兼容表的 P0，M1 是桌面原型，M0 到 M5 的必需项全部退出才构成 v0.1。没有固定工期承诺，阶段退出条件决定是否继续扩大范围。

## 候选技术基线

当前优先级随用户方向调整：Windows 继续 D3D11，以 CaseCheetah 的固定只读 UI 样本逐项扩大兼容。共享九宫格、有界中文回退、原样 AlertBar/Zustand/store/UE 协议、公共主题样式与自动尺寸已通过；新增 flow profile 已组合 block/Flex/inline、Unicode 断行、同步 geometry/滚动及圆角。flow paint-order/fragment/rounded 命中和 HWND wheel 消息现已接入原样 React 长列表；实体输入/触摸仍待验收。接下来扩大真实 CSS、档案/视图条与 D3 SVG，继续准备移动工具链。Vulkan/原生 Metal 保留同一 backend 协议，移动目标不变。不得在正在修改的游戏工程中安装、构建或替换 UI，详见 [flow 输入窗口](flow-input-window-probe.md)、[flow 合同](flow-layout-probe.md) 和 [CaseCheetah workload](casecheetah-workload.md)。

| 项目 | 候选 | 锁定前必须验证 |
| --- | --- | --- |
| 核心 | C++20 和 C ABI，CMake 与 CTest | Windows 编译器、C ABI wrapper、arm64 工具链 |
| JS | QuickJS-NG，静态私有链接 | DOM custom class、GC roots、stack/memory/interrupt、框架负载；Windows 和移动构建 |
| Flex | Yoga adapter | Web defaults、intrinsic size、baseline、百分比与文本 measure 的淘汰性差分 |
| 文本 | 固定 HarfBuzz、FreeType、libunibreak 6.1；更完整 paragraph/bidi 库仍待选择 | 字体度量与塑形、有界 UAX14/29 已验证；bidi/语言/编辑、体积与移动仍需验收 |
| 图像 | 一个成熟 PNG decoder | 色彩/alpha、分配上限、worker 所有权与构建；不同时引入多个解码栈 |
| 图形 | 可注册 backend API；Windows D3D11 reference 已接入，Vulkan 与原生 Metal 分别推进 | portable IR、基础 SVG/path/text、shader parity、native pass、capabilities 与 serial 合同 |
| 第三方前端库 | 未修改的固定版本 D3 与 Preact | 真实 native DOM/namespace、SVG 绘制、事件、调度与交互分级验收 |
| 窗口 | Windows 小型 sample host 与移动 native shells | 不进入 core；宿主接口不绑定 sample 的线程/窗口实现 |

QuickJS-NG 官方使用 CMake，并列出 Windows、Android 与 iOS，其中部分平台测试有限。这是候选依据，不是本项目已通过的证明。[构建说明](https://quickjs-ng.github.io/quickjs/building/)、[平台清单](https://quickjs-ng.github.io/quickjs/supported_platforms/)

依赖经探针后固定 release 或完整 commit、来源与校验值，记录构建开关和许可证文本。正常构建默认不联网追 latest。第三方补丁独立保存，记录上游问题、理由和退出条件。

只导出 weft_ C ABI；第三方符号隐藏或在发现宿主冲突时通过可记录的符号隔离方案处理。移动端静态链接、宿主已嵌入其他 VM、不同 CRT/allocator 是集成探针的一部分。

FreeType 的具体授权选择与 notices 随固定版本记录；打包字体的授权独立于字体引擎。项目本身许可证、商用发行策略和外部发布不由技术基线擅自确定。[FreeType 授权](https://freetype.org/license.html)

## M0 架构风险探针

交付 CMake skeleton、基础句柄与 stats、独立探针程序和机器可读报告。探针可以暂不进入最终模块，但验证数据必须保留。

1. JS 生命周期：同一个 node wrapper identity；detached subtree、listener closure、timer/Promise、原生 NodeRef 与 document root；强制 GC 后的回收；View 销毁晚到 callback；无效游戏对象访问。先证明整个 native/JS 强引用图被同一生命周期协议覆盖。
2. Yoga：逻辑几何 fixture 覆盖 auto min-size、basis 0 与 0%、indefinite percentage、box sizing、baseline、多行 Flex、固定尺寸文本、字体/图片晚到与 absolute containing block。与固定浏览器和规范 fixture 比较，允许误差必须注明来源。当前 33 个独立场景通过；字体/图片异步晚到、复杂 intrinsic 组合与格式上下文尚未验证，见 [实际探针](layout-text-probes.md)。
3. 文本：打包中英文、跨 span 空白与换行、组合字符、最终宽度重新排版、UTF-16 caret 与 grapheme 删除；验证 FreeType/HarfBuzz/ICU 数据和内存占用。当前 FreeType/HarfBuzz 与 Ahem 的真实测量、栅格和最终宽度重排通过；固定 Roboto/Noto CJK 的有界中英回退和灰度页面已验证，字体风格与完整体积预算仍待产品确定。独立单通道 SDF 生成/缩放/描边通过；ICU、编辑、生产 atlas、CJK SDF 与移动继续待验收。
4. 平台：先验证 Windows 核心与候选依赖的编译运行；可用时立即做 Android arm64 编译/链接和 iOS device/simulator 编译/链接。
5. 渲染：不依赖 DOM 的矩形、半透明分组、图像和字形，证明快照/prepare/encode/submitted/retire 与颜色合同；CPU release 早于 GPU completion 的资源仍然有效；取消录制、设备 abandon 和 broker 状态能恢复。当前 D3D11 硬件/WARP 的固定 scene、颜色、实际提交/取消/失败重试/query/退休通过；真实 device removal/恢复、长时 GPU 压力和移动路径尚未验证，见 [GPU 探针](gpu-probe.md)。
6. D3：固定原包运行同一份浏览器/native fixture，验证 keyed join、identity、reorder/exit、NS attribute、SVG DOM 与事件。生成节点还不是图表绘制通过；缺失能力进入明确的 SVG/调度队列。

退出条件：核心合同可测试，GC 与布局方案没有需要绕过语义的缺口，候选依赖在可用工具链上可构建。缺少 Mac 或手机不阻止独立探针，但相应平台保持 pending。若 Yoga 必须靠大规模尺寸猜测或第二套布局真相才能工作，应在此淘汰。

当前已有提前贯通的受限 React/Vite 实验：25 个 native 元素的显式 inline/Flex 页面经字体和 D3D11 输出三帧，含计数/keyed reorder 与 browser 差分；离屏 runner 没有 HTML parser、通用 stylesheet 文档加载或 SVG paint；另已接入受限 HWND/swapchain 主鼠标窗口，不能替代下面的退出项，见 [证据](react-vite-probe.md)。

## M1 最小完整 UI 链路

交付原生可控 DOM、结构正确的 HTML 子集、基础 CSS 层叠、显式 Flex、文本格式上下文、简单 block 调度、paint order、基础 SVG 格式上下文和 Windows GPU sample。先走全量正确路径。

用一份按钮、中英文、图片、尺寸变化、绝对定位和裁剪页面完成 HTML → DOM → computed style → layout → text → FramePacket → PreparedFrame → GPU 链路。另将真实 D3 生成的柱状/折线 SVG 图表送入同一链路；不是导出 SVG 后交浏览器代画。不得只用手工 draw commands 冒充完整链路。

退出条件：源码诊断可用；getBoundingClientRect 在 mutation 后读到新几何；固定尺寸文本仍按最终约束排版；半透明重叠和圆角裁剪 visual fixture 通过；headless 几何、软件参考绘制或 readback 检查有独立预期。

## M2 动态正确性

加入事件、焦点、脚本、资源完成、格式上下文级失效、全量参考路径和捕获。资源身份、FramePacket latest-wins、GPU serial 和热重载应能独立压力测试。

测试事件回调移除 target、listener 修改、节点重新插入、ancestor class 改变、文本从 999 到 1000、字体晚到、viewport 改变、transform 改变 containing block、同步 geometry read 与 DOM 写交替。

退出条件：随机 mutation 序列的增量结果与全量结果在 computed style、几何、文本行、paint order 与 hit test 上一致；无变化帧不求解或重建；销毁和 GC 压力无持续增长。浏览器差分另检共同语义 bug。

## M3 移动 GPU 与中文输入

移动真机仍是必需退出项，不能由现有受限 React 探针替代。Android shell 先验证 Vulkan adapter、touch 与 InputConnection；iOS shell 验证原生 Metal adapter、touch 与 UITextInput。图形 API 经 [可注册接口](render-backend-api.md) 录制，宿主拥有 device/queue/native pass 与实际提交；不要求 iOS 经 Vulkan/MoltenVK。共享业务内容和 UI 核心，平台 adapter 处理窗口、输入镜像和度量。

退出条件：中文 composing/marked text、selection、删除 grapheme、键盘 action、焦点切换、软键盘遮挡、横竖屏、安全区域、暂停恢复、surface 失效/重建、内存压力和 GPU 在途资源退休的真机测试通过。

没有对应环境时继续 M2/M4 平台无关部分，M3 不标完成。iOS 的构建与链接需要 macOS/Xcode；模拟器不能代替 iOS device 架构、真实 IME 和 GPU 性能。[Xcode 要求](https://developer.apple.com/xcode/system-requirements/)

## M4 真实应用与性能

交付动态背包、虚拟长列表、中文聊天与动画 HUD，以及固定版本 Preact/D3 测试应用。D3 增加坐标轴、路径、文字、动画、更新与指定交互。先列出真实依赖的 DOM/SVG/调度能力，再补齐组合；不承诺任意框架或插件兼容。

测量 CPU style/layout/paint、JS/native 边界、glyph raster/upload、GPU pass、内存、冷启动和打开页面尖峰。先优化已证实热点，再考虑 worker 文本、局部缓存或更细失效。

退出条件：参考 workload 与硬件固定，指标可复现且达到初始预算，或有真机证据支持并记录预算修订；不能仅凭可复现但不达标的结果退出。warm/cold 和静态/动态/输入分别测试；所有优化在 M2 差分下保持正确。复杂效果与多 View 应另有预算，不能隐含在简单页面数字里。

## M5 开发者交付

交付 DOM 与 computed style 检查、布局可视化、失效原因 trace、资源账目、热重载、C ABI 示例、打包工具和干净环境构建说明。此时再决定具体引擎插件；CDP 已在 2026-11-10 决定采用，见[开发者工具](devtools.md)。

退出条件：开发者无需读核心代码便能定位未支持 CSS、错误尺寸和慢更新；声明的兼容能力都有对应测试；平台支持矩阵有真实证据；发布包在干净环境复现。第一次 SDK 发布之前处理许可证、符号、ABI 和依赖固定。

## 初始预算与测量条件

以下为待真机修订的架构目标，不能对外宣传为已达到性能。

参考场景为 60 Hz、1080×2400 输出、约 2,000 个 live DOM 节点、最多 300 个可见节点；长列表有 10,000 条逻辑数据，虚拟化到不超过 100 行 DOM。固定字体、PNG 图片、脚本 bundle 和动画数量。

| 指标 | 初始目标 | 定义 |
| --- | --- | --- |
| UI CPU 总工作量 | P95 ≤ 2 ms，P99 ≤ 4 ms | 按帧归因的 owner、worker、renderer Prepare/Encode、上传准备与宿主 UI 提交 CPU 时间之和 |
| owner 与跨线程 critical path | 单列测量，不能用总 CPU 替代 | 报告 owner wall time、等待和端到端可见延迟，确认仍满足帧预算 |
| UI GPU | P95 ≤ 1 ms，P99 ≤ 2 ms | UI pass 与必要离屏/合成，GPU timestamp 实测 |
| CPU 稳态内存 | ≤ 64 MiB | 单 View 基准统计整个 Runtime 实占，不分摊隐藏共享缓存；含 VM、DOM、字体图片、staging、包与任务 |
| GPU 稳态内存 | ≤ 32 MiB | 整个 Runtime 的图集、图片、buffer 和 UI 额外离屏；宿主已有 scene target 单列 |
| 冷启动与重载峰值 | CPU ≤ 96 MiB，GPU ≤ 48 MiB | 包含同时存活的新旧资产、任务、staging 和在途资源；同样是待真机修订目标 |
| 静态页面 | 0 次 style/layout/paint-list 重建 | GPU 重放和合成允许发生，统计不排除资源首次准备 |
| 长时稳定性 | 30 分钟压力运行无持续增长 | warm caches 后节点、heap、资源和在途包数量有界 |

Cold start 与首次打开单独报告，不与稳态混算。异步 worker 的工作归因到触发该工作的帧或资产任务，保留各线程原始 trace，不能把后台成本排除。Runtime allocator 账目与进程 private bytes/RSS 增量对照，发现 VM/第三方或映射数据的漏记；代码与包体大小另列。

性能报告必须列设备型号、OS、GPU/driver、画幅、DPR、Release 编译选项、内容 hash、warmup/采样时段、温度状态和实际节点数。GPU 无 timestamp 时标为未测，不能用 CPU submit 时间代替。

记录 CPU/GPU 的平均值、P50/P95/P99、最大值和采样数。测耗电或温升时使用对应设备方法与基线；不能从低 draw-call 数推断能耗。

## 自动化验证

- 语义 fixture：独立预期的层叠、Flex、block/text、事件和 geometry，用选定 WPT 与自建用例。
- 差分：固定浏览器版本、同一字体和 viewport 比逻辑几何；截图采用有解释的容差，不能忽略结构性差异。
- 增量：随机 mutation 与全量重算比较；保存失败 seed 和最小化输入。
- 生命周期：detached nodes、native/JS cycles、资源晚到、丢帧、热重载、在途 GPU frame 期间关闭 View。
- 输入：真实事件序列、组合文本 revision 冲突、旧 FocusEpoch、touch cancel 和暂停恢复。
- 后端：opacity group、裁剪、alpha/color、纹理格式和资源完成；同一捕获在不同后端回放。
- 内存：可用平台启用 sanitizer/调试层，受控配额与 allocation failure；审查缓存/队列上限。

平台状态分别记录配置、编译、链接、模拟器运行、真机功能、真机性能六级证据。CI 文件和 pipeline 设计本身不算运行结果；外部 CI 尚未开通时保留本地命令和待执行清单。

## 失败时的调整规则

| 失败 | 调整 |
| --- | --- |
| Yoga 语义不满足承诺范围 | 保持 DOM/style/格式上下文接口，替换 Flex adapter 或对应算法 |
| QuickJS-NG 真实负载慢 | 先量测 DOM/桥接和更新频率；同一 workload 比替代 VM，不能靠 API 抽象保证零成本替换 |
| 字体/ICU 体积超预算 | 裁剪实际所需字体和 Unicode 数据，记录能力边界；不能自写简化规则冒充 Unicode |
| 首版 GPU 慢 | 先查像素覆盖、上传、离屏和资源准备；不要只盯 draw calls |
| 增量出现分歧 | 保存反例，回退更保守失效范围；不得关掉检查后宣称优化完成 |
| 移动环境不可用 | 继续可独立验证的工作，移动阶段保持 pending；交付称 desktop prototype |
| 首版范围膨胀 | 优先完整场景与既有承诺，删除尚未承诺的扩展；支持矩阵同步变更 |

用户需要审阅的是阶段结果、少数有影响的取舍和实际环境缺口。实现细节、常规调试、依赖探针与测试维护由研发方持续处理。

M0 自动布局追加：受限 block/Flex/absolute 合并、FontSet intrinsic 与同源 mutation/browser/GPU 证据已落地，见 [实际合同](automatic-layout-probe.md)。下一步扩展 inline 与 Unicode 换行，不从 column packing 推断一般 block 支持。
