# 架构重构计划

更新日期：2026 年 10 月 11 日。本文是 M0 探针代码走向可维护产品结构的计划，不是已实现能力的证据。第 0 步（代码规范与命名常量，见 [代码规范](coding-style.md)）已完成；其余步骤按风险从低到高排列，每步单独验证、可单独回退。

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
| 7 ✅ | 类型化样式：PropertyId、长度/颜色/关键字值，去掉字符串往返与正则插值 | `style_probe`、`flow_page`、`auto_page`、`scene_probe` | 级联/动画差分、gallery 像素与几何；见下方第 7 步记录 |
| 8 ✅（paint IR 推迟到第 10 步） | 统一格式化路径：一个调度器、一个 Yoga 适配器、一个 paint IR、一条命中路径；golden 迁移后删除 page/positioned/auto | `src/layout/*`、`input_probe` | 旧 profile 的浏览器差分全部改跑新路径 |
| 9 ✅ | DOM 与脚本分离：NodeId 原生树、与 style 共用选择器；QuickJS 对象成为包装；Canvas 状态移到旁表 | `script_dom_probe.cpp`、`style_probe.cpp` | DOM 生命周期、GC 压力、D3 / React 原包 |
| 10（暂缓） | 一份 paint IR 与统一渲染器（第 8 步推迟的部分）；第二个图形后端（Vulkan 或 Metal）与 Android/iOS 平台层，按 concept 接入；主机构建选项 | `src/render/backends`、`src/platform` | 平台验证按“配置/编译/链接/模拟器/真机功能/真机性能”分级记录 |

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

## 第 7 步记录（2026-10-11）

分三部分，每部分完成后全部确定性产物逐字节不变。

**7A 属性与值类型**（`src/style/values.*`、`properties.inc`、`keywords.inc`）：
- `PropertyId`：111 个属性，X-macro 按名字排序，按 id 迭代即按名字迭代。测试检查每个 style profile 注册的属性都有 id。
- `Keyword`：88 个关键字，大小写敏感（与原来的字符串比较一致），`currentColor` 是 `currentcolor` 的别名。
- `CssValue`：保留原文，getComputedStyle、诊断和以字符串为键的缓存仍看原文。另存一次解析出的 payload：关键字、带单位数字（px/%/em/deg/rad/turn/s/ms/fr）、`calc(a ± b)`、颜色。payload 只由原文决定，相等比较原文。
- `StyleValues`：按 id 排序的 vector。保留按名字读写的接口；写入未注册的名字报 `Diag::StyleProfile`。

**7B 计算样式与 scene 改用 `StyleValues`**：
- 级联在解析声明时确定 id，profile 按 id 建索引（是否继承、解析好的初始值），继承直接遍历父值。
- page/positioned/auto/flow 在入口用 `string_map()` 转回字符串 map（它们原来就复制 map）。第 8.2 步已去掉。
- scene、D2D 参考 raster 和 D3D11 scene 渲染器按 id 比较和跳过属性：几何相关键、布局签名忽略表、paint 参数。
- 关键帧声明只能用已注册属性，其他名字报 `SceneProfile`。原来这类名字会成为无人读取的条目。`scene_contract` 增加反例，去掉检查的变异会使它失败。

**7C 热路径**：用新加的 owner 采样剖析（见下）找到三类开销：
1. **扩容时整棵子树复制**（kline 中约占 owner 时间 19%）。MSVC 的 `std::map` 移动构造不是 noexcept，所以含 map 的 `SceneNode`、`DomSnapshot` 在 vector 扩容时被复制而不是移动。修复：建 scene 树和样式输入键前，按 DOM 子节点数 reserve。
2. **payload 解析抛异常**（scatter 约 7%、anime 约 4%）。`std::stod` 对非数字文本抛异常。修复：先按 strtod 语法检查（十进制/十六进制、可选指数），完整匹配时才调用；颜色先看首字符排除。
3. **按名字查属性**（2–4%）。修复：scene 各模块 233 处字面量名字改用 `PropertyId`；继承表、边/角 longhand、Yoga 边、min/max 和 paint 键改为 id 表。

同时去掉 `src/` 中全部 7 处 `std::regex`：
- 插值的数字切分改为 `css::number_spans`；
- 关键帧注释；
- 混排文字的空白折叠（原来每个文字节点都构造一次 regex）；
- grid `repeat()`；
- perspective 平面语法：几何与 D2D 的三份合为 `scene_grammar`（第 8.3a 步并入 `parse_scene_transform`）；
- Canvas 颜色分量改为 `css::decimal_syntax`。

剖析工具：
- `--gallery-owner-sample <目录> <类别>` 运行与 `--gallery-owner-profile` 相同的固定时钟场景，另在 warm tick 期间每 250 µs 采样 owner 线程调用栈。
- 输出 `samples.txt`（项目帧、独占、包含三张表，带调用者）和 `samples.folded`（火焰图输入格式）。
- 每次采样挂起线程，用 x64 unwind 表回溯后立即恢复；不需要管理员权限，也不用 ETW。
- `windows-profile` preset 与 Release 相同（`/O2 /Ob2`、`/OPT:REF,ICF`），另带 PDB 用于符号化。

