# 系统架构与渲染管线

本文档说明 Weft 的整体分层架构、线程所有权模型、单帧执行链路、增量更新机制以及 GPU 资源生命周期约束。

## 设计原则

Weft 围绕游戏引擎嵌入场景确立了四项核心架构原则：

1. **宿主掌控调度与线程**：Weft 不拥有窗口消息循环，也不在内部私自创建主渲染线程。逻辑更新由宿主在指定线程上显式推进，命令录制由宿主在渲染线程上显式触发。
2. **纯 C ABI 边界与零符号污染**：所有对外能力通过 [`include/weft/`](../include/weft) 暴露为稳定布局的 C 结构体与函数。集成至大型引擎（如 Unreal Engine、Unity）时，内部依赖（FreeType、HarfBuzz、Yoga、QuickJS-ng 等）可完全隐藏在独立动态库内部，避免与引擎自带版本产生符号冲突。
3. **默认增量、语义等价**：样式计算、布局求解、文字塑形与显示列表构建均以增量模式运行，并且通过自动化契约测试保证增量结果与全量重算严格等价。
4. **显式资源与 GPU 生命周期**：所有跨线程帧数据不可变；所有 GPU 上传与渲染资源均通过显式提交序号（serial）与完成回执管理，不存在隐式等待或竞态销毁。

## 分层模块结构

运行时按职责拆分为若干正交模块（均以 `weft::<module>` 静态库组织，并可打包为单一共享库 `weft_shared`）：

| 层级 | 模块 | 核心职责 |
| --- | --- | --- |
| **基础层** | `base`、`core` | 诊断模型、错误码、句柄与代次校验、运行时与 View 注册表 |
| **脚本与桥接层** | `script`、`bridge`、`dom` | QuickJS-ng 运行时托管、DOM 节点与事件派发、双向引擎桥接值编码与消息队列 |
| **样式与布局层** | `style`、`layout` | CSS 解析、选择器匹配与层叠求值、Yoga Flex/绝对定位/网格/流式排版、替换元素尺寸计算 |
| **文本与国际化层** | `text`、`platform_*` | HarfBuzz 塑形、FreeType 栅格化、SheenBidi 双向算法与文字系统切分、各平台系统字体发现（DirectWrite / CoreText / Android NDK） |
| **场景与输入层** | `scene`、`input` | 增量场景求解、动画与过渡推进、图集管理、显示列表生成、指针/触控/键盘/IME/手柄/空间导航与命中测试 |
| **渲染与后端层** | `render`、`scene_{d3d11,d3d12,vulkan,metal}` | 通用渲染器状态机、外部纹理导入与导出扩展、原生图形后端命令录制 |
| **开发工具层（可选）** | `devtools` | Chrome DevTools Protocol 服务端与热重载支持（仅在 `WEFT_DEVTOOLS=ON` 时编译） |

## 线程模型与所有权

Weft 将跨线程边界收口为清晰的**双线程归属模型**：

- **逻辑宿主线程（Runtime Owner Thread）**：
  - 创建 `weft_runtime` 的线程即为该运行时的唯一所有者线程。
  - 所有 `weft_runtime_*` 与 `weft_view_*`（除 `weft_result_string` 与显式声明线程安全的帧释放外）均必须在所有者线程上调用；跨线程误用将直接返回 `WEFT_WRONG_THREAD`。
  - 当页面运行 React、D3 等深调用栈前端库时，建议所有者线程栈空间不少于 **8 MiB**。
- **渲染宿主线程（Renderer Owner Thread）**：
  - 创建 `weft_renderer` 的线程拥有该渲染器及其底层图形后端适配器。
  - `weft_renderer_prepare_scene`、`weft_renderer_encode`、`weft_renderer_notify_submitted`、`weft_renderer_notify_completed` 等渲染接口均在该线程上顺序执行，无内部互斥锁竞争。
  - 在任务化渲染架构（如 UE5 RHI 任务线程）中，宿主可将渲染调用串行派发至固定的专用轻量渲染宿主线程，再同步回当前命令列表录制位置。
- **跨线程帧快照（`weft_view_frame`）**：
  - `weft_view_acquire_frame` 返回一个不可变的引用计数帧快照，可安全跨线程传递至渲染线程。
  - 渲染线程完成 `weft_renderer_prepare_scene` 后即可调用 `weft_view_frame_release` 释放逻辑帧句柄，底层资源由渲染器按 GPU 完成序号接管。

## 单帧执行顺序

在宿主每帧调用 `weft_view_advance(runtime, view, time_seconds)` 时，Weft 遵循与浏览器一致的确定性帧阶段推进：

