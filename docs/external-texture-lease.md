# 外部 GPU 纹理 lease：实际合同

2026-10-09：已实现图形 API 无关的实验 C ABI 扩展和 probe-private D3D11 adapter。宿主生成的 GPU 纹理可以直接进入 UI 有序 quad 合成，源图无需 CPU 读回/重传。现有 `render_backend.h` v1 的结构、函数表与普通帧入口保持不变；新增入口位于 [render_external_texture.h](../include/weft/render_external_texture.h)。它是宿主 viewport（引擎渲染的纹理）进入 UI 的入口；WebGL2/ANGLE 已于 2026-10-12 移出需求，见 [决策](decisions.md)。

## 所有权与同步

宿主创建普通 renderer，在空闲时安装一次 `weft_render_external_backend`。扩展提供 import/release/prepare 回调，原 backend 的 encode/release/submitted/completed 负责扩展生成的 prepared frame；两套回调必须理解同一个 native frame/use 命名空间。通用层没有 COM、Vulkan 或 Metal 分支。

成功 import 后，adapter 持有强 native 引用，通用层返回带 renderer/device epoch/slot/generation 的调用方 lease。描述与 native payload 只在 import 调用期间借用；退休 callback 的 user data 必须保持到 callback。失败没有引用转移，也不触发 callback。当前没有跨线程调用或隐式 retain；同一个有效调用方 lease 可用于多个 prepared/use。

`weft_render_external_frame` 传完整 CPU 资产和完整 external handle 数组，draw 索引先 CPU 后 external。prepare 复制完整绘制数据并保护全部依赖；encode 在命令开始前独立增加 use pin，部分错误返回有效 use 时也保留 pin。帧可独立录制，不依赖被丢弃的旧帧。调用方数组、native view 和 prepared 可随后销毁。

`weft_renderer_release_texture` 立即使调用方句柄失效，已有 prepared/encoded/submitted pin 继续有效。所有 pin 和调用方 lease 都消失后，先调用 adapter release，再在 owner 调用退休 callback，恰好一次。已提交使用只能等宿主真实 completion serial；等待固定帧数、录制结束或释放 prepared 都不能提前退休。callback 不得分配、抛出异常或重入 renderer。合法 submitted/completed 的容量已在安装/encode 前预留，不再分配。

同步模式有两种，生产者写入都必须在每个 UI 消费提交之前进入同 device、同宿主队列：
- `SAME_QUEUE_ORDERED`（冻结）：import 到退休期间内容冻结，不能原地刷新仍在途的纹理。动态生产者轮换两张或更多纹理，每次发布新的版本，回调后才允许复用；只 release 调用方句柄还不够。adapter 拒绝同一 native resource 在旧 import 存活时伪装成新版本重新导入。
- `SAME_QUEUE_LIVE`（2026-10-15，13.3c）：生产者在采样它的帧之间重写同一张纹理（引擎每帧重画的 render target），导入一次、一直绑定；每次写入排在之前采样它的帧的提交之后（Vulkan 上自己加从片元着色器读出发的屏障）、之后的帧之前。content id/version 只命名纹理，不代表内容；Weft 不按它缓存内容，每次按帧采样。

跨 device、keyed mutex、共享资源和异步多队列没有自动 wait/signal 路径。

这个 UI 回调只证明 UI 使用结束。没有 UI 提交的 producer-only GPU 工作仍由宿主自己保护和确认完成。取消已录制命令必须先销毁全部 native commands，再 discard use。设备 abandon 必须有宿主“旧设备永不再访问”保证，未提交命令先取消；prepared 和调用方 lease 仍正常 release，退休原因变为 `ABANDONED`。真实 driver device-removal 仍未验收。

renderer destroy 遇到任何 live import 都返回 busy 并保留回调数据，进入 drain 后不能产生新工作。registry 上限和按尺寸/格式计算的逻辑纹理 byte budget 在安装时固定；D3D11 reference 为 32 张、64 MiB，不包含 driver 额外分配。

## 颜色、坐标与 native schema

首版只接收 **linear-sRGB premultiplied RGBA**，用于 LDR 合成；生产者保证有限 `0 ≤ RGB ≤ A ≤ 1`，透明 texel 的 RGB 为零。RGBA8 UNORM、RGBA16F 和 RGBA32F 输入均在真实 GPU 验证。默认 top-left，可显式 bottom-left；UV 为视觉上的左上原点，backend 在 sample 时做整体 Y 翻转。opacity、UV 子矩形和有序混合沿既有 quad 合同。2026-10-11 起显示列表帧也可以画外部纹理（列表里排在资产之后的图片下标），D3D11 适配器把四边形帧同样转成显示列表，bottom-left 改为翻转该条目记录的 uv。

sRGB SRV、straight alpha、encoded-premultiplied Canvas、HDR、BGRA、mipmap、array 和 MSAA 没有被映射成近似输入。ANGLE 的 encoded Canvas 输出后续必须明确转换：先处理 alpha/颜色编码，再生成可过滤的 linear-premultiplied texel，不能在双线性过滤之后才做转换。当前 import 明确返回 unsupported。

