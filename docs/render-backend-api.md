# 渲染后端集成指南

Weft 的渲染架构围绕**宿主拥有 GPU 设备与命令提交权**设计。本文档说明通用渲染器状态机（[`include/weft/render_backend.h`](../include/weft/render_backend.h)）、内置图形后端的接入方式以及自定义渲染后端的扩展规范。

## 1. 架构边界

Weft 的渲染层分为三个解耦层级：

1. **核心渲染状态机（`weft_renderer`）**：负责校验线程与句柄代次、管理准备帧（Prepared Frame）与在途提交（In-Flight Use）预算、追踪外部纹理与导出纹理租约，并在 GPU 完成序号到达时触发资源回收。
2. **内置原生后端适配器**：分别针对 **D3D11**、**D3D12**、**Vulkan** 与 **Metal** 实现着色器管线、图集上传、离屏组合成、背景拷贝（`backdrop-filter` / 背景着色器）以及命令列表录制。
3. **宿主集成层**：向 Weft 传入宿主已有的 GPU 设备、目标视图与当前帧命令列表，并在队列提交与 GPU Fence 完成时通知 Weft。

## 2. 渲染器标准帧循环

以下展示宿主渲染线程对接任意后端的通用生命周期：

```c
// 1. 初始化阶段：创建原生后端与通用渲染器
weft_render_backend backend_table = {0};
weft_render_d3d12* d3d12_backend = NULL;
weft_render_d3d12_create(&d3d12_desc, &d3d12_backend, &backend_table);

weft_renderer* renderer = NULL;
weft_renderer_create(&backend_table, NULL, &renderer);

// 可选：安装外部纹理导入扩展 / 游戏内 UI 导出扩展
weft_render_external_backend ext_table = {0};
weft_render_d3d12_external(d3d12_backend, &ext_table);
weft_renderer_install_external_backend(renderer, &ext_table);

// 2. 每帧渲染阶段
weft_view_frame* view_frame = NULL;
weft_view_acquire_frame(runtime, view, &view_frame);

weft_render_handle prepared = {0};
weft_render_scene_frame scene_frame = {
    .struct_size = sizeof(scene_frame),
    .abi_version = WEFT_RENDER_ABI_VERSION,
    .frame = view_frame,
    .scale = 1.0f,
    .texture_count = bound_texture_count,
    .textures = bound_textures,
};
weft_renderer_prepare_scene(renderer, &scene_frame, &prepared);
weft_view_frame_release(view_frame); // Prepare 成功后即可释放逻辑帧引用

// 录制到宿主当前命令列表（native_pass 指向对应后端的 pass 描述）
weft_render_handle use = {0};
weft_renderer_encode(renderer, prepared, &pass_desc, &use);
weft_renderer_release_prepared(renderer, prepared);

// 3. 宿主提交命令列表至 GPU 队列后通知提交序号
weft_renderer_notify_submitted(renderer, use, queue_id, submit_serial);

// 4. GPU Fence 完成对应序号后通知回收内部缓冲与纹理租约
weft_renderer_notify_completed(renderer, device_epoch, queue_id, completed_serial);
```

### 关键同步与错误处理约束

- **放弃未提交录制**：若宿主在 `weft_renderer_encode` 之后决定取消该命令列表且不提交 GPU，必须调用 `weft_renderer_discard_encoded(renderer, use)` 归还资源占用。
- **失败诊断**：当 `prepare` 或 `encode` 返回错误时，宿主可通过 `weft_renderer_last_failure` 获取详细的人类可读错误原因并记入引擎日志。
- **无分配热路径**：`weft_renderer_notify_submitted` 与 `weft_renderer_notify_completed` 均为无失败、零堆分配热路径，追踪槽位在初始化阶段按预算一次性预分配。
- **安全关闭与设备丢失（Device Lost）**：
  - 正常退出时先等待所有在途 GPU 序号完成，再依次释放外部纹理租约并调用 `weft_renderer_destroy`。
  - 发生设备重置或移除时，在宿主确认旧设备不再访问任何资源后，调用 `weft_renderer_abandon_device` 立即解除未决序号等待并执行资源清理。

