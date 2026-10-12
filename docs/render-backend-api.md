# 可扩展渲染后端 API

2026-10-07：已实现独立 `render/render_backend` 模块（属 `weft::render`）和实验 v1 C ABI。实际 D3D11 参考适配器、React 原生窗口经过此接口。2026-10-12 起有 Vulkan 渲染器与它的 C ABI 适配器（`weft::render_vulkan`，见下文 Vulkan payload 与实现）；原生 Metal 和 MoltenVK 适配器尚未实现；接口测试中的模拟 Metal/custom 后端只验证合同，不代表平台 GPU 验收。

## 三层边界

| 层 | 负责 | 接收/输出 |
| --- | --- | --- |
| 通用绘制与 renderer facade | 值型绘制输入、能力校验、owner、带世代句柄、有界使用令牌、提交/完成协议 | 不包含 Node、JSValue、COM、Vk 或 Objective-C 类型 |
| 图形 API 适配器 | 复制输入快照、创建和保护资源、材质/上传/录制、退休；解释自己的原生 pass payload | D3D11、Vulkan、Metal 或宿主自定义 function table |
| 宿主/示例壳 | device、窗口/surface、原生录制范围、目标、实际提交、present、真实完成通知 | device epoch、队列身份、submission/completion serial |

公共入口在 [render_backend.h](../include/weft/render_backend.h)。使用 `weft_renderer_create` 注册复制后的 `weft_render_backend` function table；`user_data` 借用到成功 destroy。核心不按 API 编写渲染分支，也不自动创建 Vulkan/Metal device。新增 backend API ID 可按约定从 1024 起分配。所有回调和 renderer 操作都在创建 owner；同步重入返回 busy，异常不能跨 C ABI。

## 当前绘制 profile

`weft_render_frame` 是 prepare 调用期间借用的完整数组：有序 quad、纯色、sRGB straight 图像、线性 coverage、单通道 signed distance。资产使用非零 immutable asset ID/version、尺寸、encoding、字节数和 SDF spread；同一个身份/version 的内容不得修改。准备成功后，适配器必须拥有完整输入快照，调用者可以立即释放原始数组与像素。不同帧不依赖前一个未提交包的上传 delta。

默认上限为两个 prepared、两个未提交 use、64 个总在途 use、65,536 draw、512 asset 和 16 MiB 输入像素，可在 creation descriptor 进一步限制。在录制前校验 asset index、字节数、有限几何/颜色、opacity、UV、SDF 描边范围、capabilities 和 target。不支持能力显式返回错误。四边形帧没有 group 与裁剪，这两项由下面的显示列表帧提供；path/SVG 与生产 atlas 尚未支持。

### 显示列表帧（2026-10-11）

Weft 运行时产出的帧是不可变显示列表（`paint::DisplayList`：盒子、图片、圆角裁剪链、隔离组，带自己的资产 lease），经不透明句柄 `weft_paint_list` 穿过 C ABI。宿主只是转交，不能构造或读取其内容；内置后端读取它。

- 入口 `weft_renderer_prepare_list(renderer, weft_render_list_frame*, handle*)`：帧带列表句柄，以及列表所画的外部纹理的调用方 lease（个数必须等于列表声明的数量，要求已安装外部纹理扩展）。成功后 prepared handle 与四边形帧相同，encode、提交和完成协议不变。
- 能力位 `WEFT_RENDER_FEATURE_DISPLAY_LIST` 与后端表末尾新增的可选回调 `prepare_list` 必须同时出现，否则 create 返回 `INVALID_ARGUMENT`。旧后端表的 `struct_size` 在 `prepare_list` 之前结束，按无此能力处理；对它调用 `prepare_list` 返回 `UNSUPPORTED_FEATURE`。
- 核心在回调前校验：结构（`paint::check`，含组深度上限）、条目数与资产数上限、资产尺寸/字节数/spread 与 16 MiB 预算、有限且可逆的变换、单位范围的颜色与 opacity、盒子的半径/边宽/渐变（最多 8 个色标）、圆角裁剪可以规范化、图片的 encoding 与能力、SDF 描边小于 spread − 1。scene 光栅条目（文字、图层）不能穿过 ABI，返回 `UNSUPPORTED_FEATURE`。
- 后端回调只在调用期间借用句柄；成功的准备自己持有列表的共享引用（列表不可变，不复制条目），外部纹理元数据与四边形帧的扩展相同。
- 外部纹理在列表里排在资产之后：图片的 asset 下标 `assets.size() + i` 是第 i 个外部纹理，按自身颜色绘制、不能带 SDF 描边。底部起始的纹理由后端翻转记录的 uv（`render::flip_rows`），不改列表。

