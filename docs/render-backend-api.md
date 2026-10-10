# 可扩展渲染后端 API

2026-10-07：已实现独立 `caseelephant_render_backend` 模块和实验 v1 C ABI。实际 D3D11 参考适配器、React 原生窗口经过此接口。Vulkan、原生 Metal 和 MoltenVK 适配器尚未实现；接口测试中的模拟 Metal/custom 后端只验证合同，不代表平台 GPU 验收。

## 三层边界

| 层 | 负责 | 接收/输出 |
| --- | --- | --- |
| 通用绘制与 renderer facade | 值型绘制输入、能力校验、owner、带世代句柄、有界使用令牌、提交/完成协议 | 不包含 Node、JSValue、COM、Vk 或 Objective-C 类型 |
| 图形 API 适配器 | 复制输入快照、创建和保护资源、材质/上传/录制、退休；解释自己的原生 pass payload | D3D11、Vulkan、Metal 或宿主自定义 function table |
| 宿主/示例壳 | device、窗口/surface、原生录制范围、目标、实际提交、present、真实完成通知 | device epoch、队列身份、submission/completion serial |

公共入口在 [render_backend.h](../include/caseelephant/render_backend.h)。使用 `ce_renderer_create` 注册复制后的 `ce_render_backend` function table；`user_data` 借用到成功 destroy。核心不按 API 编写渲染分支，也不自动创建 Vulkan/Metal device。新增 backend API ID 可按约定从 1024 起分配。所有回调和 renderer 操作都在创建 owner；同步重入返回 busy，异常不能跨 C ABI。

## 当前绘制 profile

`ce_render_frame` 是 prepare 调用期间借用的完整数组：有序 quad、纯色、sRGB straight 图像、线性 coverage、单通道 signed distance。资产使用非零 immutable asset ID/version、尺寸、encoding、字节数和 SDF spread；同一个身份/version 的内容不得修改。准备成功后，适配器必须拥有完整输入快照，调用者可以立即释放原始数组与像素。不同帧不依赖前一个未提交包的上传 delta。

默认上限为两个 prepared、两个未提交 use、64 个总在途 use、65,536 draw、512 asset 和 16 MiB 输入像素，可在 creation descriptor 进一步限制。在录制前校验 asset index、字节数、有限几何/颜色、opacity、UV、SDF 描边范围、capabilities 和 target。不支持能力显式返回错误；当前不支持通用 group、clip/path/SVG、生产 atlas。

所有后端遵守 linear-sRGB premultiplied 合成；coverage/distance 不经过 sRGB transfer。通用 target 类型包括 sRGB/linear RGBA8、BGRA8、linear RGBA16F/RGBA32F，由 backend capability 位选择。类型存在不等于适配器支持；当前 D3D11 reference 宣告 RGBA8 与 RGBA32F，单采样。

2026-10-09 增加可选 [external texture lease v1 扩展](external-texture-lease.md)：原 v1 结构保持不变，独立 import/prepare 入口管理有界 generational lease 与 prepared/use pins。D3D11 同 device/queue、linear-premultiplied RGBA8/16F/32F、origin/alias/实际完成已验证；encoded sRGB、ANGLE 和移动尚未接入。

## 原生 pass 与后端扩展

`ce_render_pass` 带 renderer instance、API ID、device epoch、queue、target extent/format/sample count，外加 call-scoped `native_pass` 与字节长度。核心拒绝错实例/API、错 epoch、错误队列和不支持的目标，具体适配器再校验自己的 payload schema 与原生对象。opaque pointer 不是任意对象的通用解释方式；宿主必须传对应后端的有效类型。

[D3D11 payload](../include/caseelephant/render_backend_d3d11.h) 借用新建的 exclusive deferred context 和同 device RTV，返回一份由宿主持有的 command list COM 引用。默认保留目标内容；显式 clear 可用于独立样例。适配器只录制/关闭这份专用 list，不 Execute、Present 或等待。示例支持固定 reference canvas 尺寸，不宣称任意 target geometry 已验收。

