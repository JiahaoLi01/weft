# 架构重构计划

更新日期：2026 年 10 月 10 日。本文是 M0 探针代码走向可维护产品结构的计划，不是已实现能力的证据。第 0 步（代码规范与命名常量，见 [代码规范](coding-style.md)）已完成；其余步骤按风险从低到高排列，每步单独验证、可单独回退。

## 现状诊断

以下结论来自对 `src/`、`include/`、`probes/`、`tests/` 的只读调研，均可在源码中定位。

1. **命名与成熟度混在一起。** 产品依赖的代码也叫 `*_probe`，公开类型普遍带 `Probe` 前缀（`DomSnapshot`、`NodeIdentity`）。线程核心在 `owner_executor_probe.hpp`。产品模块与证据探针没有边界，CMake 是一个 565 行文件，`/STACK:8388608` 重复 13 次。
2. **平台代码渗入可移植层。**
   - `scene_probe.hpp` 直接暴露 `ID2D1RenderTarget`；scene 的几何与命中实现在 D2D 翻译单元里。
   - scene 布局用 DirectWrite 量字，flow/page 用 HarfBuzz/FreeType，两套文本栈并存。scene 因此离不开 Windows，UI owner 线程也必须初始化 COM。
3. **DOM 寄生在脚本层。** 节点边是 `JSValue`，节点直接持有 Canvas 状态，DOM 自带一套选择器引擎，与 style 的选择器重复。
4. **可复用运行时放在 `probes/`。**
   - D3D11 渲染器（`Experiment`）、`weft_render_backend` D3D11 适配器、`TwoFramePipeline`、`SceneNativeGpu`、窗口宿主都在探针里。
   - 探针头不自包含，在 `d3d11_probe.cpp` 中途被 include；测试反向依赖探针头。
   - JS bootstrap 运行时经 `WEFT_SOURCE_DIR` 从源码目录读取，无法随移动端/主机包发布。
5. **五套页面 profile 重复实现同一件事。** page、positioned、auto、flow、scene 各自实现 CSS 标量解析（`fail/trim/get/number/length/split/color` 各 4–9 份）。各份的数值上限互相矛盾（1e5、1e6、1e7）。此外：
   - transform 解析 3 份、paint 发射 4 份、命中测试 3 份；
   - 代际 slot 表 4 份；
   - 绘制 IR 5 种。
6. **样式全程是字符串。**
   - 计算值是 `map<string,string>`，布局每次重新解析字符串，再把数字写回字符串。
   - 动画插值靠正则切分数字串。
   - 快照每帧深拷贝。
7. **错误模型。** 153 个 `"WEFT_*"` 字符串错误码，没有枚举；控制流靠前缀匹配（例如把 `WEFT_STYLE_` 改包装）。另有 30 处 `catch(...)`，部分会静默吞错。
8. **上帝对象与函数。**
   - `ScriptDom::Impl` 约 2500 行。
   - `SceneRaster::Impl` 约 1750 行，带约 15 个临时缓存。
   - `run_gallery_preview` 约 1370 行。
   - 探针 `main` 有 21 个模式布尔量。
9. **多态方式不统一。** 只有 Canvas raster 用 virtual。宿主回调、文本测量和级联里的 `var()` 解析用 `std::function`（后者在热路径上）。渲染后端用 C 函数表，没有 concept 约束。

## 目标结构

