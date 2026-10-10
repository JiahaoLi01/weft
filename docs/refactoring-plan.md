# 架构重构计划

更新日期：2026 年 10 月 10 日。本文是 M0 探针代码走向可维护产品结构的计划，不是已实现能力的证据。第 0 步（代码规范与命名常量，见 [代码规范](coding-style.md)）已完成；其余步骤按风险从低到高排列，每步单独验证、可单独回退。

## 现状诊断

以下结论来自对 `src/`、`include/`、`probes/`、`tests/` 的只读调研，均可在源码中定位。

1. **命名与成熟度混在一起。** 产品依赖的代码也叫 `*_probe`，公开类型普遍带 `Probe` 前缀（`ProbeDomSnapshot`、`ProbeNodeIdentity`）。线程核心在 `owner_executor_probe.hpp`。产品模块与证据探针没有边界，CMake 是一个 565 行文件，`/STACK:8388608` 重复 13 次。
2. **平台代码渗入可移植层。**
   - `scene_probe.hpp` 直接暴露 `ID2D1RenderTarget`；scene 的几何与命中实现在 D2D 翻译单元里。
   - scene 布局用 DirectWrite 量字，flow/page 用 HarfBuzz/FreeType，两套文本栈并存。scene 因此离不开 Windows，UI owner 线程也必须初始化 COM。
3. **DOM 寄生在脚本层。** 节点边是 `JSValue`，节点直接持有 Canvas 状态，DOM 自带一套选择器引擎，与 style 的选择器重复。
4. **可复用运行时放在 `probes/`。**
   - D3D11 渲染器（`Experiment`）、`ce_render_backend` D3D11 适配器、`TwoFramePipeline`、`SceneNativeGpu`、窗口宿主都在探针里。
   - 探针头不自包含，在 `d3d11_probe.cpp` 中途被 include；测试反向依赖探针头。
   - JS bootstrap 运行时经 `CE_SOURCE_DIR` 从源码目录读取，无法随移动端/主机包发布。
5. **五套页面 profile 重复实现同一件事。** page、positioned、auto、flow、scene 各自实现 CSS 标量解析（`fail/trim/get/number/length/split/color` 各 4–9 份）。各份的数值上限互相矛盾（1e5、1e6、1e7）。此外：
   - transform 解析 3 份、paint 发射 4 份、命中测试 3 份；
   - 代际 slot 表 4 份；
   - 绘制 IR 5 种。
6. **样式全程是字符串。**
   - 计算值是 `map<string,string>`，布局每次重新解析字符串，再把数字写回字符串。
   - 动画插值靠正则切分数字串。
   - 快照每帧深拷贝。
7. **错误模型。** 153 个 `"CE_*"` 字符串错误码，没有枚举；控制流靠前缀匹配（例如把 `CE_STYLE_` 改包装）。另有 30 处 `catch(...)`，部分会静默吞错。
8. **上帝对象与函数。**
   - `ScriptDomProbe::Impl` 约 2500 行。
   - `SceneRasterProbe::Impl` 约 1750 行，带约 15 个临时缓存。
   - `run_gallery_preview` 约 1370 行。
   - 探针 `main` 有 21 个模式布尔量。
9. **多态方式不统一。** 只有 Canvas raster 用 virtual。宿主回调、文本测量和级联里的 `var()` 解析用 `std::function`（后者在热路径上）。渲染后端用 C 函数表，没有 concept 约束。

## 目标结构

```
include/caseelephant/      版本化 C ABI（ce.h、render_backend.h…）与公开 C++ 外观
src/base/                  诊断码与 expected、代际 slot 表、owner 线程检查、标量解析、constants
src/dom/                   原生 DOM：NodeId、树、属性、mutation journal、选择器
src/css/                   解析、PropertyId 与类型化值、级联、计算样式、动画/过渡
src/text/                  HarfBuzz/FreeType/libunibreak 塑形、回退、字形缓存
src/layout/                格式上下文调度：block/inline/flex(Yoga)/grid/absolute，几何与命中
src/paint/                 唯一的 display list、damage、隔离组
src/render/                FramePacket、资源退休、后端 concept；backends/{d3d11,d3d12,vulkan,metal,gles}
src/script/                QuickJS 绑定：DOM 包装、事件循环、timer/rAF、内嵌 bootstrap JS
src/platform/              win32/android/ios/console：窗口、输入、时钟、字体发现
probes/ tests/ tools/      证据与验证，只依赖 src，不被 src 依赖
```