验证：
- **产物对照**：7A、7B、7C 完成后分别用 `tools/compare_artifacts.py` 对照第 7 步前的产物，29 组确定性套件 1,306 个文件逐字节相同。complex-css 报告里的 paint program 构建、复用和驱逐计数也不变。
- **差分测试**：
  - `style_values_contract`：注册表与 payload 表；22,000 个随机数字/颜色文本对照标量与颜色解析器；120,000 个由 strtod 语法片段拼出的文本（十六进制、指数、inf/nan、px 后缀）对照 `parse_number`。关掉十六进制分支的变异会被发现。
  - `grammar_differential`（新）：每个手写语法对照它替换的原 regex。覆盖 37,643 个数字切分、空白与注释替换、Canvas 十进制、perspective 整串匹配与搜索（含捕获组）、grid repeat。
- **owner CPU**：用 `tools/compare_owner_profile.py` 与第 7 步前的二进制交替运行 3 轮，每轮交换先后。
  - 条件：Ryzen 9 9950X3D、Windows 11 26200、MSVC Release，测量期间无并发编译；固定 60 Hz 时钟，每例 240 帧、16 帧预热，不含 GPU。
  - warm tick（前 → 后）：

    | 场景 | P50 | P99 | 最大 |
    | --- | --- | --- | --- |
    | K 线 kline-00 | 13.07 → 4.88 ms | 20.21 → 9.50 ms | 23.89 → 10.27 ms |
    | D3 散点图 d3_interactive-03 | 3.11 → 1.93 ms | 5.28 → 3.52 ms | 6.91 → 4.48 ms |
    | anime 14 例（中位数） | 0.137 → 0.116 ms | 0.648 → 0.432 ms | — |
    | anime-05（最重的 anime 例） | 1.60 → 1.22 ms | 3.35 → 3.12 ms | 7.53 → 5.71 ms |

  - framer、spring 的变化在轮间波动内。spring-01 的 P99 在两次对照中一次 −0.13 ms、一次 +0.17 ms。
  - 原始数据在 `artifacts/local/step7/pre7-vs-step7/`；只含 7B 的对照在 `pre7-vs-7b/`（kline P50 14.17 → 8.93 ms）。
- **可见窗口拖拽**（RTX 5090、4K 240 Hz，`--gallery-live-benchmark-hardware`；前 3 次、后 6 次，取中位数）：

  | 场景 | 被拖卡片更新/秒 | owner 活动 P50/P99 | 输入到渲染 P50/P99 |
  | --- | --- | --- | --- |
  | anime 拖拽 | 87 → 126 | 10.5/20.5 → 6.9/11.7 ms | 4.5/13.6 → 4.2/10.2 ms |
  | 散点图拖拽 | 139 → 200 | 13.5/23.5 → 5.7/11.3 ms | 10.1/16.8 → 5.9/10.5 ms |

  - present FPS 不变（约 235–239）。
  - 长帧：第 7 步两个场景各 6 次中各有 1 次出现单个渲染线程长帧（anime 96 ms、散点图 194 ms），都在第一批；第 7 步前 3 次中最大 37 ms。第二批，以及另外两边交替各 8 次（与 Debug 构建并发，只用来查长帧），最大值都不超过 27 ms，未复现。渲染线程一侧只改了读样式的方式和 D2D paint program 的字节估算；该缓存先受 256 项上限约束，complex-css 的驱逐计数不变。
  - 原始数据在 `artifacts/local/step7/live*`。

未做，留到后续步骤：
- 插值仍输出 12 位有效数字文本，消费方再解析。去掉这次往返会改变输出（不再舍入），留到第 8 步统一 paint IR 时一起量化。
- page/positioned/auto/flow 给 Flex 适配器的仍是字符串 map（第 8.2 步已改为类型化输入）。
- 剩余 owner 开销主要是结构性的复制与比较，不属于样式类型。按 kline 采样：
  - `DomSnapshot` 每帧快照、复制与比较，其中 gallery 会话 `sync_scene` 的比较约 4.5%；
  - scene 保留上一帧 `SceneFrame`、DOM 与计算样式的整份复制，约 15%；
  - 选择器匹配约 7%，几何 shape 约 8%。
  - 前两项对应第 9 步原生 DOM，后两项对应第 8、9 步的统一命中路径与共用选择器。

## 第 8 步记录（2026-10-11）

已完成（paint IR 推迟到第 10 步，见本节末尾）：页面只剩一个文档格式化器，文档与 scene 共用一个 Yoga 适配器和一条命中路径，scene 只有一个 transform 模型。

**8.1a 行盒规则**（`009a11d`）：
- 文档 inline 布局改用 Blink 的半行距拆分：上方 floor(leading / 2) 整像素，其余 LayoutUnit 行距在下方。与 scene 文字共用 `split_leading()`（scene 产物逐字节不变）。
- flow-layout fixture 对 Chrome：几何平均差 0.0442 → 0.0176 px，p99 0.79 → 0.31；片段平均 0.116 → 0.037，最大 0.850 → 0.792。250 个字段变近，没有变远的。
- flow-input 命中 oracle 原来从原生片段几何推导 #outer/#inner 的采样点，行盒一变采样点就偏离采集时的坐标。现改用采集时的坐标（successor hash 已登记），命中结果不变（3,130/3,150 精确）。