## 3. 内置图形后端一览

| 后端 | 头文件 | 创建入口 | 扩展支持 | 典型目标格式 |
| --- | --- | --- | --- | --- |
| **Direct3D 11** | `render_backend_d3d11.h` | `weft_render_d3d11_create` | 外部纹理导入 | `LINEAR_BGRA8`、`LINEAR_RGBA8`、`SRGB_RGBA8`、`SRGB_BGRA8` |
| **Direct3D 12** | `render_backend_d3d12.h` | `weft_render_d3d12_create` | 外部纹理导入 | `LINEAR_BGRA8`、`LINEAR_RGBA8`、`LINEAR_RGB10A2`、`SRGB_*` |
| **Vulkan** | `render_backend_vulkan.h` | `weft_render_vulkan_create` | 外部纹理导入 | `LINEAR_BGRA8`、`LINEAR_RGBA8`、`LINEAR_RGBA16F` |
| **Metal** | `render_backend_metal.h` | `weft_render_metal_create` | 外部纹理导入 + 3D 导出纹理 | `LINEAR_BGRA8`、`LINEAR_RGBA8`、`LINEAR_RGB10A2`、`SRGB_*`、`LINEAR_RGBA16F` |

### 后端接入要点

- **Direct3D 12（如 Unreal Engine 5 RDG / RHI）**：
  - 直接复用宿主 `ID3D12Device` 并向宿主 `ID3D12GraphicsCommandList` 录制渲染指令。
  - 在 RDG Pass 内调用前先冲刷引擎挂起的资源屏障；录制完成后通知引擎重置命令列表状态并绑定手动 Fence，即可实现零拷贝直接合成到后缓冲（包括 UE5 默认的 `PF_A2B10G10R10`）。
- **Vulkan**：
  - 运行时通过宿主提供的 Vulkan 加载函数指针与逻辑设备运行，不强制静态链接系统 Vulkan SDK 库；支持 Windows 与 Android 平台。
  - 宿主需确保外部实时纹理在 UI 采样前已处于着色器只读布局并设置相应管线屏障。
- **Metal（macOS / iOS）**：
  - 共享 HLSL 着色器在构建期自动编译为原生 Metal Shading Language（MSL），不依赖 MoltenVK 翻译层。
  - 针对 Apple TBDR 分块架构优化了 Pass 切分与 `clear_target` 行为，并在命令编码器上标注了清晰标签（`weft out`、`weft group`、`weft uploads` 等），可直接在 Xcode Metal System Trace 与 GPU Frame Capture 中定位耗时。

## 4. 外部纹理导入扩展

头文件 [`include/weft/render_external_texture.h`](../include/weft/render_external_texture.h) 定义了宿主 GPU 纹理零拷贝接入 UI 合成的扩展契约：

- **两种同步模式**：
  - `WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED`（冻结纹理）：导入后内容不可变，适合动态图标或轮换贴图。
  - `WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE`（实时渲染目标）：允许引擎在两帧 UI 绘制之间原地重绘同一张 `RenderTarget`（如小地图、实时场景捕获），只要写操作与 UI 录制在同一队列上保持先后顺序即可，无需每帧反复导入。
- **安全回收**：调用 `weft_renderer_release_texture` 仅终止宿主句柄租约，底层原生纹理引用会一直维持到最后一次采样它的 GPU 提交完成后，再通过 `retired` 回调交还宿主销毁。

## 5. 实现自定义渲染后端

对于主机平台（PS5 / Xbox / Switch）或自研专有图形抽象层，引擎方可直接实现 `weft_render_backend` 函数表接入 Weft：

- 将 `backend.api` 设置为不小于 `WEFT_RENDER_API_CUSTOM_BEGIN`（`1024`）的自定义标识。
- 实现 `prepare_scene`、`release_prepared`、`encode`、`discard_encoded`、`submitted`、`completed`、`abandon` 与 `shutdown` 回调。
- 核心渲染状态机将自动为自定义后端提供完整的句柄校验、帧配额保护、线程检查与 GPU 序号生命周期管理。