1. **时间戳校验**：校验单调递增时钟（时间回退返回 `WEFT_TIME_REGRESSION`），驱动内部动画时间线与 `performance.now()`。
2. **宿主数据与输入交付**：
   - 将上一帧以来写入的视图模型（`weft_view_set_model`）、引擎事件（`weft_view_emit`）与异步调用回执（`weft_view_complete_call`）按顺序交付页面脚本。
   - 刷新微任务队列，推进到期的定时器（`setTimeout` / `setInterval`）与惯性滚动/触控回弹。
3. **图像加载与帧回调**：完成待决图像解码与 `load` / `error` 派发，依次执行 `requestAnimationFrame` 回调及 Web Animations 状态结算。
4. **增量样式、布局与场景求解**：
   - 仅对脏节点重算匹配规则与计算样式，检测变化属性并按需触发 Yoga 局部重排与行内文字重塑形。
   - 推进活跃的 CSS Animations、CSS Transitions、Web Animations 及 UI 粒子状态。
   - 合并新写入的 3D 世界锚点坐标（`weft_view_write_anchors`），构建或复用当前帧显示列表。
5. **发布不可变帧快照**：若本帧存在视觉变更，生成新的不可变 `weft_view_frame`；若界面静止且无动画，渲染端可直接跳过冗余上传与重绘。

## 增量更新与缓存策略

面向 60–240 FPS 实时游戏场景，Weft 在渲染与场景求解链路中引入了多层缓存：

- **样式与布局脏标记传播**：DOM 属性、类名、内联样式或 `@media` 条件变更仅标记受影响子树，未变化分支保留既有行内上下文与布局盒。
- **字形与路径图集（Atlas）**：高频文本字形、圆角阴影与矢量路径栅格化结果按内容键驻留在动态图集页中，跨帧复用，避免每帧重复上传小纹理。
- **零脚本锚点更新**：带有 `data-anchor="<key>"` 标记的血条、名牌、伤害飘字等随场景移动的 UI 元素，其屏幕变换直接在显示列表与渲染阶段注入，不触发任何 JavaScript 执行、样式重算或布局重排。
- **内容哈希与局部脏区**：显示列表项与渲染后端按内容键缓存 GPU 侧资源；静态面板在未变更帧上无需重建顶点与常量缓冲。

## GPU 生命周期与绘制管线

Weft 的渲染核心采用**四阶段显式帧状态机**：

1. **Prepare（准备）**：`weft_renderer_prepare_scene` 消费不可变帧快照与外部纹理句柄，完成资源依赖绑定与校验，返回准备帧句柄。
2. **Encode（录制）**：`weft_renderer_encode` 在宿主提供的目标视图与命令列表上录制绘制指令（支持圆角裁剪链、离屏合成组、`backdrop-filter` 背景拷贝、自定义着色器效果与遮罩合成），返回本次录制的使用句柄（`use`）。
3. **Submit / Discard（提交或丢弃）**：
   - 宿主将命令列表提交至 GPU 队列后，调用 `weft_renderer_notify_submitted(renderer, use, queue, serial)` 绑定单调递增的提交序号。
   - 若宿主取消该命令列表（未提交 GPU），调用 `weft_renderer_discard_encoded` 立即归还引用。
4. **Complete（完成回收）**：当 GPU Fence 推进至某序号时，宿主调用 `weft_renderer_notify_completed(renderer, device_epoch, queue, serial)`，渲染器据此安全回收该序号之前所有已退休的临时缓冲、纹理与外部资源租约。

### 色彩空间与合成契约

- 场景内部遵循浏览器合成规范，默认支持 `SRGB_RGBA8`、`SRGB_BGRA8`、`LINEAR_RGBA8`、`LINEAR_BGRA8`、`LINEAR_RGBA16F` 及 `LINEAR_RGB10A2`（如 UE5 默认 10-bit 后缓冲）等多种目标格式。
- 宿主导入的外部纹理与导出给 3D 世界的游戏内 UI 纹理统一约定为**线性预乘 RGBA（Linear Premultiplied）**，可直接与引擎 HDR 渲染管线及硬件 Mipmap 过滤无缝衔接，不产生半透明黑边。

## 文本与国际化排版管线

Weft 的文字系统完全基于原生 C/C++ 库构建，不依赖浏览器壳：

1. **双向与文字系统切分**：采用 **SheenBidi** 实现 Unicode 17 双向算法（UAX #9）与文字系统切分（UAX #24），支持 `direction`、`unicode-bidi`、`dir="auto"` 及 `<bdi>` / `<bdo>`。
2. **字形塑形与多级字体回退**：使用 **HarfBuzz** 按段落方向与文字系统进行连写、合体字与复杂脚本塑形；按 `font-family` → 脚本专属回退字体 → 全局回退字体逐级匹配，支持 `@font-face` 与 `unicode-range` 复合字体配置。
3. **换行与栅格化**：通过 **libunibreak** 实现标准 Unicode 换行，由 **FreeType** 按当前设备缩放比（`device_scale`）栅格化高精度覆写率图集，确保高 DPI 与 UI 缩放下文字锐利清晰。