静多态的边界：

- **内部用 concept 约束模板。**
  - `RenderBackend`、`TextShaper`/`FontProvider`、`Platform`（时钟、线程、文件、COM 等初始化）各自定义为 concept。
  - style profile 改为策略类型，不再用布尔开关。
  - owner 线程检查做成 CRTP mixin。
- **唯一的动态分派在 C ABI。** 用 `make_c_backend<B>()` 为每个后端生成 thunk 表，宿主引擎仍只见到稳定的函数表。热路径中的 `std::function` 换成模板参数或 `function_ref`。

## 迁移步骤

| 步骤 | 内容 | 主要文件 | 验证方式 |
| --- | --- | --- | --- |
| 0 ✅ | 格式规范、一行一声明、命名常量 | 全部 C++/HLSL | Release 反汇编逐函数等价、着色器字节码等价、全量测试 |
| 1 ✅ | `src/base`：统一 CSS 标量解析、`enum class Diag` + `Result<T>`、代际 slot 表、`OwnerThread` | `src/layout/*`、`scene_*`、`style_probe`、`canvas_probe`、`runtime`、`render_backend`、`resource_lifetime` | 旧实现逐字副本随机差分、各 profile 上限保留、全量测试不变；见下方第 1 步记录 |
| 2 ✅ | `tests/support`：check 宏、文件/PNG 工具；`host_time`、`window_frame_rate`、`gallery_owner_trace` 移出探针 | `tests/*`、`probes/*` | 全量测试；见下方第 2 步记录 |
| 3 ✅ | bootstrap JS 由 CMake 生成 `.inc` 内嵌，去掉运行时 `CE_SOURCE_DIR` | `script_dom_probe.cpp`、各窗口/探针 | 脚本与 GC 测试；移动/主机打包前置条件；见下方第 3 步记录 |
| 4 | CMake 拆成模块目录与 `ce::` 目标，`ce_add_test()`；公开/私有头分离；去 `Probe` 前缀 | `CMakeLists.txt`、`include/` | 全量构建与测试，C ABI smoke |
| 5 | D3D11 渲染器、FramePacket、两帧流水线移入 `src/render`，经 `RenderBackend` concept 接 C ABI；探针头改为独立 TU | `probes/d3d11_probe.cpp`、`probe_render_backend.hpp`、`two_frame_pipeline.hpp`、`scene_native_gpu_probe.hpp` | WARP/硬件像素、两帧退休、debug layer 零告警 |
| 6 | scene 可移植化：拆出纯几何/命中 TU；DirectWrite 量字换成与 flow 相同的 HarfBuzz 测量；D2D 退为 Windows 参考 raster | `scene_probe.hpp`、`scene_d2d_probe.cpp`、`gallery_scene_session.hpp` | 字体度量会变，重新建立 Chrome 对照基线（已确认） |
| 7 | 类型化样式：PropertyId、长度/颜色/关键字值，去掉字符串往返与正则插值 | `style_probe`、`flow_page`、`auto_page`、`scene_probe` | 级联/动画差分、gallery 像素与几何 |
| 8 | 统一格式化路径：一个调度器、一个 Yoga 适配器、一个 paint IR、一条命中路径；golden 迁移后删除 page/positioned/auto | `src/layout/*`、`input_probe` | 旧 profile 的浏览器差分全部改跑新路径 |
| 9 | DOM 与脚本分离：NodeId 原生树、与 style 共用选择器；QuickJS 对象成为包装；Canvas 状态移到旁表 | `script_dom_probe.cpp`、`style_probe.cpp` | DOM 生命周期、GC 压力、D3 / React 原包 |
| 10 | 第二个图形后端（Vulkan 或 Metal）与 Android/iOS 平台层，按 concept 接入；主机构建选项 | `src/render/backends`、`src/platform` | 平台验证按“配置/编译/链接/模拟器/真机功能/真机性能”分级记录 |