**8.1b 统一的文档格式化器**（`dca1716`、`12e460c`）：
- flow profile 改名为文档格式化器：`layout_document`、`DocumentLayout`、`document_style_profile`、`WEFT_DOCUMENT_PROFILE`。输出类型移到 `layout/output.hpp`。
- 先用实验补丁让旧对照跑在 flow 上。几何结果与旧 profile 逐值相同（auto fixture 1,984 个字段、React 300 个字段零差异），只有 AlertBar 字体基线超阈值（0.59），由 8.1a 解决。
- 补齐其他 profile 的 fixture 所需能力，都是 CSS 语义：
  - 视口大小的初始包含块：根元素可为 static，没有定位祖先的 absolute 盒相对视口；
  - flex item 的 px/auto margin（block flow 仍要求 0，因为还没有 margin 合并）；
  - 只写 `ProbeRoboto` 的字体列表；
  - 匿名盒继承 white-space；会被折叠掉的空白不生成匿名盒（CSS 2.1 9.2.2.1）；
  - canvas 替换元素：属性给出固有尺寸，单边指定时按固有比例，两边都指定时（px 或 %）交给 Yoga；
  - 延续 positioned 的规则：ul 必须显式设置 margin 与 padding（UA 缩进未建模）。
- 所有调用方改用 `layout_document`：
  - React 页面与窗口，经共享的 packet 构建器（`prepare_page_glyphs` 和 `MouseRouter` 的 build_page 构造函数删除）；
  - NineSlice、AlertBar 四种 CSS 及字体测量、Canvas gallery；
  - 测试：positioned 与 auto 的单元用例移到 `document_cases_tests.cpp`；变成合法 CSS 的旧拒绝用例（`:hover`、根元素 padding、`overflow:auto`、只写宽度的 absolute、CJK 换行、混合 inline）删去，仍适用的诊断保留。

**8.1c 删除旧代码**（`d51277c`）：page/positioned/auto 的实现、三个样式 profile、两个诊断码和只被它们使用的常量。`document_style_profile()` 改为一张表，与原链式结果逐项相同（导出两边比较）。

验证：

| 浏览器对照 | 旧 profile 最大 | 统一路径最大 | 阈值 |
| --- | --- | --- | --- |
| React | 0.0759 | 0.0759 | 0.1 |
| NineSlice | 0.1238 | 0.1238 | 0.2 |
| AlertBar 几何 / 字体基线 | 0.1511 / 0.4416 | 0.1511 / 0.0896 | 0.2 / 0.5 |
| cheetah-style 几何 / 颜色 | 0.1511 / 3.9e-7 | 0.1511 / 3.9e-7 | 0.2 / 1e-5 |
| cheetah-auto | 0.2581 | 0.2581 | 0.3 |
| auto fixture（48 场景） | 0.1841 | 0.1841 | 0.3 |
| Canvas gallery 位图 | 通过 | 通过 | 原阈值 |

- `ctest --preset windows-release --parallel 6` 96/96，`windows-debug --parallel 6` 96/96（319.75 s）。
- 产物：除迁移的 7 组套件外与 8.1a 后逐字节相同。迁移套件的变化来自绘制列表（透明背景不再发出绘制项，文字按 Blink 行盒）和字体测量；React 退休纹理仍为 522。

**8.2a 类型化输入**（`9679ab7`）：
- `FlexNode::style` 改为 `StyleValues`。适配器从解析好的 payload 读 px、% 长度和无单位系数，其余值仍走原文本路径，诊断不变。
- 文档格式化器直接持有计算出的 `StyleValues`，`string_map()` 删除；gallery 探针在脚本边界自己生成 getComputedStyle 的 map。

**8.2b 一个 Yoga 适配器**（`3719a5a`）：
- 新接口 `layout_boxes`（[flex.hpp](../src/layout/flex.hpp)）：
  - 输入是按树序排列的 `LayoutBox`，借用格式化器的样式与文本，不复制；
  - 文字由格式化器经 `TextMeasurer` 测量（`FunctionRef`，不分配）；
  - 文档的 `FlexNode` 树在入口展平；scene 直接展平 `SceneNode`，每次布局每个文字盒只调用一次测量回调。
- scene 原有的约 400 行 Yoga 代码删除。下列能力移入共享适配器：
  - grid（等宽 1fr 列）；
  - svg 替换盒（宽高属性与 viewBox 比例）；
  - 子项全是行内级的块（单行、顶对齐的近似）；
  - transform 建立的包含块。
- 属性按作用分类：
  - 盒属性映射到 Yoga，文字属性交给文字测量，绘制属性忽略；
  - 影响盒子却没有映射的属性（`border`、`list-style`、`overflow-x/y`）报错；
  - `CssAdapter` 与 `RestrictedBlockAdapter` 合并为一种 CSS 模式。
- scene 改用与文档相同的 CSS 语义：
  - flex item 默认 shrink 1，并有自动最小尺寸（原来非 flex 容器的项从不收缩，flex 容器的项可以缩到 0）；
  - block 的子项不 grow、不 shrink，align-self 不起作用；
  - display:none 的盒子及其子树几何为 0；grid 的 display:none 子项不占格；grid 的行、列间距分别取 row-gap、column-gap；
  - `normal` 对齐关键字按 CSS 解释；scene 也接受适配器支持的其他值，如 space-between、baseline、content-box、inline-flex；
  - 文字盒的高度不再被可用高度截断（CSS 中内容溢出）。