```
include/weft/              版本化 C ABI（weft.h、render_backend.h…）与公开 C++ 外观
src/base/                  诊断码与 expected、代际 slot 表、owner 线程检查、标量解析
src/constants/             命名常量（仅头文件）
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
| 3 ✅ | bootstrap JS 由 CMake 生成 `.inc` 内嵌，去掉运行时 `WEFT_SOURCE_DIR` | `script_dom_probe.cpp`、各窗口/探针 | 脚本与 GC 测试；移动/主机打包前置条件；见下方第 3 步记录 |
| 4 ✅ | 改名 Weft；CMake 拆成模块目录与 `weft::` 目标，`weft_add_test()`；公开/私有头分离；去 `Probe` 前缀 | `CMakeLists.txt`、`include/`、全部源码 | 测试定义与编译选项逐项对照、全量构建与测试、C ABI smoke；见下方第 4 步记录 |
| 5 ✅ | D3D11 渲染器、FramePacket、两帧流水线移入 `src/render`，经 `RenderBackend` concept 接 C ABI；探针头改为独立 TU；shader 构建期编译内嵌 | `probes/d3d11_probe.cpp`、`probe_render_backend.hpp`、`two_frame_pipeline.hpp` | WARP/硬件产物逐字节对照、两帧退休、debug layer 零告警；见下方第 5 步记录 |
| 6 ✅ | scene 可移植化：拆出纯几何/命中 TU；DirectWrite 量字换成与 flow 相同的 HarfBuzz 测量；D2D 退为 Windows 参考 raster | `scene_probe.hpp`、`scene_d2d_probe.cpp`、`gallery_scene_session.hpp` | 新增 Chrome 文字度量基线；几何与 D2D 差分；见下方第 6 步记录 |
| 7 | 类型化样式：PropertyId、长度/颜色/关键字值，去掉字符串往返与正则插值 | `style_probe`、`flow_page`、`auto_page`、`scene_probe` | 级联/动画差分、gallery 像素与几何 |
| 8 | 统一格式化路径：一个调度器、一个 Yoga 适配器、一个 paint IR、一条命中路径；golden 迁移后删除 page/positioned/auto | `src/layout/*`、`input_probe` | 旧 profile 的浏览器差分全部改跑新路径 |
| 9 | DOM 与脚本分离：NodeId 原生树、与 style 共用选择器；QuickJS 对象成为包装；Canvas 状态移到旁表 | `script_dom_probe.cpp`、`style_probe.cpp` | DOM 生命周期、GC 压力、D3 / React 原包 |
| 10（暂缓） | 第二个图形后端（Vulkan 或 Metal）与 Android/iOS 平台层，按 concept 接入；主机构建选项 | `src/render/backends`、`src/platform` | 平台验证按“配置/编译/链接/模拟器/真机功能/真机性能”分级记录 |

## 第 1 步记录（2026-10-10）

`caseelephant_base`（现为 `weft::base`，位于 `src/base/`）提供以下内容：

- **`diagnostic.hpp`**：`Diag` 枚举、`Diagnostic`，以及过渡用的 `DiagnosticError`。`DiagnosticError` 继承 `std::invalid_argument`，消息仍以原 `WEFT_*` 名称开头。
- **`result.hpp`**：`Result<T>`/`Status`。C++20 没有 `std::expected`，这里只实现运行时需要的部分。
- **`css_scalar.hpp`**：`trim`、`style_value`、可配置嵌套的 `split`、`split_unit`、沿用 `std::stof`/`std::stod` 文法的 `parse_number`、颜色简写展开。
- **`rect.hpp`**：布局矩形与求交。
- **`generational_slots.hpp`**：代际槽表。
- **`owner_thread.hpp`**：owner 线程检查。

迁移结果：

- **标量解析**：flex、page、positioned、auto、flow、scene、scene native/paint、D2D raster、style、canvas 不再各自持有 trim/split/number/length/color/intersect 副本。各 profile 保留自己的数值上限（1e5/1e6/1e7/无上限）和诊断文本。原先直接漏出的 `std::stof`/`std::stod` 异常，现在一律报告为该 profile 的诊断。
- **按错误码分派**：`positioned_page_probe` 不再匹配 `WEFT_STYLE_` 前缀，canvas 绑定也不再匹配 `WEFT_CANVAS_FILL_RULE`/`IndexSizeError` 前缀，两处都改为按 `Diag` 分派。
- **代际槽表**：runtime 视图、renderer prepared/use/外部纹理、resource lifetime 的三张表、脚本 DOM 节点身份，共用 `GenerationalSlots`。句柄序列不变：首次占用第 1 代，代际耗尽的槽位退役。
- **owner 检查**：8 个文件的 owner 线程检查改为组合 `OwnerThread` 值成员。计划中写的是 CRTP mixin，组合更简单，效果相同。

验证：

- **差分测试**：`tests/base_tests.cpp` 用被替换实现的逐字副本做随机差分，覆盖 2 万条拆分输入（3 种分隔符 × 3 种变体）和 4 万条数值输入（float/double），另有 Result、诊断、槽表退役与跨线程检查。
- **全量测试与性能**：结果见 [status](status.md)。

尚未迁移：

- owner executor、视图故障、节点上限等其余 `WEFT_*` 字符串错误码，随各自模块改造时迁入 `Diag`。
- 异常到 `Result` 的全面替换按模块逐步进行。

## 第 2 步记录（2026-10-10）

`tests/support/` 是测试与探针共用的头文件库，命名空间为 `weft::testing`：

| 文件 | 内容 |
| --- | --- |
| `check.hpp` | `require` 与 `WEFT_REQUIRE`（失败时报出表达式和位置） |
| `files.hpp` | 原样字节读取、文本模式读取、写入，以及相对源码根的读取 |
| `json.hpp` | `json_string` |
| `png.hpp` | WIC PNG 读写与 `check_hresult` |
| `host_time.hpp` | `format_host_time`，原 `probe_host_time` |
| `frame_rate.hpp` | `WindowFrameRate` |
| `owner_trace.hpp` | `OwnerTrace`/`TraceControl`，原 `GalleryOwnerTrace`/`GalleryProfileControl` |

它们的常量放在 `tests/test_constants/support.hpp`。

迁移内容：

- **断言与读文件**：22 个测试文件里的 `require`/`check` 和 14 份读文件函数不再各写一份，每个调用点保留原来的读取方式（相对源码根还是给定路径、字节还是文本模式）。两个宏断言测试改为基于 `WEFT_REQUIRE`，且不在成功路径分配内存。
- **探针侧**：`json_string`、`read_text`、`checked`、PNG 编解码改用共享实现，共替换 7 份副本（4 份解码、3 份编码）。`d3d11_probe.cpp` 在全局作用域先包含全部 support 头，再在匿名 namespace 里用 using 声明接入。
- **头文件自包含**：`scene_visual_compare.hpp`、`cluster_visual_compare.hpp`、`scene_test_image.hpp` 和 owner trace，不再依赖包含方预先定义 `require`、`hr`、`ComPtr`、`json_string`。之前测试里伪造的不转义 `json_string` 已删除。
- **移出探针**：`probes/host_time.hpp`、`window_frame_rate.hpp`、`gallery_owner_trace.hpp` 已删除，测试不再包含这三个探针头。

仍然存在的依赖：四个 scene 测试仍包含 `probes/gallery_scene_session.hpp`/`gallery_cluster_input.hpp`。这套 gallery 宿主包装了 D2D raster 与 DirectWrite 测量，留到第 5、6 步随 D3D11 抽取和 scene 可移植化一起迁出。

## 第 3 步记录（2026-10-10）

三个 owner 平台模块从 `probes/` 移到 `src/script/js/`：
- `web_host_bootstrap.js`：DOM 构造器、CSSOM、计时器、微任务、console；
- `animation_frame_bootstrap.js`：rAF 批次；
- `scene_platform_bootstrap.js`：SVG/Element、窗口事件、几何读取、ResizeObserver、matchMedia。

构建时由 `cmake/embed_bytes.cmake` 生成 `build/<preset>/generated/script/js/*.js.inc`，编进 `weft::script`。生成规则如下：

- **换行**：CRLF 统一为 LF，内嵌文本不随 checkout 方式变化。
- **字节表**：用无符号字节表，不受 MSVC 单个字符串字面量长度上限的约束。
- **增量**：内容不变时不改写输出文件，避免无谓重编。

公开接口 `src/script/script_platform.hpp` 提供三个函数：`script_platform_source`、`script_platform_file`、`install_script_platform(vm, module)`。运行时库因此不再需要源码树。

迁移与测试：

- **调用点**：测试和探针里 37 处 `WEFT_SOURCE_DIR "/probes/*_bootstrap.js"` 读取改为调用 `install_script_platform`。`owner_hotpath_tests` 的旧版本差分仍从 fixture 读取优化前的两个模块。
- **新增测试**：`script_dom_lifecycle` 校验内嵌文本与源文件一致，三个模块无需文件访问即可按序安装。

仍然保留的源码树依赖：浏览器参考时钟（`probes/*_reference_clock.js`，供浏览器采集工具使用，路径被 fixture 清单锁定）、测试 fixture、字体和 gallery 原样 bundle。它们都只属于测试与证据，不属于运行时。

## 第 4 步记录（2026-10-10）

项目定名 Weft（CaseElephant 是立项代号）。分两次提交。

**改名**（提交 `fce0361`）：
- **命名空间与 ABI**：`caseelephant::` → `weft::`；C ABI `ce_*`/`CE_*` → `weft_*`/`WEFT_*`，`ce.h` → `weft.h`；诊断码为 `WEFT_*`。
- **CMake**：`project(Weft)`、`WEFT_BUILD_TESTS`、`WEFT_SOURCE_DIR`，可执行文件 `weft_*`。
- **去 `Probe` 前缀**：`ScriptDomProbe` → `ScriptDom`、`ProbeDomSnapshot` → `DomSnapshot`、`TextProbeFont` → `Font` 等。
- **保持不变**：固定 fixture、原样 bundle（`examples/**`）、`third_party/`、浏览器参考时钟、JS 宿主全局 `__ce*`、字体族名 `ProbeRoboto`/`ProbeCjk`。它们被 fixture 清单或浏览器参考锁定。

**目录与构建**：
- **公开头**：只剩 C ABI，即 `include/weft/` 下的 `weft.h`、`render_backend.h`、`render_backend_d3d11.h`、`render_external_texture.h`。
- **内部头**：移到 `src/<模块>/`，与实现同目录，以 `"<模块>/<文件>.hpp"` 引用。
- **文件去 `_probe`**：例如 `script_dom_probe.cpp` → `script/script_dom.cpp`，`text_probe` → `text/font`，`input_probe` → `input/mouse_router`，`rounded_probe` → `render/rounded_clip`。
- **scene 归位**：scene 文件从 `src/layout`、`src/render` 归到 `src/scene/`。
- 共移动 64 个文件，均用 `git mv` 保留历史。

CMake 拆分：
- **根文件**：根 `CMakeLists.txt` 只剩选项与子目录；`src/`、`tests/`、`probes/` 和每个模块目录各有自己的 CMakeLists。
- **`cmake/WeftTargets.cmake`** 提供四个函数：
  - `weft_add_library`：建 `weft_<模块>` 与别名 `weft::<模块>`；
  - `weft_add_executable`：用 `SOURCE_DIR`/`LARGE_STACK`/`GC_STRESS` 开关取代原先重复 13 次的 `/STACK:8388608` 与宏定义；
  - `weft_add_test`：默认在构建根运行，支持 `GC_STRESS_TIMEOUT`；
  - `weft_add_python_test`：没有 Python 时不注册。
- **`cmake/WeftThirdParty.cmake`**：固定第三方依赖与 `weft_unibreak`。
- **生成文件**：内嵌 JS 表改到 `build/<preset>/generated/script/js/`。

模块目标是 base/core/render/canvas/script/text/style/layout/scene/input，Windows 另有 `canvas_d2d`、`scene_d2d`：
- **合并**：原 19 个 `caseelephant_*` 库按目录合并为 12 个。render 含 lifetime/backend/rounded_clip，layout 含 inline/flex/page/positioned/auto/flow。
- **不变**：静态库只按引用提取目标文件，因此可执行文件链入的代码不变。

验证：
- **测试定义**：改前后用 `ctest --show-only=json-v1` 逐项对照。88 项的名称、命令参数、超时、fixture、依赖和工作目录全部一致，只有可执行文件改了名。GC 压力配置另用 Ninja 只做配置对照，86 项同样一致。
- **编译选项**：同一 Ninja 配置导出 compile_commands，141 个源文件的宏定义与编译开关逐个一致；`WEFT_SOURCE_DIR`、`WEFT_REACT_GC_STRESS` 落在与原先相同的文件上。差异只有两处：
  - 模块源多了 `src/` include 根；
  - 合并后的 layout 源能看到 yoga/harfbuzz/libunibreak 头路径，因此多出一个只影响警告级别的 `-external:W0`。
- **全量测试**：结果见 [status](status.md)。

## 第 5 步记录（2026-10-10）

分四个提交，每个提交单独验证。

**5A shader 构建期编译**：
- `cmake/WeftShaders.cmake` 用 Windows SDK 的 fxc 编译 HLSL，`weft_add_hlsl()` 生成字节表并内嵌。
- 每个 shader 生成两份：带 `NDEBUG` 的构建用 `/O3`，否则用 `/Zi /Od`，与原先运行期编译的选项一致。
- 四边形 shader 移到 `src/render/backends/d3d11/quad.hlsl`。运行时不再读源码，也不再链接 d3dcompiler。
- 字节码对照：Release 字节码与原 `D3DCompileFromFile`（系统 d3dcompiler_47）的输出逐字节相同；Debug 字节码只有调试信息块 SPDB 不同，指令、签名与反射相同。

**5B 渲染器移入 `src/render`**：
- `render/frame_packet.hpp` 定义与 GPU 无关的 `FramePacket`/`Draw`/`Asset`（`Draw::rect` 改用 `geometry::Rect`）；`render/srgb.hpp` 提供 sRGB 解码。
- `weft::render_d3d11`（`src/render/backends/d3d11/`）包含四部分：
  - `Renderer`：原 `Experiment` 去掉夹具检查后的部分；
  - `Backend`：原 `ProbeBackend`，即 C ABI 适配器，连同 `PacketView`；
  - `host.hpp`：同步宿主 `render_window`/`compose_window`，改为自由函数；
  - `TwoFramePipeline`。
- 渲染器报错改用自己的 `ensure`/`throw_if_failed`，消息文本不变。D3D11 限额移入 `constants::render::d3d11`，探针常量改为引用它。
- 探针保留 `Experiment`，作为 `Renderer` 的子类，承载色彩夹具的默认尺寸、断言计数和自检式 `execute()`。

**5C `RenderBackend` concept**：
- `render/backend.hpp` 定义 `RenderBackend`/`ExternalTextureBackend` 两个 concept，以及 `make_c_backend`/`make_c_external_backend`。函数表由 noexcept thunk 生成，是唯一的动态分派点。
- 异常处理：
  - prepare、encode、import 抛出的异常转为 `WEFT_RENDER_BACKEND_ERROR`，并清零输出句柄；
  - shutdown 抛出的异常报告为错误；
  - 按合同不能失败的回调抛出时直接 terminate。
- D3D11 `Backend` 改为满足 concept 的成员函数，函数体不变。
- 新测试：
  - `render_backend_concept`：concept 正反例、表内容、thunk 错误策略，以及经过渲染器核心的完整帧周期；
  - `render_d3d11_contract`：只链接 `weft::render_d3d11`，在 WARP 上跑两帧，做像素回读，验证纹理在完成查询之后才退休、调试层零消息。
- 四个公开 C 头的 include guard 去掉 `CASEELEPHANT_` 前缀。

**5D 探针拆成独立翻译单元**：
- 原先探针头在 `d3d11_probe.cpp` 的匿名 namespace 中途被包含，各头不自包含。
- 现在共享部分在 `probes/probe_common.hpp`，代码放在全局作用域的 `namespace probes`，因此非限定查找与原先相同。
- 每个探针头只保留共享辅助（inline，include 在全局作用域）。每组 `run_*` 模式各为一个 `.cpp`，共 17 个，在 `probe_modes.hpp` 中声明。

验证：
- **产物对照**：`tools/compare_artifacts.py` 逐字节比较图片，报告去掉计时字段后逐值比较。对照基线是第 5 步之前的构建产物。Release 的 28 组确定性套件在每个子步骤后都完全一致；Debug 在第 5 步结束时对照，同样一致。
- **实时窗口套件**：以下 6 组含实时窗口的套件即使同一二进制两次运行也不同（滚轮合并、实时帧数），只依赖它们自身的断言：
  - `cheetah-flow-window-4k`、`cheetah-flow-window-motion`；
  - `gallery-preview`、`gallery-trace`；
  - `owner-async`、`scene-surface-retirement`。
- **硬件对照**：RTX 5090 上新旧二进制各跑 22 个 `--*-hardware` 模式，21 组产物逐字节相同，`gallery-preview` 的差异与旧二进制自身重跑的差异相同。全量测试结果见 [status](status.md)。

留在探针、待后续步骤处理的部分：
- `SceneNativeGpu`：依赖 Direct2D/DirectWrite 文字 raster，随第 6 步 scene 可移植化迁出（已完成，见第 6 步记录）。
- Win32 窗口与交换链宿主（`react_window_host.hpp`）：属于第 10 步的平台层。

## 第 6 步记录（2026-10-10）

分四部分，每部分单独验证。

**6A 可移植几何与命中**（`src/scene/scene_geometry.*`）：
- `SceneGeometry` 计算元素盒（getBoundingClientRect 式几何）与命中测试，不用任何图形 API。
- 实现内容：
  - CSS/SVG transform 列表与 transform-origin；
  - SVG viewBox/preserveAspectRatio；
  - path 数据解析成绝对坐标命令；
  - Bézier 与椭圆弧的精确极值包围盒；
  - nonzero/even-odd 填充包含；
  - 带端点与虚线的描边包含。
- D2D 参考 raster 改为重放同一份解析结果（transform 操作、path 命令），自己只做 D2D 数学。改动后 1,306 个产物文件逐字节不变。
- gallery 会话、owner、预览与原生 GPU 探针的命中和几何全部改用 `SceneGeometry`。

**6B HarfBuzz/FreeType 文字**：
- 字体层：
  - `text/font`：`FontFile` 共享一份字体字节，`Font` 打开 TTC 索引与可变轴，给出 DirectWrite 规则的行度量，并可用 16.16 精度塑形；
  - `text/font_catalog`：字体列表作为数据，按 CSS Fonts 4 匹配；
  - `weft::platform_win32`：用 DirectWrite 枚举已安装字体，只做这一件事。
- `scene/scene_text`：`SceneText` 按 Blink 规则塑形与排版：
  - 逐词塑形；
  - 按 cluster 加 letter-spacing；
  - 宽度向上取整到 LayoutUnit（1/64 px）；
  - 构造 Blink 的单行 line box（半行距向下取整到整像素）。
- D2D 参考 raster 用同一字体文件、索引和轴值的 DirectWrite face 绘制这些 glyph run。

**6C 原生 GPU 文字图像与 SDF**：
- `Font::rasterize` 增加亚像素原点。
- `scene_text_image` 从 FreeType 覆盖率生成文字节点的图像。它与 D2D 共用 `scene_text_origin`（text-align、flex 居中、padding、border），定位一致。
- `scene_values.hpp` 收拢几何、文字图像与 D2D 三处重复的 CSS 值解析（`fail/get/num/len`）。
- 原生 GPU pass 的文字改用该图像；只有含彩色字形的文字和显式 layer 仍用 D2D raster。
- SDF 在 scene 字体上的验证：
  - `scene_text_image` 测试按 scene 布局摆放 SDF field，在 CPU 上用 shader 公式重建，与覆盖率图对比；
  - 详见 [SDF](sdf-font-probe.md#scene-文字重构第-6-步2026-10-10)。

**6D 原生场景渲染器移入 `src/render`**：
- 新目标 `weft::scene_d3d11` 提供 `render::d3d11::SceneRenderer`（原探针 `SceneNativeGpu`），shader 移到 `src/render/backends/d3d11/scene.hlsl`。
- 依赖改为构造参数：`Renderer&` 取代探针的 `Experiment&`，`SceneText&` 取代探针的线程字体。常量移入 `constants::render::d3d11::scene`。

验证：
- **Chrome 文字度量**：
  - 新 fixture `tests/fixtures/scene_text_reference.json`，用 Chrome 155 和本机字体记录 852 个 span；
  - `scene_text_browser_diff` 结果：字体选择一致，宽度最大差 1/64 px，高度与基线完全一致；
  - 154 个合成粗体例只报告（未实现 font-synthesis，宽度最多差 17 px）。
- **几何差分**：可移植实现对 D2D 参考，`tests/scene_geometry_compare.hpp` 接在 scene 与 cluster 测试里。
  - scene：102 帧，369 个元素 rect 最大差 8e-6 px；337,920 个命中点有 234 个边界点不同，原因是 D2D 把 0.25 DIP 内的点算作内部。
  - cluster：210 帧，7,477 个元素 rect 最大差 0.0287 px（D2D 曲线包围盒是近似值），矩阵最大差 6.8e-5。
  - 与 Chrome 的 1,476 个几何字段对照不变，最大 0.274975 px。
- **对 Chrome 参考图**（测试的软掩码 IoU，第 6 步前后对比）：

  | 范围 | 变化的图 | 更近 / 更远 | 变化图中的最小 IoU |
  | --- | --- | --- | --- |
  | scene D2D 参考 | 42 / 102 | 35 / 0 | 0.757 → 0.872 |
  | cluster D2D 参考 | 131 / 210 | 93 / 14（最大退 0.0017） | 0.788 → 0.829 |
  | 原生 GPU 文字（6C 对 6B） | scene 15 张、cluster 56 张 | 38 / 15 | 单张最大退 0.0076、最大进 0.0101 |

  FreeType 覆盖率是线性的，比 D2D 带 gamma 增强的灰度字略细；Chrome 用 ClearType。
- **产物对照**：`tools/compare_artifacts.py`。
  - 6A 的 D2D 去重、6C 的 `scene_text_origin` 与值解析合并：D2D 参考产物逐字节不变；
  - 6D 迁移：对 6C 产物 29 组确定性套件、1,306 个文件逐字节相同。
- **内存错误**：Debug 全量测试发现 6B 的悬空 `string_view`（`split_unit(css::trim(line))` 指向已销毁的临时字符串，Release 下恰好读到原值）。已修复。ASan Debug 构建下 4 个文字测试程序全部通过。
- **耗时**：WARP 下最重的三组原生 GPU 套件，第 6 步前后单独顺序运行分别为 27.7/28.3/24.4 s 和 28.1/29.2/25.1 s，没有回退。全量结果见 [status](status.md)。

仍依赖 Windows 或 D2D 的部分：
- 字体发现只有 Windows 实现（`weft::platform_win32`）；其他平台需要宿主提供 `FontCatalog`。
- 彩色字形（COLRv1 emoji）和显式 layer 图像仍由 D2D 参考 raster 绘制。
- 合成粗体未实现；原生场景 pass 没有 SDF 材质与动态 atlas。
- `SceneGpuSurfaces`（D2D GPU 目标、卡片表面池）与 Win32 窗口宿主仍在探针里，属于第 10 步的平台层。

## 顺序调整（2026-10-10）

用户决定先完成第 7–9 步，第 10 步（第二个图形后端与移动平台）暂缓。跨平台仍是硬性需求：
- 新代码不引入平台 API；
- 平台相关部分只放在 `src/platform` 与 `src/render/backends`；
- 可移植模块不依赖 Windows 头文件或类型。

## 已确认的取舍（2026-10-10）

用户已确认以下四项，依据与修改条件记在 [关键决策](decisions.md)。

1. **静多态边界放在 C ABI 内侧。** 引擎内部全部用模板/concept，ABI 处为每个后端生成函数表。运行时性能优先于编译时间；同一二进制带多个后端时各自实例化。
2. **核心路径从异常迁到 `expected<T, Diag>`。** UI 的局部错误不能让游戏进程终止：错误作为值返回，按 View/节点降级并进入诊断，由宿主决定处理方式。这也满足主机平台常关闭异常和 RTTI 的约束。第 1 步引入 `Diag` 并去掉前缀匹配，其余模块逐步迁移。
3. **退役 page/positioned/auto 三个历史 profile。** 先让它们的浏览器对照全部跑在统一路径上，再删除旧代码。
4. **scene 量字改用 HarfBuzz/FreeType。** Windows 依赖违背跨平台需求；D2D/DirectWrite 只保留为 Windows 参考 raster。现有 102 张 Chrome 对照图依赖 DirectWrite 度量，在第 6 步重新采集基线并说明差异。