D3D11 参考适配器只有一条绘制路径：四边形帧在适配器里先由可移植的 `render::quad_list()` 转成显示列表（纯色四边形成为盒子，带纹理的成为图片条目），与显示列表帧走同一个线性 pass（`scene.hlsl` 的 `SPACE_LINEAR` 变体）。逐四边形绘制的 `quad.hlsl`、CPU 圆角遮罩与 `lower_to_quads` 已删除；隔离组在渲染器尺寸的线性 float 图层里合成，rtt 合成与线性 alpha 检查也是一条只含一个条目的列表。

所有后端遵守 linear-sRGB premultiplied 合成；coverage/distance 不经过 sRGB transfer。通用 target 类型包括 sRGB/linear RGBA8、BGRA8、linear RGBA16F/RGBA32F，由 backend capability 位选择。类型存在不等于适配器支持；当前 D3D11 reference 宣告 RGBA8 与 RGBA32F，Vulkan 宣告 sRGB RGBA8、sRGB BGRA8 与线性 RGBA16F（设备能混合时），都是单采样。

2026-10-09 增加可选 [external texture lease v1 扩展](external-texture-lease.md)：原 v1 结构保持不变，独立 import/prepare 入口管理有界 generational lease 与 prepared/use pins。D3D11 同 device/queue、linear-premultiplied RGBA8/16F/32F、origin/alias/实际完成已验证；encoded sRGB、ANGLE 和移动尚未接入。

## 原生 pass 与后端扩展

`weft_render_pass` 带 renderer instance、API ID、device epoch、queue、target extent/format/sample count，外加 call-scoped `native_pass` 与字节长度。核心拒绝错实例/API、错 epoch、错误队列和不支持的目标，具体适配器再校验自己的 payload schema 与原生对象。opaque pointer 不是任意对象的通用解释方式；宿主必须传对应后端的有效类型。

[D3D11 payload](../include/weft/render_backend_d3d11.h) 借用新建的 exclusive deferred context 和同 device RTV，返回一份由宿主持有的 command list COM 引用。默认保留目标内容；显式 clear 可用于独立样例。适配器只录制/关闭这份专用 list，不 Execute、Present 或等待。示例支持固定 reference canvas 尺寸，不宣称任意 target geometry 已验收。

[Vulkan payload](../include/weft/render_backend_vulkan.h)（2026-10-12）带设备、命令缓冲与目标视图三个句柄，按 uint64 传递：可分派句柄与 64 位平台上的不可分派句柄是指针值，32 位平台上不可分派句柄本身就是 uint64，所以核心 C ABI 不依赖 Vulkan 头文件。宿主在后端设备的图形队列族上开始一个 primary command buffer，在 render pass 之外交给 encode；适配器录入上传、各 render pass 与隔离组，不结束、不提交、不 present、不等待，之后绑定的管线、描述符、viewport 与 scissor 未定义。目标是渲染器尺寸的单采样二维颜色图像的视图（一层一级 mip），执行时处于 `COLOR_ATTACHMENT_OPTIMAL`、宿主之前的写入已对颜色附件访问可用；命令结束时仍是该布局，写入在 `COLOR_ATTACHMENT_OUTPUT` 阶段完成，宿主读、采样、拷贝或 present 前自己加 barrier。默认保留目标内容。encode 失败时什么也没录：所有资源在录制前建好，录制阶段只发命令、不会失败，所以 Vulkan 适配器的错误总是 native_use=0。

Metal payload 可声明 device、command buffer 和已打开的 render encoder 及 attachment/load/store 合同。这些 native scope schema 分别定义，不能强迫 Metal encoder 遵循 D3D11 deferred-context 模型。通用层不会隐式 begin/end 宿主的 pass；具体 scope 开始、结束和失败取消方式由对应 schema 写明。shader 的 HLSL/SPIR-V/MSL 工具链也由适配器选择。

新增 adapter 需要实现 prepare、release_prepared、encode、discard_encoded、submitted、completed、abandon、shutdown，并选择能力；报告 `WEFT_RENDER_FEATURE_DISPLAY_LIST` 时还要实现 prepare_list（C++ 后端满足 `DisplayListBackend` concept，由 `make_c_backend` 生成）。无需修改 DOM、布局、输入或通用提交状态机。它必须以真实资源和同步实现下面的合同，只有注册一个 API ID 不算后端完成。

## 提交与错误合同

