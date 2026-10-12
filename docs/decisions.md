# Weft 关键架构决策

这些决策构成设计基线，事实依据与需要验证的判断分开记录。QuickJS-NG 0.17.0、D3 7.9.0、Yoga 3.2.1、FreeType 2.13.3 与 HarfBuzz 11.2.1 已固定源码和校验值，Windows 探针结果见 [status](status.md)。固定版本允许复现探针，不等于候选全部验收。改变决策须保留反例、测量或需求依据，并同步架构、兼容表和阶段计划。

## 单个 UI owner

采用 DOM、JS、style 和 layout 单 owner，宿主可在游戏线程调用 Advance，也可安排到独立 owner。worker 只处理不可变输入，render 只处理不可变快照。

原因是首版主要风险在同步 geometry、事件重入、资源晚到和生命周期。把 DOM/布局/脚本分线程会增加协调状态，尚无性能证据支持其成本。

修改条件是确定性负载证明 owner critical path 超预算，并且可拆分的纯任务占主要耗时。先考虑文本或资源准备 worker，再讨论并行布局；不采用异步 DOM 可见性作为隐式优化。

## 自研 CSS 语义与可替换 Flex 后端

自己控制层叠、默认值、computed style、格式上下文、intrinsic sizing 和失效。Yoga 是 Flex 算法候选，不作为 DOM 结构或完整布局语义来源。