- scene 给匿名文字节点同时写了 `border-width` 简写和四个长写。共享适配器把这种组合当作丢失了声明顺序而报错（kline-00 发现）。这个简写没有读者，已删去。

验证：
- 先做实验：scene 用三种 shrink 规则（CSS、全 0、全 1）时，34 个 gallery 场景与 30 个 cluster 场景的产物都逐字节相同。所有采集场景都没有主轴溢出，因此改成 CSS 语义不改变已有证据。
- 新增 `box_layout_contract`，10 组反例覆盖上述 CSS 行为与输入诊断。以下 7 个变异各自都会使它失败：
  - block 子项可以 flex；
  - 去掉自动最小尺寸；
  - 恢复文字高度截断；
  - grid 的隐藏子项参与行高或占格；
  - transform 不建立包含块；
  - 忽略 viewBox 比例。
- `ctest --preset windows-release --parallel 6` 97/97，`windows-debug --parallel 6` 97/97（375.8 s）。
- Debug 下 scene gallery、cluster 测试由 89、311 s 变为 121、375 s：共享适配器每次布局都逐属性校验并计算固有尺寸，未优化构建中开销明显。Release 下两者不变（6.56、27.92 s → 6.58、27.77 s，交替各 3 次）。
- 29 组确定性套件的 1,306 个产物与 8.1 后逐字节相同，包括 scene 的 102 个 Chrome 几何采集、cluster 的 210 个采集与像素参考。
- owner CPU（与 8.2a 二进制交替运行，固定 60 Hz，不含 GPU）：
  - K 线 warm tick P50/P99 中位数 4.135/5.896 → 4.121/5.481 ms（6 轮）；
  - 散点图、anime、framer、spring 持平。

**8.3a 一个 transform 模型**（`40b9470`）：
- 原来 scene 的 transform 有两套文法（几何用的 `scene_transform_ops`、绘制用的类型化值）、一套透视文法（`scene_grammar`）和四处矩阵合成（几何、原生绘制、D2D 的字符串路径与类型化路径）。现在合成一个模型（[scene_transform.hpp](../src/scene/scene_transform.hpp)）：
  - `parse_scene_transform`：唯一的文法，CSS 与 SVG 两种写法，包括透视平面子集；
  - `scene_affine`：双精度求值；
  - `scene_perspective`、`scene_plane`：透视平面与节点平面；
  - `scene_transform_origin`。
- 几何与命中、原生绘制、D2D 参考光栅都改用它。各自读样式文本还是读编译好的类型化值，保持原来的选择：D2D 的增量/全量逐字节对照依赖 CPU 光栅读文本。`scene_grammar.hpp` 删除。
- 修正一个实际错误：SVG transform 属性里无单位的角度按规范是度，旧文法当成弧度。gallery 的 D3 径向标签用 `rotate(${x - 90}) translate(${y},0)`，d3_interactive-00 因此画错。改后与 Chrome 参考的平均通道差 0.32 → 0.075（24、63、120 三帧），偏差超过 64 的像素 38,910 → 1,024。
- 文法的其他变化：
  - SVG 列表允许用逗号分隔函数，rotate 可带中心；
  - 透视平面接受 CSS 的其他合法写法（函数间不留空格、指数）。原正则接受的文本，新文法给出相同的平面（差分测试 4 万条随机文本）。

验证：
- 新增 `scene_transform_contract`：顺序、百分比、CSS 与 SVG 角度、旋转中心、分隔符、诊断与透视平面。以下 7 个变异各自都会使它失败：
  - SVG 角度改回弧度；
  - 去掉旋转中心；
  - 去掉逗号分隔；
  - 颠倒合成顺序；
  - 平移百分比不按盒子解析；
  - 放开透视的函数次序；
  - 忽略 transform-origin。
- `ctest --preset windows-release --parallel 6` 98/98，`windows-debug --parallel 6` 98/98（408.7 s；scene cluster 测试 375 → 408 s，Debug 下逐节点解析 transform 的开销）。
- 产物与 8.2 后相比：
  - 26 组逐字节相同，其中包括 D2D 光栅的 102 张 gallery 参考图；
  - cluster：d3_interactive-00 的 14 张图如上变化；其余变换元素的几何最多差 6e-6 px，13 张原生图最多差 1 个通道值、最多 25 个像素；
  - scene-native 8 张、render-stages 4 张 WARP 图最多差 1 个通道值、最多 292 个像素：原生矩阵改为先用 double 计算再转 float。
- owner CPU 与 8.2b 交替 4 轮无回退：K 线 P50/P99 4.957/8.651 → 4.868/8.289 ms，散点图 1.977/3.323 → 2.008/3.366 ms，anime、framer、spring 持平。

**8.3b 一条命中路径**（`4ef79ad`）：
- 新模块 [hit_test.hpp](../src/layout/hit_test.hpp)：
  - 格式化器按绘制顺序列出命中区域 `HitAreas`；
  - 每个区域有逆平面与形状：半开矩形、闭合边界（SVG 文字）、圆角、按规则填充或描边的轮廓；
  - 区域挂在一条裁剪链上，裁剪链按父索引共享；
  - 一个函数 `hit_areas` 取最上层包含该点的区域。