## 第 1 步记录（2026-10-10）

`caseelephant_base`（`include/caseelephant/base/`、`src/base/`）提供以下内容：

- **`diagnostic.hpp`**：`Diag` 枚举、`Diagnostic`，以及过渡用的 `DiagnosticError`。`DiagnosticError` 继承 `std::invalid_argument`，消息仍以原 `CE_*` 名称开头。
- **`result.hpp`**：`Result<T>`/`Status`。C++20 没有 `std::expected`，这里只实现运行时需要的部分。
- **`css_scalar.hpp`**：`trim`、`style_value`、可配置嵌套的 `split`、`split_unit`、沿用 `std::stof`/`std::stod` 文法的 `parse_number`、颜色简写展开。
- **`rect.hpp`**：布局矩形与求交。
- **`generational_slots.hpp`**：代际槽表。
- **`owner_thread.hpp`**：owner 线程检查。

迁移结果：

- **标量解析**：flex、page、positioned、auto、flow、scene、scene native/paint、D2D raster、style、canvas 不再各自持有 trim/split/number/length/color/intersect 副本。各 profile 保留自己的数值上限（1e5/1e6/1e7/无上限）和诊断文本。原先直接漏出的 `std::stof`/`std::stod` 异常，现在一律报告为该 profile 的诊断。
- **按错误码分派**：`positioned_page_probe` 不再匹配 `CE_STYLE_` 前缀，canvas 绑定也不再匹配 `CE_CANVAS_FILL_RULE`/`IndexSizeError` 前缀，两处都改为按 `Diag` 分派。
- **代际槽表**：runtime 视图、renderer prepared/use/外部纹理、resource lifetime 的三张表、脚本 DOM 节点身份，共用 `GenerationalSlots`。句柄序列不变：首次占用第 1 代，代际耗尽的槽位退役。
- **owner 检查**：8 个文件的 owner 线程检查改为组合 `OwnerThread` 值成员。计划中写的是 CRTP mixin，组合更简单，效果相同。

验证：

- **差分测试**：`tests/base_tests.cpp` 用被替换实现的逐字副本做随机差分，覆盖 2 万条拆分输入（3 种分隔符 × 3 种变体）和 4 万条数值输入（float/double），另有 Result、诊断、槽表退役与跨线程检查。
- **全量测试与性能**：结果见 [status](status.md)。

尚未迁移：

- owner executor、视图故障、节点上限等其余 `CE_*` 字符串错误码，随各自模块改造时迁入 `Diag`。
- 异常到 `Result` 的全面替换按模块逐步进行。

## 第 2 步记录（2026-10-10）

`tests/support/` 是测试与探针共用的头文件库，命名空间为 `caseelephant::testing`：

| 文件 | 内容 |
| --- | --- |
| `check.hpp` | `require` 与 `CE_REQUIRE`（失败时报出表达式和位置） |
| `files.hpp` | 原样字节读取、文本模式读取、写入，以及相对源码根的读取 |
| `json.hpp` | `json_string` |
| `png.hpp` | WIC PNG 读写与 `check_hresult` |
| `host_time.hpp` | `format_host_time`，原 `probe_host_time` |
| `frame_rate.hpp` | `WindowFrameRate` |
| `owner_trace.hpp` | `OwnerTrace`/`TraceControl`，原 `GalleryOwnerTrace`/`GalleryProfileControl` |

它们的常量放在 `tests/test_constants/support.hpp`。

迁移内容：