未来 Vulkan payload 可以声明 VkCommandBuffer、目标 image/view、render-pass/dynamic-rendering 范围、layout/barrier 的入出状态与责任；Metal payload 可声明 device、command buffer 和已打开的 render encoder 及 attachment/load/store 合同。这些 native scope schema 分别定义，不能强迫 Metal encoder 遵循 D3D11 deferred-context 模型。通用层不会隐式 begin/end 宿主的 pass；具体 scope 开始、结束和失败取消方式由对应 schema 写明。shader 的 HLSL/SPIR-V/MSL 工具链也由适配器选择。

新增 adapter 需要实现 prepare、release_prepared、encode、discard_encoded、submitted、completed、abandon、shutdown，并选择能力。无需修改 DOM、布局、输入或通用提交状态机。它必须以真实资源和同步实现下面的合同，只有注册一个 API ID 不算后端完成。

## 提交与错误合同

1. prepare 成功返回 owner/epoch/generation handle；输入快照归适配器。
2. encode 在写任何 GPU 资源引用前保护依赖，预留提交容量。核心也在调用 callback 前预留固定 use slot。
3. release_prepared 只释放缓存与 CPU 快照。编码 use 独立保持资源存活。
4. 宿主实际 submit 后调用 `ce_renderer_notify_submitted`。同一次 submit 可绑定多个 use；合法通知不得分配或因容量满而失败。错队列/serial 保留 pin，允许纠正重试。
5. 宿主报告真实完成 timeline，`ce_renderer_notify_completed` 才退休 use。完成 serial 可以覆盖同队列里没有 UI use 的宿主工作。
6. 取消必须先销毁/撤销该 use 的全部原生命令，再 `ce_renderer_discard_encoded`。已提交 use 不能取消。

encode 的错误不总等于“没录制任何命令”。若 callback 完整撤销所有部分命令，它返回 native_use=0；否则错误仍返回有效 use handle，由宿主先取消原生命令再 discard，或者在确实提交部分命令后履行提交/完成协议。这保证失败不会释放仍被引用的资源；提交了部分命令并不代表正确绘制了 UI。

destroy 开始 drain，存在 prepared/encoded/submitted handle 时返回 busy，保留 renderer 与 callback data；继续释放、取消或完成后再 destroy。设备真正丢失时，宿主首先取消未提交 native scope，并保证旧设备永远不会再访问资源，才能 `ce_renderer_abandon_device`。surface 丢失、暂停或窗口关闭不够。旧 prepared 正常 release，新设备创建新 adapter/renderer epoch；实际 driver device-removal 尚未测试。

## 实际实现与复现

[D3D11 reference adapter](../probes/probe_render_backend.hpp) 使用原有资源状态机与真实 COM/query。它是 probe-private reference，还没有稳定 SDK factory、通用资源 broker、多线程队列或宿主引擎插件。React 三帧离屏 runner 保留旧路径作参考；[可点击窗口](native-window-probe.md) 已走新 API。

4K flow 样例现可启用该 adapter 的私有 immutable texture 缓存：相同 asset ID/version 复用 texture/SRV，以及 Ready 或已实际提交的同队列上传，frame 仍传完整有序绘制与资产集合。准备前保护整个输入的已有依赖，避免 LRU 创建较早的 miss 时淘汰较晚的 hit；每帧 lease 和 Prepared/Encoded/Submitted pin 独立于 cache lease。容量为 1,024 资源/128 MiB 纹理有效载荷，只淘汰没有使用 pin 的资源，真实 query completion 前的相关 Submitted pin 阻止淘汰；destroy drain 后释放缓存。Created/Encoded 保留重试字节，取消/录制失败不把它当作 Ready；Submitted 可按同队列顺序读取，真实完成才 Ready。超容量准备失败可 drain/reuse，不遗留 CPU lease。该缓存不扩张 C ABI v1，也不让下一完整包依赖被丢弃的包。