- 文档：`Page::hit_regions` 换成 `HitAreas`；`hit_page` 改用共享判定，`enabled` 由调用方的谓词决定。
- scene：
  - `SceneGeometry::hit_test` 不再逐点遍历整棵树判定，改为先生成命中表 `hit_map`，再做共享判定；
  - fill/stroke 的包含判断移入共享模块；
  - D2D 参考保留自己独立的树遍历，仍作差分 oracle；
  - 比对辅助函数每帧只建一次命中表。
- 平面类型 `geometry::Plane` 移到 `base`，布局与 scene 共用。
- 修正 scene 命中与绘制的一处不一致：`z-index:auto` 在绘制中按 0 处理，命中却直接报错，现在也按 0。

验证：
- 命中结果与改动前完全相同：
  - D2D oracle 的逐帧网格对比通过；
  - gallery 中依赖悬停/点击状态的帧、cluster 的 50 次输入与 210 个状态采集逐字节不变；
  - 产物差异集合与 8.3a 完全相同。
- 新增 `hit_test_contract`：形状、填充规则、端点与虚线、绘制顺序、跳过的目标、裁剪链、平面。以下 8 个变异各自都会使它失败：
  - 矩形改为闭区间；
  - 忽略裁剪；
  - 忽略谓词；
  - 改为先画者优先；
  - 忽略 even-odd；
  - 裁剪不用自己的平面；
  - square 端点当作 butt；
  - 忽略虚线。
- `scene_contract` 增加层叠用例：z-index:1 的兄弟盖住后插入的 z-index:auto。去掉 auto 的处理或去掉层叠排序都会使它失败。
- `ctest --preset windows-release --parallel 6` 99/99，`windows-debug --parallel 6` 99/99（323.9 s；比对辅助函数每帧只建一次命中表，scene gallery、cluster 测试 120、408 s → 82、324 s）。
- owner CPU（与 8.3a 交替 4 轮）：K 线 P50/P99 4.629/7.779 → 4.670/7.767 ms，散点图 1.949/3.229 → 1.967/3.290 ms，其余持平。

**8.3b 补充：scene 命中按圆角**（`038e534`）：
- 文档命中早已按元素自己的圆角判定，scene 原来忽略 border-radius。现在 scene 的盒子区域和 overflow:hidden/auto 的裁剪都按画出来的圆角判定。
- 半径的算法抽成 `scene_values::corner_radii`，命中、原生绘制与 D2D 参考共用同一个长度解析：
  - D2D 一直支持 `calc()`，原生路径原来不支持；
  - 动画在 px 与 % 之间插值圆角时会产生 `calc(… px + …%)`，这些帧原来整帧回退到 D2D；
  - 现在原生路径也能画：scene-native 套件的原生回退 1512 → 1452 次，css-06-24 由原生绘制（与原 D2D 图相差 640 个边缘像素，测试按原容差通过）。
- D2D oracle 的命中同样改为按圆角，网格差分仍然有效。
- `scene_contract` 增加反例：
  - 圆形盒子外接矩形的角不算命中；
  - 圆角裁剪外的子元素不算命中。

仍未做：
- 自动最小尺寸仍用自底向上的固有尺寸估计，百分比的指定尺寸不参与上限。百分比边距或间距、百分比宽或按比例定高的替换盒，只在 flex item 确实需要时报 `WEFT_UNSUPPORTED_INTRINSIC`。
- scene 文字仍不换行；子项全是行内级的块是单行、顶对齐的近似（不断行，不按基线对齐）；svg 比例只在父高度不确定时生效（沿用 scene 原规则）。
- 适配器的错误仍是 `WEFT_UNSUPPORTED_*` 字符串异常，尚未迁入 `Diag`。
- p、button 的 UA 默认 margin/padding/border 未建模（ul 已显式诊断）；文档字体仍是固定的探针 FontSet，按 FontCatalog 匹配只在 scene 中实现。

**用户决定（2026-10-11）**：一份 paint IR 推迟到渲染器合并时再做，作为第 10 步渲染工作的开头。

理由：两套 IR 的消费者无法汇合。
- scene 的 GPU 渲染器从 `SceneNode` 栅格化文字与图层，彩色字形和图层回退到 D2D（仅 Windows），只支持单个裁剪矩形；
- 文档路径是已塑形字形、CPU 圆角裁剪链和轴对齐四边形，没有变换。

只合并数据类型，代码不会减少，收益要等统一渲染器（字形图集、可移植彩色字形、裁剪栈、图层）落地才兑现。第 8 步就此完成，下一步是第 9 步。

## 第 9 步记录（2026-10-11）

已完成：原生 DOM 树在 `src/dom`，节点之间的边用 NodeId，QuickJS 对象只做包装；DOM 查询与样式共用一个选择器引擎；Canvas 状态在节点旁表；快照按版本号增量刷新。

**9.1 DOM 值类型移入 `src/dom`**（`c435dd5`）：
- `NodeIdentity`、`DomSnapshot`、`DomChanges`（`dom/node.hpp`）与 `ElementGeometry`（`dom/geometry.hpp`）是各层都用的值。style、layout、scene 原来为了它们引用 `script/script_dom.hpp`，连带 Canvas 模型。
- 现在 style、layout、scene、render、text、canvas、base 的头文件都不再引用 `script/`；用到 `ScriptDom` 的文件自己引用。
- 该提交顺带收入了 9.2 尚未接入构建的 `dom/selector.{hpp,cpp}` 与 `constants/dom.hpp`。