1. prepare 成功返回 owner/epoch/generation handle；输入快照归适配器。
2. encode 在写任何 GPU 资源引用前保护依赖，预留提交容量。核心也在调用 callback 前预留固定 use slot。
3. release_prepared 只释放缓存与 CPU 快照。编码 use 独立保持资源存活。
4. 宿主实际 submit 后调用 `weft_renderer_notify_submitted`。同一次 submit 可绑定多个 use；合法通知不得分配或因容量满而失败。错队列/serial 保留 pin，允许纠正重试。
5. 宿主报告真实完成 timeline，`weft_renderer_notify_completed` 才退休 use。完成 serial 可以覆盖同队列里没有 UI use 的宿主工作。
6. 取消必须先销毁/撤销该 use 的全部原生命令，再 `weft_renderer_discard_encoded`。已提交 use 不能取消。

encode 的错误不总等于“没录制任何命令”。若 callback 完整撤销所有部分命令，它返回 native_use=0；否则错误仍返回有效 use handle，由宿主先取消原生命令再 discard，或者在确实提交部分命令后履行提交/完成协议。这保证失败不会释放仍被引用的资源；提交了部分命令并不代表正确绘制了 UI。

destroy 开始 drain，存在 prepared/encoded/submitted handle 时返回 busy，保留 renderer 与 callback data；继续释放、取消或完成后再 destroy。设备真正丢失时，宿主首先取消未提交 native scope，并保证旧设备永远不会再访问资源，才能 `weft_renderer_abandon_device`。surface 丢失、暂停或窗口关闭不够。旧 prepared 正常 release，新设备创建新 adapter/renderer epoch；实际 driver device-removal 尚未测试。

## 实际实现与复现

[D3D11 reference adapter](../src/render/backends/d3d11/backend.cpp)（`weft::render_d3d11`，经 `RenderBackend` concept 与 `make_c_backend` 生成函数表）使用原有资源状态机与真实 COM/query。它是参考实现，还没有稳定 SDK factory、通用资源 broker、多线程队列或宿主引擎插件。React 三帧离屏 runner 保留旧路径作参考；[可点击窗口](native-window-probe.md) 已走新 API。

[Vulkan adapter](../src/render/backends/vulkan/backend.cpp)（2026-10-12）同样经 `make_c_backend` 生成函数表：显示列表帧，以及经可移植的 `render::frame_packet` 与 `quad_list` 转成显示列表的四边形帧（`frame_packet` 与 D3D11 适配器共用）；一个 prepared 帧、两个在途 use；没有外部纹理扩展。`vulkan::Renderer` 的 prepare 为列表每个资产建纹理并在 `ResourceLifetime` 登记租约与帧；encode 把还没有已提交上传的纹理的上传与整个 pass 录进宿主的命令缓冲。上传规则与跟踪器一致：Created 与 Encoded 的纹理每次录制都上传，所以两个都未提交的录制丢弃其一不影响另一个；同一帧的第二次录制复用已提交的上传。每次录制的暂存、记录、裁剪、描述符池与帧缓冲属于该 use：丢弃时立即销毁，提交后在宿主报告完成时销毁；纹理在没有 prepared 帧与 use 引用后销毁，宿主确认设备丢失后的 abandon 立即退休所有 use。缓存渲染器（`cache_assets`，2026-10-12）与 D3D11 共用可移植的缓存策略（`render/texture_cache.hpp`）：按资产身份保留纹理，小覆盖率资产放进 R8 atlas 页面。页面也是一个跟踪器资源，它的“上传”是首次写入时那次丢弃内容的布局转换：另一个尚未提交的录制正首次写入帧所需的页面时，encode 返回 `BUSY`、什么都不录，宿主提交或丢弃那个录制后再试；已写过的页面按原内容转换，只上传新区域。`render_vulkan_backend_contract` 以宿主身份驱动它（自己的命令缓冲、目标、fence 与 serial），验证层同时开启同步验证。

4K flow 样例现可启用该 adapter 的私有 immutable texture 缓存：相同 asset ID/version 复用 texture/SRV，以及 Ready 或已实际提交的同队列上传，frame 仍传完整有序绘制与资产集合。准备前保护整个输入的已有依赖，避免 LRU 创建较早的 miss 时淘汰较晚的 hit；每帧 lease 和 Prepared/Encoded/Submitted pin 独立于 cache lease。容量为 1,024 资源/128 MiB 纹理有效载荷，只淘汰没有使用 pin 的资源，真实 query completion 前的相关 Submitted pin 阻止淘汰；destroy drain 后释放缓存。Created/Encoded 保留重试字节，取消/录制失败不把它当作 Ready；Submitted 可按同队列顺序读取，真实完成才 Ready。超容量准备失败可 drain/reuse，不遗留 CPU lease。该缓存不扩张 C ABI v1，也不让下一完整包依赖被丢弃的包。