- **断言与读文件**：22 个测试文件里的 `require`/`check` 和 14 份读文件函数不再各写一份，每个调用点保留原来的读取方式（相对源码根还是给定路径、字节还是文本模式）。两个宏断言测试改为基于 `CE_REQUIRE`，且不在成功路径分配内存。
- **探针侧**：`json_string`、`read_text`、`checked`、PNG 编解码改用共享实现，共替换 7 份副本（4 份解码、3 份编码）。`d3d11_probe.cpp` 在全局作用域先包含全部 support 头，再在匿名 namespace 里用 using 声明接入。
- **头文件自包含**：`scene_visual_compare.hpp`、`cluster_visual_compare.hpp`、`scene_test_image.hpp` 和 owner trace，不再依赖包含方预先定义 `require`、`hr`、`ComPtr`、`json_string`。之前测试里伪造的不转义 `json_string` 已删除。
- **移出探针**：`probes/host_time.hpp`、`window_frame_rate.hpp`、`gallery_owner_trace.hpp` 已删除，测试不再包含这三个探针头。

仍然存在的依赖：四个 scene 测试仍包含 `probes/gallery_scene_session.hpp`/`gallery_cluster_input.hpp`。这套 gallery 宿主包装了 D2D raster 与 DirectWrite 测量，留到第 5、6 步随 D3D11 抽取和 scene 可移植化一起迁出。

## 第 3 步记录（2026-10-10）

三个 owner 平台模块从 `probes/` 移到 `src/script/js/`：
- `web_host_bootstrap.js`：DOM 构造器、CSSOM、计时器、微任务、console；
- `animation_frame_bootstrap.js`：rAF 批次；
- `scene_platform_bootstrap.js`：SVG/Element、窗口事件、几何读取、ResizeObserver、matchMedia。

构建时由 `cmake/embed_bytes.cmake` 生成 `build/<preset>/generated/caseelephant/script_js/*.js.inc`，编进 `caseelephant_script`。生成规则如下：

- **换行**：CRLF 统一为 LF，内嵌文本不随 checkout 方式变化。
- **字节表**：用无符号字节表，不受 MSVC 单个字符串字面量长度上限的约束。
- **增量**：内容不变时不改写输出文件，避免无谓重编。

公开接口 `include/caseelephant/script_platform.hpp` 提供三个函数：`script_platform_source`、`script_platform_file`、`install_script_platform(vm, module)`。运行时库因此不再需要源码树。

迁移与测试：

- **调用点**：测试和探针里 37 处 `CE_SOURCE_DIR "/probes/*_bootstrap.js"` 读取改为调用 `install_script_platform`。`owner_hotpath_tests` 的旧版本差分仍从 fixture 读取优化前的两个模块。
- **新增测试**：`script_dom_lifecycle` 校验内嵌文本与源文件一致，三个模块无需文件访问即可按序安装。

仍然保留的源码树依赖：浏览器参考时钟（`probes/*_reference_clock.js`，供浏览器采集工具使用，路径被 fixture 清单锁定）、测试 fixture、字体和 gallery 原样 bundle。它们都只属于测试与证据，不属于运行时。
## 已确认的取舍（2026-10-10）

用户已确认以下四项，依据与修改条件记在 [关键决策](decisions.md)。

1. **静多态边界放在 C ABI 内侧。** 引擎内部全部用模板/concept，ABI 处为每个后端生成函数表。运行时性能优先于编译时间；同一二进制带多个后端时各自实例化。
2. **核心路径从异常迁到 `expected<T, Diag>`。** UI 的局部错误不能让游戏进程终止：错误作为值返回，按 View/节点降级并进入诊断，由宿主决定处理方式。这也满足主机平台常关闭异常和 RTTI 的约束。第 1 步引入 `Diag` 并去掉前缀匹配，其余模块逐步迁移。
3. **退役 page/positioned/auto 三个历史 profile。** 先让它们的浏览器对照全部跑在统一路径上，再删除旧代码。
4. **scene 量字改用 HarfBuzz/FreeType。** Windows 依赖违背跨平台需求；D2D/DirectWrite 只保留为 Windows 参考 raster。现有 102 张 Chrome 对照图依赖 DirectWrite 度量，在第 6 步重新采集基线并说明差异。