[D3D11 schema](../include/weft/render_backend_d3d11.h) 借用有效 `ID3D11ShaderResourceView`；reference 校验其真实 device、view format、全尺寸单 mip Texture2D 和描述尺寸。拒绝 CPU writable、shared/misc flags、array/MSAA；仅接受 DEFAULT/IMMUTABLE。import AddRef；通用层的退休调用 release。encode 在任何命令前检查输入纹理是否同时是当前 RTV，发现 alias 返回 invalid state。D3D11 不允许同一 subresource 同时绑定 SRV/RTV，见 [官方功能规范](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)。

2026-10-12 起 Vulkan 适配器实现了 native import：[Vulkan schema](../include/weft/render_backend_vulkan.h) 借用宿主的 `VkImageView`，校验设备，拒绝同一视图的第二次导入。Vulkan 不能查询视图的属性，格式（线性 RGBA8、RGBA16F，设备能线性过滤时 RGBA32F）、尺寸与用法由宿主保证；宿主写入的命令在同一队列上先提交，把图像留在 `SHADER_READ_ONLY_OPTIMAL` 并让写入对片元着色器可见。`render_vulkan_external_contract` 在 RTX 5090 与 AMD 核显上验证像素、bottom-left、租约退休与拒绝路径，验证层含同步验证零告警。Metal 仍需实现 native import、barrier 和颜色合同；现有 API ID 和模拟合同不能当作移动后端已通过。

## 实际验证与运行

```powershell
cmake --build --preset windows-debug --parallel 10
ctest --preset windows-debug
cmake --build --preset windows-release --parallel 10
ctest --preset windows-release
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --external-texture-hardware artifacts/local/external-texture/release/hardware
# 显式软件 GPU：--external-texture-warp
```

`render_backend_contract` 增加 512 轮 seed `0xCE112026` 的取消/原子失败/部分错误/真实合同提交生命周期压力。验证容量、native import failure/prepare failure、错 instance/epoch/queue、owner/reentry、caller release 与两个 use 独立 pin、旧句柄拒绝、drain、host-confirmed abandon，以及禁止分配时的 submitted/completed。C11 smoke 编译新头与所有新增导出入口。该 fake backend 只验证合同，不代表 Metal GPU。

`d3d11_warp_external_texture` 和显式 hardware 各运行 48 帧、最多两个提交在途。源由 GPU quad 写入 RGBA8/16F/32F 纹理，入队后直接合成，消费前没有 CPU wait；每帧生成独立 native resource/version，宿主原 view 和调用方/prepared 在 submit 前释放。实际 query 完成后依次退休，较早 serial 不能退休较晚纹理。另测同纹理重复发布、foreign device、sRGB view、input/RTV alias、命令取消和录制失败。

最终 Debug **68/68、74.25 秒**，Release **68/68、27.12 秒**；两配置各执行 hardware 与 WARP。收尾复查将混合 CPU coverage 依赖接到现有两帧 cache 路径，实际 drain 后再退休 24 张 CPU texture；之前两配置全量亦通过，日志独立保留。

两条最终目标分别为 sRGB RGBA8 和 linear RGBA32F，在 24 轮 mutation 中验证 top/bottom origin、缩放、UV 子矩形、透明混合顺序、CPU coverage/external 混合索引。每 run **1,572,864 个独立 CPU bilinear/混合像素**。WARP 最大 sRGB 差 3 色阶 / linear 误差 0.00263887；RTX 5090 为 2 / 0.00331104。阈值显式为 3 色阶和 linear `1/256`；非逐像素一致。D3D11 采样坐标和过滤权重存在有限 subtexel precision，见 [官方采样精度合同](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#TextureAddressingAndLODPrecision)。初始探针的 sRGB 2/linear 0.003 假设过严，改为这项明确边界；旧页面阈值未改。调试层发现的 producer sampler 缺失和重复覆盖 query 已修复，最终 warning/error 为零。

报告/四张预览 PNG/日志在 `artifacts/local/external-texture/{debug,release}/`，WARP CTest 报告复制到 `ctest-results/warp`，源/产物/可运行二进制 hash 和只读原游戏 drift 封存在 `run-metadata.json`。机器为 Windows 11 26200 / Ryzen 9 9950X3D / RTX 5090 / MSVC 19.42，画幅 256×128，源纹理同幅、无字体、一个 CPU coverage texel；这是正确性压力，未测启动尖峰、CPU/GPU 性能、内存或实体 4K viewport。

本轮没有修改 DOM/JS，因此未重跑强制 GC；普通全量仍覆盖既有 Canvas/React/DOM。没有修改原游戏、原包或固定 gallery 资产，也没有新增 gallery 案例验收。下一步固定 ANGLE 来源/构建/许可证，先打通 WebGL2 clear/triangle、context 生命周期与外部纹理转换，再接原样 Three/gallery。