2026-10-12 起缓存渲染器还把不超过 128 px 的覆盖率资产放进共享的 R8 atlas 页面（复制一圈边缘纹素，过滤结果与独立纹理相同），相邻的盒子与同页字形合成一次实例化绘制；放不下时退回独立纹理，淘汰时归还区域，页面清空后才重新装箱。同日起这套策略（资产身份、按身份与版本的 LRU 纹理索引及其淘汰规则、atlas 候选、带边图像与区域 uv）移到可移植的 `render/texture_cache.hpp`，D3D11 与 Vulkan 共用，纹理本身仍是各后端的。

固定值型 Asset 的身份注册表保留源对象防止地址复用，上限 2,048 项/64 MiB 源像素，清空后身份仍单调不重用。它是本探针的适配策略；通用 SDK 的资源版本、device removal 和跨 API 异步缓存仍待实现。`d3d11_warp_render_cache` 与 `--cache-hardware` 用 seed 0xCE902026 的 48 次源图、字号、圆角、位置和绘制顺序 mutation 对照无缓存全量路径，另验证丢包、容量失败、录制重试及提交后失败的真实完成退休，见 [滚动优化](flow-input-window-probe.md#4k-滚动优化2026-10-09)。

4K 宿主默认使用 [TwoFramePipeline](../src/render/backends/d3d11/two_frame_pipeline.hpp)，adapter 只在此有界 host 下允许 prepare 与先前 Submitted use 重叠；一个 owner 顺序 prepare/encode，两个 GPU 在途 use 的配额沿既有 C ABI v1，未改变 API。两个 slot 各自保存 constant buffer、event query、command list 和拥有值的 host lease；HUD 替换仍保留旧 slot 的 GPU 资源。宿主非阻塞 poll 真实 event，只有两个 slot 都未完成时等待最旧者，截图/resize/关闭则 drain。提交后 host 异常仍保留 slot 并结束 completion query，已执行命令不走 discard。独立 `d3d11_warp_two_frame` / `--pipeline-hardware` 验证两个 Submitted use、同队列上传复用、48 帧像素差分、错误和关闭，详见 [两帧合同与测量](flow-input-window-probe.md#两帧流水线2026-10-09)。这不等于稳定 SDK 或 Vulkan/Metal 已实现。

```powershell
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
.\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --backend-hardware artifacts/local/backend-debug/api-hardware
.\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --react-window-test-hardware artifacts/local/backend-debug/window-hardware
```

`render_backend_contract` 验证不同 API/native schema、独立 instance、BGRA 能力、最新世代、输入 ownership、线程/重入、容量、分配失败期间提交/完成、部分录制失败、drain 与 host-confirmed abandon。`render_backend_c_abi` 用 C11 编译公开头文件并调用实际导出符号。

`d3d11_warp_backend_api` 和显式 hardware 测试由宿主提供另一份真实 deferred context 与 RTV：颜色/coverage 和 SDF 与旧 GPU reference 逐像素差异为 0，linear float target 读回 `[0, 0.5, 0, 0.5]`；已有红色目标叠半透明绿色得到 `[188,188,0,255]`。取消与注入部分录制失败不令 upload Ready；错误 device 被拒绝。每会话实际提交四帧，退休四个输入 texture，调试层 warning/error 为零。

原生 React window 的 hit/input/resize/capture/close 与 post-submit failure 继续通过，七帧退休 605 texture。Debug/Release/强制 GC 的完整结果见 [status](status.md)。产物在 artifacts/local/backend-debug、backend-release、backend-gc-stress；自动 WARP 在对应 build/artifacts/backend-$CONFIG/warp 与 window-$CONFIG/warp。

后续按用户当前方向继续用 D3D11 复刻 CaseCheetah 真实 workload，已接入原样九宫格和中文 AlertBar 的值型图片/coverage 快照；多字体栅格仍在 owner 完成，backend 无字体对象依赖。reference adapter 的 CPU 输入 byte budget 从 4 MiB 调到有界 32 MiB，以容纳三张原图（18 MiB decoded），不改变通用 API 的默认参数或所有权合同。Vulkan 与原生 Metal 后续分别接同一协议；跨平台 target/layout、native shader、真机输入与性能都要分别验收，见 [游戏样本](casecheetah-workload.md) 和 [中文组件](cjk-alert-probe.md)。