**9.2 一个选择器引擎**：
- `dom/selector`：选择器只编译一次，匹配从右向左，经 `SelectorElement` concept 读取元素（模板，静态分发）。三个访问器：
  - 样式的快照祖先路径 `PathElement`；
  - 单个快照元素 `LoneElement`，供 hover/active 状态键使用；
  - JS 活 DOM `LiveElement`。
- 删除 script 的字符串匹配器（每个节点重新解析一次选择器），以及 style 自己的选择器类型与解析器。`SelectorGrammar` 保留两处原有差异：非 scene 样式不接受转义与 `:hover`/`:active`；两边的诊断码不同。
- 统一后的语义变化见 [兼容范围](compatibility.md#样式和布局)：
  - DOM 新增 `>`、属性运算符、转义、`:root`/`:hover`/`:active`；样式新增 `~= |= ^= $= *=`；
  - HTML 元素按小写后的选择器名精确比较，SVG 名称按原样；
  - 标识符不能以未转义的数字或“连字符加数字”开头（与浏览器一致，旧解析器都接受）；
  - DOM 查询把合法但未支持的选择器（`WEFT_UNSUPPORTED_SELECTOR`）与语法错误（`WEFT_SELECTOR_SYNTAX`）分开诊断。
- 新增 `selector_contract`，覆盖：大小写、标识符开头、属性运算符、class 空白、需要回溯的后代匹配、伪类、specificity、列表、两种文法与两类诊断。20 个旧行为或错误行为的变异全部被它发现。`scene_contract` 的 DOM 选择器检查与 `style_cascade_contract` 的反例随范围更新。
- 验证：Release 100/100，Debug 100/100（278.3 s）；1306 个产物与改动前逐字节相同。owner CPU：与第 8 步结束时的二进制交替 4 轮，每轮交换先后（RTX 5090 / Ryzen 9 9950X3D / Windows 11 26200 / MSVC Release，默认优先级与 affinity，固定 60 Hz 时钟，每例 240 帧、16 帧预热；只计 owner CPU，不含 GPU 与内存；测量期间无并发编译或测试）。warm tick 各轮中位数 P50/P99/max：K 线 4.556/6.995/9.680 → 3.991/6.772/8.889 ms，散点图 1.981/3.289/4.525 → 1.704/2.571/4.132 ms，两例每轮 P50 都更快；K 线 P99 各轮范围重叠（候选有一轮 9.86 ms）。anime 14 例合并中位数 0.117/0.507 → 0.109/0.440 ms，framer、spring 持平。收益来自样式匹配：旧实现每匹配一个复合选择器，都用 istringstream 把 class 拆进一个 std::set。原始数据在 `artifacts/local/step9/owner-cpu-9.2/comparison.json`。

**9.3 Canvas 状态移到旁表**：
- 节点不再有 Canvas 成员。CanvasState 与 CanvasRenderingContext2D 包装放在 `dom::NodeTable`（[node_table.hpp](../src/dom/node_table.hpp)）里，按节点 slot 索引、按代际校验。
- 条目随元素存亡：元素的 `mark()` 报告 context 包装这条边，元素的 finalizer 释放条目。slot 被新节点复用时，新节点没有旧条目。
- 两份 canvas 尺寸解析（脚本 DOM 与文档布局）合并为 `dom::canvas_dimension`（[attributes.hpp](../src/dom/attributes.hpp)）。默认 300×150 只在 `constants::dom` 定义一次。两处原来的错误码不变：脚本报 `WEFT_CANVAS_SIZE_LIMIT`，布局报 `WEFT_DOCUMENT_PROFILE`。
- 行为变化（见 [兼容范围](compatibility.md#当前-canvas-探针范围2026-10-09)）：
  - `-0` 按 HTML 规则得到 0；
  - getContext 的 options 脚本若已创建 context，返回那一个，修掉了原来的引用泄漏。
- 测试：
  - 新增 `dom_contract`：旁表的查找、删除与 slot 复用，canvas 尺寸的逐例解析与上限；
  - `canvas_2d_contract` 增加三个反例：只由元素保活的 context 在 GC 后仍是同一个对象，options 重入只得到一个 context，回收后复用同一 slot 的新 canvas 没有 context。
  - 9 个变异全部被发现：漏报 mark 边、finalizer 不清条目、getContext 不复查、resize 找不到条目、旁表忽略代际（查找、删除各一）、`-0`、前导空白、上限差一。
- 验证：Release 101/101，Debug 101/101（323.8 s）；1306 个产物逐字节不变。强制 GC 配置（`build/windows-gc-stress`，每次分配前完整 GC）下 canvas_2d_contract、script_dom_lifecycle、animation_frame_contract、page_react_contract、dom_contract 5/5（53.0 s，日志 `artifacts/local/step9/gc-stress-9.3.log`）。owner CPU：与第 9.2 步结束时的二进制交替 3 轮，条件同 9.2：K 线 P50/P99 4.131/6.890 → 4.038/6.798 ms，散点图 1.669/2.781 → 1.666/2.949 ms，anime 0.109/0.348 → 0.110/0.409 ms，各轮范围重叠，记为持平。改动在热路径上只多了 GC 标记时每个节点一次越界判断（没有 canvas 时旁表为空）。原始数据在 `artifacts/local/step9/owner-cpu-9.3/comparison.json`。

**9.4a 树边改用 NodeId**：
- 节点的 parent、document 与 children 从 JSValue 改为 NodeIdentity；遍历（祖先、子节点、选择器、快照、事件路径、命中与滚动链）都按身份查 slot 表。
- GC 所有权不变：每条边仍在邻居的包装对象上持有一个引用，`mark()` 报告它，finalizer 释放它，存活仍由 QuickJS 的收集器决定。
- slot 表的值改为 `{node, wrapper}`。节点被回收时只清 node，wrapper 留到 slot 被复用为止：同一次收集里，后回收的节点仍能释放对先回收邻居的引用（此时该对象尚未真正释放）。
- 变异：6 个中 5 个被发现。mark 漏报子边或父边、detach 不放开父对子的引用，生命周期测试都报泄漏；insert 不取子引用、新节点不取 document 引用，都会崩溃。“finalizer 不释放对子节点的引用”不可观测：父节点被回收时子节点必然在同一批垃圾里，QuickJS 整批释放，计数是否归零无从观察。真正能观测的是对始终存活的 document 的引用，它的变异会崩溃。
- 验证：Release 101/101，Debug 101/101（336.4 s）；1306 个产物逐字节不变；强制 GC 下 script_dom_lifecycle、animation_frame、scene_contract、canvas_2d、page_react 5/5（60.3 s）。owner CPU：先交替 3 轮，K 线 P50 4.685 → 4.996 ms；但基线本身各轮在 4.15–5.16 ms 之间波动，于是只测 K 线复测 5 轮。复测中后台安静的两轮两者都在 3.98–4.01 ms，其余轮次互有快慢，记为持平。散点图与 anime 持平。原始数据在 `artifacts/local/step9/owner-cpu-9.4a/`。

**9.4b 脚本状态移出节点**：
- 节点只剩 DOM 数据。包装对象、expando（D3 的 `__data__`、`__on` 等自有属性）、事件监听器和滚动事件标记放进按 slot 索引的 `Binding` 表。
- 绑定表用 `std::deque`：表增长时已有条目不搬家。派发事件时，监听器回调可能新建大量节点，事件派发仍持有目标的监听器列表引用。
- 节点被回收时，绑定表清掉 expando、监听器和滚动标记，只留包装对象，供同一次收集里的邻居释放引用。
- 测试：
  - 生命周期测试新增两条：回收后复用同一 slot 的新节点没有旧节点的监听器和自有属性；监听器在派发途中新建 4096 个节点后，同一目标后面的监听器照常运行。
  - 3 个变异全部被发现：finalizer 不清监听器，新节点沿用 slot 里的旧 expando，绑定表改用 vector（后者崩溃）。
- 验证：Release 101/101，Debug 101/101（323.1 s）；1306 个产物逐字节不变；强制 GC 下 5/5（71.9 s）。owner CPU：测不出变化。本机这段时间负载重（IDE、浏览器等在后台运行，K 线 P99 从平时约 6–7 ms 升到 8–15 ms），交替对照的方向互相矛盾。默认优先级下 9.4a→9.4b 测 4 轮，互有快慢。对照工具新增 `--high-priority`（两个版本都以 Windows 高优先级运行），再测三组，每组 5 轮：9.3→9.4b 每轮都是候选更慢（P50 中位 4.817 → 5.252 ms）；9.3→9.4a 与 9.4a→9.4b 却都是候选在 5 轮中 4 轮更快（4.135 → 3.987、4.746 → 4.238 ms）。候选在纯几何的命中测试上也同幅快慢，可见差异来自运行环境，不是 DOM 代码。数据在 `artifacts/local/step9/owner-cpu-9.4b/`。

**9.4c slot 表改用空闲堆**：
- `GenerationalSlots::first_free()` 原来从 slot 0 线性扫描，新建一个节点的代价与存活 slot 数成正比。现在空闲 slot 进最小堆，仍返回最低的可用 slot：节点、视图与渲染资源分到的 slot 与原来完全相同，只是查找降为对数时间。
- 占用一个 slot 时不删它的堆条目，`first_free()` 在这类条目到达堆顶时丢弃。每个 slot 至多一个条目，堆为每个 slot 预留了容量，所以 noexcept 的 `vacate()` 不会分配内存。
- base 测试新增随机差分：20 万步占用、回收与追加，逐步与线性扫描的参考实现比较；代际用 8 位，slot 会在运行中退役。5 个变异全部被发现。
- 基准（临时程序，MSVC /O2，Ryzen 9 9950X3D）：在全部占用的 slot 表上，每步随机回收一个再取最低空闲 slot。2048 个 slot 时 311 → 15.8 ns/步；32768 个（DOM 节点上限）时 3.48 µs → 19.4 ns/步。两版选出的 slot 序列相同（校验和一致）。源码与结果在 `artifacts/local/step9/slot-churn-9.4c/`。
- 验证：Release 101/101，Debug 101/101（347.2 s）；1306 个产物逐字节不变。gallery owner 剖面的稳态帧很少新建节点，这一步没有再测。

**9.4d 原生 DOM 树移入 `src/dom`**：
- `dom::Tree`（[tree.hpp](../src/dom/tree.hpp)）持有节点存储（按 slot 与代际），提供：
  - 带校验的插入与移除。拒绝时不改动任何东西；成功时返回原父节点，供绑定层调整引用；
  - textContent、是否连到本树的 Document、变更日志与子树版本号、快照、dump、选择器查询与 `TreeElement`。
- 树不决定节点寿命：脚本绑定在包装对象被回收时调用 `release()`。
- 脚本层（script_dom.cpp 约 3050 → 2900 行）只剩包装对象、脚本状态和 GC 所有权。每条树边在邻居包装上持有一个引用。插入时新边先取引用、旧边后放，子节点在中间不会降到零引用。
- reset 时树保留 slot 代际与累计计数，节点身份和统计照旧跨 reset 延续。DOM 上限（深度 256、变更日志 1024）移到 `constants::dom`。
- 测试：`dom_contract` 新增 6 组不依赖脚本引擎的树测试：
  - 插入次序与移动；
  - 各类拒绝及其原子性，含深度上限；
  - 变更日志与子树版本号；
  - 快照与 dump；
  - 选择器的查询范围；
  - 释放、slot 复用与 epoch。
  11 个变异全部被发现；其中去掉环检查后树会成环，遍历不终止，以超时失败。
- 验证：Release 101/101，Debug 101/101（336.1 s）；1306 个产物逐字节不变；强制 GC 下 6/6（67.0 s）。owner CPU：后台安静时与 9.4b 的二进制以高优先级交替 5 轮：K 线 P50/P99 3.978/6.046 → 3.960/5.859 ms，散点图 1.737/2.520 → 1.723/2.792 ms，anime 两边都是 0.115 ms，各轮范围重叠，记为持平。K 线 P50 与 9.3 时安静条件下的 3.98–4.01 ms 一致，9.4 整体没有可测的回退。数据在 `artifacts/local/step9/owner-cpu-9.4d/`。

**9.5 增量快照**：
- 快照记录复制时节点的子树版本号（`DomSnapshot::revision`）。它不属于内容，`==` 不比较它。同一节点、同一非零版本号的两份副本内容必然相同。改动快照副本的代码（`normalize_selector_state`）会把版本号清零。
- 9.5a 生产端：
  - `dom::Tree::refresh` / `ScriptDom::refresh_snapshot` 把旧副本刷新到最新：版本号没变的子树原样保留；子节点按身份对应，重排、插入、移除只动变化的部分。
  - 返回值是内容是否真的变化：只登记了变更、内容其实没变的，不算变化。
  - 宿主几何/样式读保留一份文档副本原地刷新，不再在每次 DOM 变更后丢弃重建。
  - gallery scene 会话首帧做一次全量快照，之后原地刷新，直接用刷新结果决定是否重建场景，不再深比较和整份拷贝。JS 几何读回调拿到的文档副本用 `dom::update_copy` 增量并入。
- 9.5b 消费端：场景求解器的上一帧 DOM 用 `update_copy` 增量维护；静态子树复用的比较改用 `dom::same_content`，同一副本状态的子树直接判等，变化路径之外不再逐层深比较。
- 测试：
  - `dom_contract` 新增随机差分：6000 步随机变更，包括插入、移除、文本、属性、悬停/激活、滚动，以及只登记、不改内容的变更。定期刷新副本并断言：与全量快照相同；报告的“是否变化”与内容比较一致；版本号对齐；没有变化时不做任何工作。`update_copy`（来源分别为快照和清零了版本号的值）与 `same_content` 也逐步对照。
  - 另有快照相等只看内容（逐字段）与“版本号相同、节点不同”的反例；`script_dom` 新增 400 轮 JS 随机变更加宿主悬停的集成差分。
  - 变异：refresh 10 个，update_copy/same_content 8 个，全部被发现。refresh 原有 1 个等价变异：“副本版本号非零”的判断多余（节点版本号从 1 开始），已删去。
- 剖析（windows-profile，K 线 owner 采样，`artifacts/local/step9/sample-*`）：改动前，快照相关开销约占三成，包括深比较、拷贝赋值、析构、复制与样式输入键。9.5a 后同样帧数的样本从 1275 降到 872；9.5b 后再降到 795。
- 验证：Release 101/101，Debug 101/101（325.7 s）；1306 个产物逐字节不变；强制 GC 下 6/6（63.7 s，日志 `artifacts/local/step9/gc-stress-9.5.log`）。
- owner CPU（高优先级交替 5 轮，后台安静）：
  - 9.5a 相对 9.4d，每轮都更快：K 线 P50/P99 4.003/5.982 → 3.209/4.675 ms（P50 −20%），散点图 1.680/2.401 → 1.565/2.369 ms，anime 0.111/0.372 → 0.107/0.357 ms，framer、spring 小幅下降；
  - 9.5b 相对 9.5a：K 线 P50/P99 3.270/5.505 → 2.915/4.712 ms（P50 −11%，每轮都更快）；散点图、anime、framer、spring 持平。整个 9.5 下来，K 线 P50 从 4.003 降到 2.915 ms（−27%）。
  - 数据在 `artifacts/local/step9/owner-cpu-9.5a/`、`owner-cpu-9.5b/`。

第 9 步就此完成。求解器里还有不随版本号的整份工作：样式输入键每帧重建、上一帧 SceneNode 与计算样式的整份拷贝。它们不属于快照，留待后续。

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