固定值型 Asset 的身份注册表保留源对象防止地址复用，上限 2,048 项/64 MiB 源像素，清空后身份仍单调不重用。它是本探针的适配策略；通用 SDK 的资源版本、device removal 和跨 API 异步缓存仍待实现。`d3d11_warp_render_cache` 与 `--cache-hardware` 用 seed 0xCE902026 的 48 次源图、字号、圆角、位置和绘制顺序 mutation 对照无缓存全量路径，另验证丢包、容量失败、录制重试及提交后失败的真实完成退休，见 [滚动优化](flow-input-window-probe.md#4k-滚动优化2026-10-09)。

4K 宿主默认使用 [TwoFramePipeline](../probes/two_frame_pipeline.hpp)，adapter 只在此有界 host 下允许 prepare 与先前 Submitted use 重叠；一个 owner 顺序 prepare/encode，两个 GPU 在途 use 的配额沿既有 C ABI v1，未改变 API。两个 slot 各自保存 constant buffer、event query、command list 和拥有值的 host lease；HUD 替换仍保留旧 slot 的 GPU 资源。宿主非阻塞 poll 真实 event，只有两个 slot 都未完成时等待最旧者，截图/resize/关闭则 drain。提交后 host 异常仍保留 slot 并结束 completion query，已执行命令不走 discard。独立 `d3d11_warp_two_frame` / `--pipeline-hardware` 验证两个 Submitted use、同队列上传复用、48 帧像素差分、错误和关闭，详见 [两帧合同与测量](flow-input-window-probe.md#两帧流水线2026-10-09)。这不等于稳定 SDK 或 Vulkan/Metal 已实现。

```powershell
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
.\build\windows-msvc\bin\Debug\ce_d3d11_probe.exe --backend-hardware artifacts/local/backend-debug/api-hardware
.\build\windows-msvc\bin\Debug\ce_d3d11_probe.exe --react-window-test-hardware artifacts/local/backend-debug/window-hardware
```

`render_backend_contract` 验证不同 API/native schema、独立 instance、BGRA 能力、最新世代、输入 ownership、线程/重入、容量、分配失败期间提交/完成、部分录制失败、drain 与 host-confirmed abandon。`render_backend_c_abi` 用 C11 编译公开头文件并调用实际导出符号。

`d3d11_warp_backend_api` 和显式 hardware 测试由宿主提供另一份真实 deferred context 与 RTV：颜色/coverage 和 SDF 与旧 GPU reference 逐像素差异为 0，linear float target 读回 `[0, 0.5, 0, 0.5]`；已有红色目标叠半透明绿色得到 `[188,188,0,255]`。取消与注入部分录制失败不令 upload Ready；错误 device 被拒绝。每会话实际提交四帧，退休四个输入 texture，调试层 warning/error 为零。

原生 React window 的 hit/input/resize/capture/close 与 post-submit failure 继续通过，七帧退休 605 texture。Debug/Release/强制 GC 的完整结果见 [status](status.md)。产物在 artifacts/local/backend-debug、backend-release、backend-gc-stress；自动 WARP 在对应 build/artifacts/backend-$CONFIG/warp 与 window-$CONFIG/warp。

后续按用户当前方向继续用 D3D11 复刻 CaseCheetah 真实 workload，已接入原样九宫格和中文 AlertBar 的值型图片/coverage 快照；多字体栅格仍在 owner 完成，backend 无字体对象依赖。reference adapter 的 CPU 输入 byte budget 从 4 MiB 调到有界 32 MiB，以容纳三张原图（18 MiB decoded），不改变通用 API 的默认参数或所有权合同。Vulkan 与原生 Metal 后续分别接同一协议；跨平台 target/layout、native shader、真机输入与性能都要分别验收，见 [游戏样本](casecheetah-workload.md) 和 [中文组件](cjk-alert-probe.md)。