官方文档显示 Yoga 默认值、box sizing、单位和 shorthand 与 Web 有差异，measure 也不保证在 definite 尺寸下执行。[Yoga 样式](https://www.yogalayout.dev/docs/styling/)、[Yoga 测量](https://www.yogalayout.dev/docs/advanced/external-layout-systems)

修改条件是 M0 的 auto minimum、百分比、baseline、文本换行或 containing block 测试不能通过合理 adapter 解决。此时保留布局协议，替换对应算法；不扩大一层又一层补丁来掩盖第二套布局真相。

当前独立探针由原始 24 场景中的 baseline 14/24、CSS adapter 23/24，推进到 33/33 浏览器对照通过。Yoga 3.2.1 在 indefinite column 中把已解析 length flex-basis 当成 content；保存了一处条件补丁，限定 column，保留 row intrinsic 算法和回归反例。CSS 默认值、自动最小尺寸和最终文本排版由 adapter 处理，输出只有一份 Yoga 盒几何。该结果支持继续验证 Yoga；复杂 intrinsic 组合、资源晚到和真实格式上下文仍未退出 M0。补丁尚未向上游提交；替换为通过反例的上游版本后删除。见 [探针与证据](layout-text-probes.md)。

## QuickJS NG 候选与统一回收探针

优先验证 QuickJS-NG 的解释器路径、CMake 构建和私有静态集成，不依赖第三方 JIT。官方提供 Windows/移动平台清单，但移动测试程度有限，不能直接当项目支持证据。[构建](https://quickjs-ng.github.io/quickjs/building/)、[平台](https://quickjs-ng.github.io/quickjs/supported_platforms/)

VM adapter 隔离第三方 API，但实际 GC 与 DOM ownership 紧密相关，不能宣称替换 VM 零成本。M0 验证节点强引用图与 JS graph 的统一 tracing、原生 roots 和 DestroyView 顺序；没有循环回收证据前不扩大 DOM API。

当前 Windows M0 探针已实现 custom-class gc_mark、原生 roots、detached node 和 closure/expando 循环回收，原版 D3 通过 29 项 DOM 行为检查。这允许继续验证该 VM，不代表移动构建、完整运行时或 SVG 像素输出已通过。

修改条件是平台构建、跨语言生命周期或固定框架负载不满足合同。先归因 DOM/bridge 开销，再比较替代 VM。默认只交付源码/bundle，bytecode 为可再生成缓存。

## 先贯通真实 React 页面

用户要求用 React/Vite 页面验证 native 渲染。固定原版 React runtime，以标准 JSX/createRoot/useState 调用自己的 DOM；Vite 同时输出普通 browser app 和 native IIFE。本阶段先接受显式 Flex、行内声明与单行文字，缺失格式/paint 报错，不把 block 偷换成 Flex。Node 不进入 render：owner 复制 native DOM，再生成逻辑盒/glyph 的自有值快照，GPU query 才决定纹理退休。

实际三帧计数/keyed reorder、browser 几何和强制 GC 已验证，见 [React profile](react-vite-probe.md)。随后接入受限 Windows 主鼠标窗口，见 [窗口证据](native-window-probe.md)。此路径用于验证模块接口，不替代 HTML parser、CSS cascade、同步 geometry、完整输入、生产 atlas 和移动验收。下一步逐项扩展标准接口；普通 Vite HMR/ESM 不作为 game runtime 必需能力，发布 bundle 是目前入口。

## 窗口输入在 owner 安全点处理

Win32 WindowProc 仅记录有界输入值与窗口元数据，owner 在安全点处理消息、flush 最新布局、按弱代际身份派发 MouseEvent。消息保存接收时的 viewport 变换，避免 resize 改写已排队坐标；mouseup 回调后重读当前树决定 click。关闭也延后到安全点，避免 Present 重入销毁 HWND。MouseEvent prototype 由 VM root 持有，事件路径保持原生节点存活，跨线程不传 Node/JSValue。

窗口持续使用同一 D3D11 device。UI 与宿主合成共享 completion serial，完成 marker 每次提交只写一次；提交后异常也先 drain，再释放 swapchain views。隐藏 HWND 的消息/缩放/取消/关闭和提交后失败已实测；实体鼠标、多显示器 DPI、device removal 与完整焦点/IME 仍需验收。这是全量正确性参考路径，每帧阻塞等待并重建 glyph texture，后续性能优化须保留相同生命周期合同。

## UI 录制与宿主 GPU 提交

图形 API 使用独立 adapter function table，当前已有实验 C ABI facade 和实际 D3D11 reference。通用层不解释 COM/Vk/Objective-C 对象，也不创建窗口/device；native pass payload 由具体 adapter 校验。Vulkan 与原生 Metal 分别实现，不以 Vulkan 作为所有平台必经层。只抽象实际 UI 绘制能力，target/capabilities、失败取消和资源退休明确记录，详见 [API 合同](render-backend-api.md)。

采用 FramePacket → PreparedFrame → Encode/UseToken → NotifySubmitted → CompletionSerial。快照是逻辑绘制与不可变资产依赖；图集和后端对象由 renderer 持有。独立 sample 可封装 submit，核心不替引擎提交 command buffer。

原因是 UI pass 可能只是游戏 command buffer 的一个片段。CPU 包释放、录制完成和 GPU 完成是不同事件，不能用固定三帧延迟替代 fence。[Metal buffer 生命周期](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/TripleBuffering.html)

修改条件是接入具体宿主时需要其他 resource synchronization；扩展队列身份和同步合同，不取消资源 pin。Device lost 的 abandon 必须由宿主确认旧设备不会再使用资源，surface 不可用不足以触发 abandon。

## v0.1 颜色合成

设计默认采用 linear-sRGB premultiplied-alpha 内部合成。CSS 与普通 PNG 输入视为 sRGB straight color，先 decode，再 premultiply。Coverage/alpha 不做颜色 transfer。D3D11、GLES3、Metal 与 direct/RTT 必须遵守相同规则。

理由是目标为游戏 UI，线性输入输出边界更容易与宿主的线性场景 pass 一致。代价是半透明 UI 的混合可能与浏览器编码空间混合不同，因此不承诺浏览器逐像素相似。[CSS 颜色](https://www.w3.org/TR/css-color-3/)、[GPU framebuffer blending](https://docs.vulkan.org/spec/latest/chapters/framebuffer.html)

M0 颜色探针要固定并检查：50% 白色叠黑色、两个半透明重叠、透明彩色边缘、父 opacity group、glyph coverage、direct/RTT 和固定 Chromium 对照。编码空间里 0.5 白叠黑约为 0.5；线性合成再 sRGB 编码约为 0.735，足以区别两种规则。该数值是数学示例，不是已测试浏览器结果。

直接绘制只在宿主目标的 encoding、blend 和 alpha 合同匹配时可用，不匹配时要转换或使用规范化离屏目标。HDR 的 tone mapping 和 gamut 不能由核心猜测，v0.1 不宣称完整 HDR 支持。

当前独立 D3D11 硬件/WARP 探针已实际验证该合同：50% 白叠黑的 sRGB readback 约 188，Chrome 截图为 128；透明 linear target 为正确 premultiplied RGBA。父 opacity、透明图片边缘、glyph coverage 与 direct/RTT 检查通过。保留 linear-sRGB 设计选择，记录浏览器视觉差异；尚未验证移动后端、任意宿主目标或完整页面。见 [GPU 证据](gpu-probe.md)。

修改条件是产品明确要求浏览器颜色匹配，或移动真机证明这条路径不可接受。必须新增颜色模式或修改唯一默认并同步 golden；adapter 不得自己改默认。颜色探针与具体输出映射没有通过前，不宣布图形后端完成。

## 静多态，动态分派只在 C ABI

引擎内部的渲染后端、文本塑形/字体、平台服务和样式 profile 用 concept 约束的模板实现，热路径不用 virtual 或 `std::function`。宿主看到的仍是版本化 C 函数表：每个后端由 `make_c_backend<B>()` 一类的适配生成 thunk，唯一的间接调用在 ABI 边界。

用户于 2026-10-10 确认：运行时性能比编译时间重要。代价是编译变慢；同一二进制带多个后端时各自实例化，体积增加。

修改条件是某平台必须在运行时切换后端，且多份实例化的体积不可接受。此时在 C ABI 层增加选择，不把虚函数引入逐帧路径。

## 错误作为值返回，UI 错误不终止游戏

核心路径逐步改为 `expected<T, Diag>` 风格：`Diag` 是枚举诊断码加上下文，替代现有 153 个 `"WEFT_*"` 字符串前缀和按前缀匹配的控制流。解析、样式、布局、脚本和资源错误按 View 或节点降级（跳过该声明、该节点不绘制、该回调失败），记录诊断并交给宿主；不吞掉错误，也不因此终止进程。异常不跨 C ABI 的既有不变量保持。

用户于 2026-10-10 确认：游戏不能因为 UI 的小错误直接崩溃，返回错误值更合适。主机平台常关闭异常和 RTTI，也要求这一方向。迁移按 [重构计划](refactoring-plan.md) 第 1 步开始，逐模块进行，迁移期间异常只在模块内部使用并在边界转换。

修改条件：内存耗尽或内部不变量被破坏这类不可恢复状态，可以销毁对应 View 并报告宿主，但仍不终止宿主进程。

## 统一格式化路径，退役历史 profile

page、positioned、auto 三个早期 profile 与 flow、scene 重复实现 CSS 解析、paint 发射和命中。统一为一个格式上下文调度器、一个 Yoga 适配器、一份 paint IR 和一条命中路径。这三个 profile 原有的浏览器对照全部跑在统一路径上并通过后，删除旧代码（用户于 2026-10-10 确认）。2026-10-11 第 8.1 步已执行：三个 profile 的对照全部按原阈值通过后删除。第 8.2 步：文档与 scene 共用一个 Yoga 适配器，scene 的对照逐字节不变。第 8.3 步：scene 只有一个 transform 模型，文档与 scene 共用一条命中路径。一份 paint IR 按用户 2026-10-11 的决定推迟到渲染器合并（第 10 步渲染工作的开头）：两个渲染器的文字、图层与裁剪模型不同，只合并数据类型没有收益。

修改条件：如果统一路径在某个对照上无法达到旧路径的结果，先修统一路径或明确记录差异，不保留第二条实现。

## 文本测量只用跨平台栈

scene 布局的文本测量从 DirectWrite 改为与 flow 相同的 HarfBuzz/FreeType/libunibreak 路径；D2D/DirectWrite 只保留为 Windows 参考 raster，不进入可移植层，UI owner 线程不再需要 COM。用户于 2026-10-10 确认：依赖 Windows 违背跨平台设计需求。

代价是字体度量改变，现有 102 张 Chrome 对照图需要在迁移时重新采集基线并记录差异；如需调整对照阈值，须单独说明理由。

## 字形材质：灰度与 SDF 并存

逻辑塑形和布局共用 HarfBuzz 的度量，renderer 决定 raster 模式。小字号正文优先灰度，大幅缩放/描边可用通过质量验收的 SDF。单通道 SDF 使用已固定 FreeType 的 bsdf generator，不另写轮廓距离算法；显式 Outline 作为实验选项，generator 进入缓存身份。距离 padding 不改变文字度量，atlas 仍按宿主实际完成退休。

Windows 独立探针的 R8 距离材质、三种倍率、描边和 direct/RTT 已通过，详见 [实际证据](sdf-font-probe.md)。尚未选择产品字体或实现 MSDF；单通道尖角误差、极端缩小与移动显示质量未验收。若固定产品场景的尖角/大倍率质量无法达到阈值，再验证 MSDF；若生成耗时或 atlas 预算过高，保留灰度路径并调整 raster 策略。不会将这些选择暴露为改变 CSS 排版的属性。


## WebGL 不在目标内，3D 交给宿主引擎

用户于 2026-10-12 决定：WebGL/WebGL2 与固定 gallery 里两例 Three.js（webgl-02、webgl-03）移出需求。

依据：
- 游戏引擎渲染 3D 的能力强于 Three.js，在 UI 里再跑一套 WebGL 是和引擎抢活；
- 3D 内容由引擎渲染进纹理，经外部纹理在 UI 中合成：同设备、零拷贝，D3D11 与 Vulkan 已实现；
- 完整 WebGL 需要 JS 绑定、校验、一致性测试和 ANGLE 这一大型依赖，换不来对应的价值。

`getContext('webgl')` 与 `getContext('webgl2')` 返回 `null`，与浏览器不支持时一致，库自行降级。烟雾等小型着色器效果仍在 UI 内做，作为单独的 UI 着色器效果目标设计，不经过 WebGL。

修改条件：出现必须原样运行的 WebGL 网页库需求时，ANGLE 方案仍可接在同一条外部纹理合成路径上。
