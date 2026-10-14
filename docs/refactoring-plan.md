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
src/vector/                矢量路径的覆盖率：FreeType 光栅器与描边器，虚线（2026-10-12 加入）
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
| 10（进行中，见下方计划） | 一份 paint IR 与统一渲染器（第 8 步推迟的部分）；第二个图形后端（Vulkan 或 Metal）与 Android/iOS 平台层，按 concept 接入；主机构建选项 | `src/render/backends`、`src/platform` | 平台验证按“配置/编译/链接/模拟器/真机功能/真机性能”分级记录 |

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

第 9 步就此完成。求解器里还有不随版本号的整份工作：样式输入键每帧重建、上一帧 SceneNode 与计算样式的整份拷贝。它们不属于快照，留待后续（后由 owner CPU 第五轮第 3 步完成，见 [交互性能](gallery-interaction-profile.md#重构之后继续压-owner-cpu2026-10-11第五轮)）。

## 第 10 步计划（2026-10-11）

**用户决定（2026-10-11）**：第 9 步完成后开始第 10 步。先统一 paint IR 与渲染器，再接第二个图形后端和移动平台。开发机是 Windows PC，第二个后端选 Vulkan。

现状（只读调研）：
- 两份绘制 IR，两个 D3D11 渲染器：
  - 文档：版面输出 `PositionedPage`（矩形、文字、图片，带裁剪矩形和圆角裁剪链）在探针里转成 `render::FramePacket` 的轴对齐四边形。圆角裁剪在 CPU 上栅格成遮罩资产，字形也由 owner 栅格成资产。`d3d11::Renderer` 每个四边形一次绘制调用，按线性预乘混合。
  - scene：`compile_native_scene` 产出 `NativeScene`（带矩阵的圆角盒、边框、渐变、文字与图层图像、隔离组）。`d3d11::SceneRenderer` 用 `scene.hlsl` 实例化绘制，按与 D2D/Chrome 相同的编码空间预乘混合。SVG、投影变换、阴影等整帧回退 D2D；含彩色字形的文字和显式图层也由 D2D 栅格。
- 对外 C ABI v1（`weft_render_frame`）是四边形加资产，属实验接口。

目标（架构合同“FramePacket 与 GPU 生命周期”“绘制顺序与后端”两节）：
- 一份可移植的 `paint::DisplayList`：值型绘制项、逻辑坐标、变换、裁剪链、分组顺序，以及字体与图片的不可变 lease；不含 atlas UV、设备对象或 Node 指针。
- 一个渲染器：可移植部分（准备、批处理、字形 atlas、分组、裁剪）写一次；后端只实现设备、资源、管线和 pass。D3D11 与 Vulkan 共用同一份 HLSL 源码（Vulkan 经 glslang 编成 SPIR-V）。
- 合成空间是 pass 的属性：文档 pass 用线性预乘（架构决定），scene pass 保持编码空间预乘（Chrome 对照基线）。
- D2D 只作 Windows 参考 oracle，不再是任何平台的绘制路径。

分段（每段单独验证、提交）：

| 段 | 内容 | 验证 |
| --- | --- | --- |
| 10.1 | `src/paint`：`DisplayList` 与 scene 生产者，替换 `NativeScene`；`SceneRenderer` 改读 `DisplayList` | scene-native、cluster GPU 产物逐字节不变 |
| 10.2 | 可移植的 scene pass 规划（记录、批次、清屏色、分组深度）与 D3D11 执行分开，Vulkan 只需实现执行 | 产物逐字节不变 |
| 10.3 | 文档生产者 `PositionedPage → DisplayList`；统一渲染器补齐圆角裁剪链（着色器内距离场）、字形 atlas、图片与线性合成；退役每四边形一次调用的旧路径和 CPU 圆角遮罩 | 文档类浏览器差分全部通过；像素变化逐套说明 |
| 10.4 | Vulkan 后端：同一渲染器核心与同一 HLSL；宿主 encode/submit/完成 serial 合同；WARP/D3D11 对照 | Vulkan 与 D3D11 像素差分、完成与退休、validation layer 零告警 |
| 10.5 | 可移植文字与路径：FreeType 彩色字形；SVG 填充与描边的 CPU 三角化（带抗锯齿）；scene 不再回退 D2D 绘制 | D2D oracle 与 Chrome 对照；回退计数降为 0 或逐项列出 |
| 10.6 | `src/platform` concept；Android（NDK）与 iOS 平台层；主机构建选项 | 按“配置/编译/链接/模拟器/真机功能/真机性能”分级记录，未执行的标为未验证 |

依赖与工具（2026-10-11 用户批准：先装 Vulkan-Headers 与 Vulkan SDK，需要时再装 DXC 与 Android NDK；没有 macOS）：
- Vulkan 官方头文件（Khronos Vulkan-Headers，Apache-2.0），按固定 tag vendored 进 `third_party`，记录 SHA-256。运行时经驱动自带的 `vulkan-1.dll` 加载，不依赖 Vulkan SDK。
- SPIR-V 编译：本机 Windows SDK 的 dxc 没有 SPIR-V 后端；RenderDoc 自带 glslang 16.2.0 可把现有 HLSL 编成 SPIR-V，但不是可复现的构建依赖。候选：vendored glslang 源码在构建期编译，或要求安装固定版本的 Vulkan SDK。
- validation layer 需要 Vulkan SDK 或单独的 layer 包。已安装 Vulkan SDK 1.4.363.0（LunarG 签名的安装包，SHA-256 与 winget 清单一致），含验证层与带 SPIR-V 后端的 DXC 1.9.0.5512。
- Android NDK 用于编译与链接验证；iOS 需要 macOS 与 Xcode，本机只能做到“代码与配置”，编译及以上全部标为未验证。

## 第 10 步记录（2026-10-11）

**10.1：`src/paint` 与 scene 显示列表**（详见上方计划）：
- 新模块 `src/paint`（`weft::paint`）：
  - `DisplayList` 是可移植的绘制 IR：条目（种类、仿射变换、轴对齐裁剪、尺寸、不透明度、负载下标）按绘制顺序排列，盒子与光栅源放在各自的表里，条目保持小而连续；
  - `Box` 是带渐变与实线边框的圆角盒；`Raster` 是渲染器按设备缩放栅格的内容；`BeginGroup`/`EndGroup` 是隔离的不透明度组；
  - `check()` 校验负载下标与种类、光栅源、分组配对与深度上限，失败返回 `Diag::PaintList`（`WEFT_PAINT_LIST`）；渲染器绘制前调用，校验失败计为一次带原因的回退。
- scene 生产者 `scene_display_list()`（原 `compile_native_scene`，文件改名为 `scene_display_list.*`）产出 `DisplayList`；`NativeScene`、`NativePaint` 与 `constants::scene::gradient_*` 删除，渐变种类改用 `paint::GradientKind`（数值与 `scene.hlsl` 一致）。
- `d3d11::SceneRenderer` 改读条目与负载表；记录里只有着色器不读的字段（文字、图层条目的盒子字段）不再填写。
- 过渡：scene 文字与显式图层的光栅源仍是 scene 节点（`paint` 头文件里前向声明），10.5 换成字形串与嵌套列表后去掉。

验证：
- `ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（228.3 s）；
- 1306 个产物与改动前逐字节相同（scene-native、cluster GPU 的像素与回退计数都不变）；
- 新增 `paint_contract`：合法嵌套列表与空列表通过；越界的盒子与光栅负载、无源光栅、多余的组结束、未闭合的组、超出深度和未知种类各自报出对应原因。

**10.2：可移植的 scene pass 规划**：
- 段落调整：Vulkan 只依赖 scene pass 的规划与执行接口，不依赖文档路径并入，所以把规划拆分放在前面；可移植文字与路径排到 Vulkan 之后（10.5），平台层改为 10.6。
- `render::plan_scene_pass()`（`src/render/scene_pass.*`）把显示列表变成共享着色器的实例记录 `SceneRecord` 与绘制步骤：连续的盒子合成一次实例化绘制，文字与图层图像、组开始、组结束各占一步；第一个盒子若不透明且（含 crop 在内）正好覆盖目标，就作为清屏色。
- `d3d11::SceneRenderer` 只负责图像构建、资源与执行这些步骤。scene pass 常量从 `constants::render::d3d11::scene` 移到可移植的 `constants::render::scene`。

验证：
- `ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（227.3 s）；
- 1306 个产物逐字节相同；
- `paint_contract` 增加规划检查：步骤切分与盒子连续段、渐变与遮罩着色记录、组合成记录覆盖整个目标、crop 使第一个盒子不再作为清屏色、尺寸不足时不清屏。

**10.3a：文档版面产出 `DisplayList`，经可移植的四边形降级保持旧输出**：
- IR 增加：
  - 资产 `paint::Asset`/`paint::Encoding`（原 `render::Asset`，`render` 保留同名别名）；
  - 图片条目 `Item::Kind::Image` 与负载 `paint::Image`（资产下标、uv、着色、SDF 描边宽度与颜色）；
  - 圆角裁剪：`DisplayList::clips` 与条目上的连续区间（外层在前）；`RoundedClip` 类型移到 `paint/rounded_clip.hpp`，几何与覆盖率函数仍在 `render`；
  - `check()` 同时校验图片负载与资产、圆角裁剪区间。
- 生产者 `page_display_list()`（`src/layout/page_display_list.hpp`，按字体类型静态多态）：矩形成为盒子，图片与字形成为图片条目（字形以文字颜色着色）；同一段绘制的条目共享裁剪与圆角裁剪区间。字形图像缓存 `GlyphImages` 带条目与字节上限（常量移到 `constants::layout`）。
- 降级 `render::lower_to_quads()`（`src/render/quad_lowering.*`）：每个条目按裁剪矩形裁切并移动 uv；圆角裁剪切到条目时在 CPU 上准备覆盖率遮罩（每像素 16×16 采样），可跨包保留在 `QuadMasks` 里。资产按首次使用排序，与原探针一致。
- 探针 `cheetah_packet()` 改为“生产者 + 降级”的组合，原来约 200 行的四边形构建与遮罩代码离开探针。
- scene pass 只画盒子、光栅与分组，遇到图片条目或圆角裁剪返回 `Diag::PaintList`（渲染器计为回退）。
- IR 的裁剪矩形存 `{left, top, right, bottom}`，`right` 只在生产端按 `x + width` 算一次，降级时直接使用，与原来的 `clip[0] + clip[2]` 是同一个浮点值，所以裁切结果逐位不变。

验证：
- `ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（334.1 s）；
- 1306 个产物逐字节相同，其中文档路径的 cheetah、cheetah-alert/auto/style/flow、flow、react、render-cache、render-stages、complex-css 各套件都在内；
- `paint_contract` 增加：图片裁切后的 uv、完全在圆角内的盒子不建遮罩、被圆角切到的盒子用覆盖率遮罩、保留的遮罩在下一包命中、旋转条目与圆角盒子被拒绝；scene pass 拒绝图片条目与圆角裁剪。

**10.3b：共享着色器直接绘制文档显示列表**：
- 着色器 `scene.hlsl` 增加：
  - 图片条目：覆盖率遮罩着色、保留自身颜色的图像、SDF 填充与描边，都按 uv 取资产的子区域；
  - 着色器内的圆角裁剪链：裁剪放在结构化缓冲里，每层取矩形距离与角上椭圆的一阶距离，按一个设备像素抗锯齿，多层相乘；
  - 线性合成是编译期变体（`SPACE_LINEAR`，`display_linear`）：颜色由 CPU 转成线性并预乘，不量化；没有圆角也不旋转的盒子按像素中心覆盖，与四边形光栅一致，不在直角外漏出距离带。
- scene 用的编码变体代码不变。一次在 `premult` 里用运行时分支的尝试使编译器改变了计算顺序，一张图有 1 个像素差 1 级，所以改用编译期变体。
- 规划器：图片条目的记录（模式、uv、着色、描边）；圆角裁剪规范化后随 pass 上传；线性 pass 的颜色在这里转换；2px 透明边距只属于 scene 的文字与图层光栅。
- D3D11 `Renderer`：
  - `prepare(DisplayList)` 复用原有的资产准备、缓存与引用；
  - `encode` 遇到显示列表就用线性变体实例化绘制，记录与裁剪是不可变结构化缓冲，随命令列表存活；之后恢复四边形管线，rtt 合成照旧；
  - 宿主辅助 `render_list()` 不经 C ABI 直接驱动渲染器（准备、录制、提交、真实完成）。
- `SceneRenderer` 遇到图片条目或圆角裁剪计为回退，因为 scene 槽位没有资产纹理与裁剪缓冲。

验证：
- `ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（390.6 s）；
- 1306 个产物逐字节相同：scene 的像素不变，文档探针此时仍走四边形；
- `render_d3d11_contract` 新增对照：同一个显示列表（整屏底色、半透明盒子、着色的覆盖率遮罩、取半透明图片子区域、带描边的 SDF、被圆角裁剪切到的盒子与图片）直接绘制与经四边形降级绘制，在 WARP 上只有 16 个像素超过 2 级，最差 8 级，都在圆角裁剪边缘（解析覆盖率与 16×16 超采样遮罩之差），平均差 0.019 级；
- 变异检查 5 个（裁剪链失效、覆盖率遮罩不着色、线性颜色不转换、忽略 uv、忽略 SDF 描边）全部被发现。

**10.3c：文档与四边形帧都走显示列表，逐四边形绘制与 CPU 圆角遮罩退役**：
- C ABI 增加显示列表帧（[backend API](render-backend-api.md#显示列表帧2026-10-11)）：
  - 不透明句柄 `weft_paint_list`、`weft_render_list_frame` 与 `weft_renderer_prepare_list`；
  - 能力位 `WEFT_RENDER_FEATURE_DISPLAY_LIST` 与后端表末尾可选的 `prepare_list` 同时出现，`struct_size` 在它之前结束的旧表按无此能力处理；
  - 核心在回调前校验结构与上限、资产、有限可逆的变换、颜色、盒子与渐变、圆角裁剪和图片能力；scene 光栅条目不过 ABI。
- IR：
  - 外部纹理是排在资产之后的图片下标（`DisplayList::external_textures`），按自身颜色绘制、不带 SDF 描边；
  - `paint::append()` 带缩放与平移追加列表并重排下标，HUD 与页脚拼接用它；
  - 一个条目的圆角裁剪链最多 16 层（`max_rounded_clips`，架构合同要求的嵌套上限），超出时规划返回 `Diag::PaintList`，C ABI 返回 `UNSUPPORTED_FEATURE`。
- `FramePacket` 可以只带一个不可变显示列表；`PacketView` 按帧的种类选择入口。
- D3D11 渲染器只剩一条绘制路径 `record()`：
  - 四边形帧经可移植的 `render::quad_list()` 变成显示列表：纯色成为盒子，带纹理成为图片条目，组诊断成为隔离组，裁剪诊断成为内缩的裁剪；
  - 隔离组在渲染器尺寸的线性 float 图层里合成，嵌套用多层，每次开始时清空；
  - rtt 合成、线性 alpha 检查、外部纹理的生产者和窗口宿主的呈现（输出图像加 HUD）也都是显示列表；
  - 底部起始的外部纹理翻转记录的 uv（`render::flip_rows`）；
  - 记录与裁剪缓冲按最大帧增长并复用，详见下面的性能。
- 规划器把实例范围收到裁剪外 2 px 以内，被裁掉的条目不再光栅化；线性 pass 的轴对齐图片和无圆角盒子不再外扩 2 px，覆盖规则与原四边形光栅一致。输出不变。
- 删除 `quad.hlsl`、`Renderer::draw`/`QuadConstants` 与 `lower_to_quads`/`QuadMasks`（CPU 圆角遮罩）。Direct2D 编码预乘纹理的线性转换改为 `srgb_convert.hlsl` 的编译期变体（`encoded_convert`）。
- 线性变体在裁剪与图片范围外输出透明黑而不是 discard：预乘 source-over 不改目标，相邻像素的导数也保持有定义。WARP 上输出不变，编码变体代码不变。
- 文档探针（cheetah 系列、flow、React 窗口、图库 shell 与 HUD）发送显示列表帧。CPU 对照改为对显示列表求值：
  - 像素中心包含、裁剪矩形、双线性采样，圆角裁剪取 16×16 子像素面积；
  - 圆角边缘（像素中心 1 px 内有圆角经过）在线性光下比较，界 0.05：线性距离过渡在 45° 边上与精确面积最多差 0.043，而 sRGB 编码会把透明边缘上的同一差放大到几十级。

像素变化（与第 9 步基线逐套对照，WARP）：
- cheetah、cheetah-alert/auto/style：每帧 1–84 个像素差 1 级，来自图片与字形的 uv 由插值改为按像素中心计算；
- cheetah-flow 与 flow 窗口：最差 7 级，每帧 11–21 个像素超过 2 级，都在圆角卡片边缘，即解析抗锯齿取代 16×16 超采样遮罩；
- flow：透明根节点的圆角处最差 59 级（同上，透明边缘被 sRGB 放大）；椭圆裁剪的 alpha 与独立的稠密面积积分最多差 11 级，界从 5 改为 12；
- sdf 与 backend 的 SDF 图：8 个像素差 3–9 级，都在缩放字形抗锯齿带的暗端：uv 改为按像素中心计算，千分之几的覆盖率差在暗端被 sRGB 编码放大；
- render-cache：遮罩资产不再参与，GPU 缓存命中由 99 次变为 137 次；flow 每帧纹理数 161/166/106 变为 155/158/99；
- 其余 19 个套件逐字节不变，包括颜色夹具（组与裁剪诊断）、外部纹理、React、scene、图库与两帧。

性能（4K 文档滚动窗口 `--cheetah-flow-window-perf-sync-hardware` 与 `--cheetah-flow-window-perf-hardware`，与第 9 步末的二进制交替运行）：
- 条件：AMD Ryzen 9 9950X3D + RTX 5090，Release，3840×2160，原样 React flow 页面（60 个 DOM 元素，中文与拉丁字体，原图）。每轮 8 帧预热加 120 帧，两个二进制交替各 3 轮，每侧 360 帧，未控制热状态。

  | 每帧阶段（ms） | 第 9 步 P50 / P99 / 最大 | 10.3c P50 / P99 / 最大 |
  | --- | --- | --- |
  | 构包（版面到显示列表，旧路径再降成四边形） | 0.057 / 0.079 / 0.093 | 0.048 / 0.069 / 0.078 |
  | 准备 | 0.038 / 0.072 / 0.155 | 0.047 / 0.070 / 0.076 |
  | 录制 | 0.342 / 0.553 / 0.594 | 0.440 / 0.626 / 0.690 |
  | 窗口合成 | 0.122 / 0.207 / 0.259 | 0.164 / 0.251 / 0.256 |
  | UI 的 GPU 时间 | 0.063 / 0.072 / 0.512 | 0.125 / 0.312 / 2.309 |
  | 两帧流水线的一帧（不含完成等待） | 5.04 / 6.68 / 6.99 | 5.31 / 6.93 / 7.19 |

- 两帧流水线吞吐 186.4 → 178.5 帧/秒（3 轮平均）；冷启动首帧约 41 → 23 ms；峰值工作集 120 → 115 MB。
- 构包变快，因为不再生成 CPU 遮罩。录制、合成与 GPU 变慢：每个条目一条 448 字节的记录，每个字形仍是一次绘制。
- 每帧新建记录缓冲曾让 GPU 出现 6–8 ms 的尖峰。改为复用缓冲后，360 帧中最慢的一帧是 2.3 ms：原生录制命令列表的驱动用 `UpdateSubresource`；WARP 由运行时模拟命令列表，在那里结构化缓冲的 `UpdateSubresource` 让大列表只画出清屏色（原因未查明），所以 WARP 改用 discard 映射。
- 下一步 10.3d：字形 atlas 让相邻字形合成一次实例化绘制，并压缩记录。

验证：
- `ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（235.6 s）；
- 文档类浏览器差分（cheetah、alert、auto、style、flow、React）全部通过；RTX 5090 上 16 个硬件探针（颜色、backend、SDF、cheetah 四套、flow、React、外部纹理、两帧、缓存、两个 flow 窗口与 React 窗口）全部通过，覆盖原生命令列表的缓冲写入路径；
- `paint_contract` 增加四边形列表、外部纹理、`append` 与裁剪链上限检查；`render_backend_contract` 增加显示列表帧的能力、表长度、拒绝原因与引用持有检查；`render_d3d11_contract` 改为显示列表直接绘制与 C ABI 列表帧逐字节相同、各部分对 CPU 期望值，以及嵌套隔离组和逐帧清空的组图层；
- 变异检查 12 个：首轮 9 个（组与裁剪诊断失效、底部起始纹理不翻转、组图层不清空、嵌套组取错图层、线性裁剪与图片范围失效、追加时外部纹理不后移、能力位不查）全部被发现；补充的 3 个里两个裁剪链上限被发现，“scene pass 也去掉外扩”起初未被发现，补规划器范围测试后被发现。

**10.3d：字形 atlas 与按纹理合批**：
- 可移植的 `render::AtlasPacker`：方形页面上的货架装箱。页面只追加，某页的区域全部归还后才重新开始；区域从不移动（架构合同“首版页面 append-only，无在线搬移”）。
- 规划器：调用方给出每个图片源的纹理名时（`plan_scene_pass` 的 `textures`），相邻的盒子与采样同一纹理的图片合成一次实例化绘制；批次记下要绑定的图片源（`Batch::source`）。不给纹理名时行为不变，scene 路径就是这样。
- D3D11 渲染器（只在开启资产缓存时）：
  - 不超过 128 px 的覆盖率资产放进 2048×2048 的 R8 页面（最多 4 页），四周复制一圈边缘纹素，双线性过滤读到的就是独立纹理在 CLAMP 下的值；
  - 放不下的资产仍用自己的纹理；缓存淘汰时归还区域；
  - 上传沿用按资源阶段重试的机制（取消或录制失败后会重新上传），按区域写入用 `UpdateSubresource1`：模拟命令列表时，原来的 `UpdateSubresource` 会把非零起点的目标框错误地施加到源数据上（调试层也会报告）；
  - 记录的 uv 映射到页内区域（`render::place_in`）；纹理名取 SRV 地址，同一页上的连续条目合成一次绘制；
  - 新增计数 `atlas_placements`、`display_draws`、`display_instances`。

像素：与 10.3c 的产物逐字节相同（含 Debug 产物共 2659 个文件，0 套差异）。新单元对照里 48 个缩放的覆盖率图：RTX 5090 上与单独纹理逐字节相同；WARP 上 270 个像素差 1 级，原因是 WARP 量化过滤权重，而页内坐标的舍入略有不同。

性能（条件同 10.3c，与第 9 步末的二进制交替各 3 轮，每侧 360 帧）：

  | 每帧阶段（ms） | 第 9 步 P50 / P99 / 最大 | 10.3d P50 / P99 / 最大 |
  | --- | --- | --- |
  | 录制（同步模式） | 0.392 / 0.477 / 0.602 | 0.127 / 0.177 / 0.241 |
  | 录制（两帧流水线） | 0.324 / 0.432 / 0.458 | 0.086 / 0.142 / 0.160 |
  | UI 的 GPU 时间（同步模式） | 0.063 / 0.165 / 0.478 | 0.116 / 0.131 / 0.316 |
  | 两帧流水线的一帧 | 5.13 / 6.15 / 6.97 | 4.87 / 5.64 / 6.75 |

- 4K 页面每帧约 23 次绘制、375 条记录；10.3c 是每个字形一次绘制。吞吐 190.8 → 198.7 帧/秒（3 轮平均）；冷启动首帧约 39 → 20 ms；峰值工作集 120 → 115 MB。
- GPU 每帧仍比第 9 步多约 0.05 ms，来自每个像素读一条 448 字节的记录与线性 pass 的运算；记录压缩留作后续。

验证：
- `ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（310.2 s）；RTX 5090 上 14 个硬件探针与硬件版 `render_d3d11_contract` 通过；
- `paint_contract` 增加装箱（货架顺序、开新页、页数上限、清空后重来、重复归还）与按纹理合批的检查；`render_d3d11_contract` 增加缓存渲染器经 atlas 绘制 48 个缩放覆盖率图，与单独纹理对照，每帧不超过 4 次绘制，第二帧全部命中，清空缓存后区域全部归还；
- 变异检查 6 个（不复制边缘、不映射页内区域、合批不比较纹理、淘汰不归还区域、上传写到页面原点、批次绑错纹理）全部被发现。

**10.4a：Vulkan 渲染器直接绘制显示列表**：
- 依赖（用户 2026-10-11 批准下载）：
  - Vulkan-Headers 按 tag vulkan-sdk-1.4.363.0（commit 6802bb47）vendored 进 `third_party/vulkan-headers`，只取 C 头文件与许可，逐文件锁 hash；
  - Vulkan SDK 1.4.363.0（LunarG 签名，SHA-256 与 winget 清单一致）提供验证层和 DXC 1.9.0.5512；`cmake/WeftSpirv.cmake` 核对 DXC 版本，没有它时 Vulkan 后端不构建。
- 共享着色器移到 `src/render/shaders/scene.hlsl`。DXC 把线性变体编成 SPIR-V（Vulkan 1.1，VS 翻转 y），寄存器按位移放进一个描述符集：b0→0，t0/t1/t2→10/11/12，s0→20（`constants::render::vulkan` 同名常量）。
- `weft::render_vulkan`：
  - `Device`：运行时打开驱动的加载器（Windows `vulkan-1.dll`，其他平台 `libvulkan.so.1`），不链接导入库；装了验证层就启用，并统计 warning/error；选指定名字、否则第一块独显的设备；
  - `Renderer`：资产经暂存缓冲上传（半透明 sRGB 图像在 CPU 上按与 D3D11 GPU 转换逐位相同的算法转成线性预乘浮点）；用可移植规划绘制，同一资产的相邻图片合批；Vulkan 的实例号已含 firstInstance，所以每批用 firstInstance 定位、View 常量里的 base 为 0；隔离组在浮点图层里合成（R32G32B32A32 不支持混合时用 16 位浮点）；用传统渲染通道，兼顾只有 Vulkan 1.1 的 Android 设备；这一版每帧等待完成，C ABI 与两帧在途留到 10.4b；
  - 组图层在录制前按规划的嵌套深度一次建好。最初在 BeginGroup 时按需追加，嵌套组让图层数组扩容，目标栈里指向旧图层的指针随之失效：Debug 下 `render_vulkan_contract` 崩溃，Release 碰巧通过。D3D11 按下标取图层，没有这个问题。
- 测试：D3D11 与 Vulkan 共用的显示列表和 CPU 期望移到 `tests/support/render_lists.hpp`；`render_vulkan_contract` 用同一份列表对照 CPU 期望和 D3D11 WARP 的像素。
- 跨后端一致性：
  - Vulkan 允许 `fwidth` 取粗导数或细导数，RTX 5090 上距离场边缘与 WARP 差到 18 级；线性变体改用显式细导数（`ddx_fine`/`ddy_fine`，Vulkan 1.0 必须支持）；
  - 距离场边缘的宽度不再对采样值求导：距离每个纹素变化一个纹素，宽度就是一个设备像素覆盖的纹素数，由规划器按条目映射算出放进记录（`mode.w`）。对采样值求导会在 8 位距离的平台上骤变；
  - 能覆盖像素中心的条目，范围外扩半个像素：Vulkan 对恰好落在边上的像素中心，取舍规则由实现决定；
  - 编码变体（scene）的代码不变。
- 像素：D3D11 的 SDF 夹具（`sdf`、`backend` 的 sdf.png）每张 1006 个像素变化，最差 143 级，都在缩放拉丁字形的边缘；方块直边与描边的误差指标（0.0633、0.0343）前后相同，大差值是旧算法在采样距离上求导时的孤立毛刺。其余产物与 10.3d 逐字节相同（计时套件除外，Release 与 Debug 都比较了）。

验证：
- `ctest --preset windows-release --parallel 6` 103/103（新增 `render_vulkan_contract`，没有 Vulkan 时以 77 跳过），`windows-debug --parallel 6` 103/103（293.5 s）；
- `render_vulkan_contract` 在 RTX 5090（Vulkan 1.4.341）上：页面列表的各部分符合 CPU 期望，两帧隔离组正确，与 D3D11 WARP 对照页面列表最差 1 级、隔离组 0、字形 2 级，验证层零告警；`--adapter AMD` 在 AMD Radeon 核显（1.4.315）上同样通过，最差 1 级；
- 变异检查 4 个（直通混合、组图层不转为着色器读、实例都从记录 0 开始、距离边缘宽度为零）全部被发现：前三个由 `render_vulkan_contract`，第四个由 `paint_contract`；组图层不转换时验证层是否也报告，未单独确认。

**10.4b：Vulkan 的 C ABI 适配器**：
- 公共头文件 `include/weft/render_backend_vulkan.h`：payload `weft_render_vulkan_pass` 带设备、命令缓冲与目标视图三个句柄，按 uint64 传递，核心 C ABI 不依赖 Vulkan 头文件；C 冒烟测试固定它的布局（56 字节）。
- 录制合同：宿主开始的 primary command buffer，在 render pass 之外交给 encode；适配器只录命令，不结束、不提交、不等待。目标执行时处于 `COLOR_ATTACHMENT_OPTIMAL`，命令结束时仍是该布局，默认保留内容。
- encode 失败时什么都没录：暂存、记录、裁剪、描述符集、帧缓冲、组图层与新目标格式的管线都在录制前建好，录制阶段是一段 `noexcept` 的纯命令序列，所以错误总是 native_use=0，与 D3D11 一样完整回滚。
- `vulkan::Renderer` 拆成 prepare、encode 与 collect：
  - prepare 为列表每个资产建纹理，在 `ResourceLifetime` 登记租约与帧，失败时销毁已建的；
  - encode 上传 Created 与 Encoded 的纹理（与跟踪器的规则相同），录制整个 pass；每次录制的暂存、记录、裁剪、描述符池与帧缓冲属于该 use，丢弃时立即销毁，提交后在完成时销毁；
  - collect 销毁退休的纹理，丢弃已提交上传的 CPU 纹素；`draw()` 是同一序列在渲染器自己的命令缓冲上，读回前加 host barrier。
- `vulkan::Backend` 经 `make_c_backend` 生成函数表：显示列表帧与四边形帧（经可移植的 `render::frame_packet` 与 `quad_list`；`frame_packet` 由 D3D11 适配器共用），一个 prepared 帧、两个在途 use，目标格式 sRGB RGBA8、sRGB BGRA8 与线性 RGBA16F（Vulkan 要求这三种都能混合）。还没有外部纹理扩展、纹理缓存与字形 atlas。
- 验证层默认同时开启同步验证（`VK_EXT_validation_features`）。

验证：
- `ctest --preset windows-release --parallel 6` 104/104（新增 `render_vulkan_backend_contract`），`windows-debug --parallel 6` 104/104（235.5 s）；
- `render_vulkan_backend_contract` 在 RTX 5090 与 AMD Radeon 核显上通过，同步验证零告警：
  - 12 种被拒的 payload 与注入的录制失败都不录命令、不留资源；
  - 丢弃的录制立即释放，下一次重新上传；两个未提交的录制丢弃其一，另一个完整；
  - 两帧在途各按自己的 serial 退休，完成前不释放任何资源；同一帧录到两个目标只上传一次；完成 serial 可越过 UI 的提交；
  - 宿主红色上的半透明绿色在 sRGB、BGRA 与 RGBA16F（精确的 0.5）目标上保留，清除时 alpha 正确；
  - 四边形帧、页面列表、隔离组与字形串经 ABI 的像素与渲染器自身路径逐字节相同；
  - destroy 在帧在途时返回 BUSY 并拒绝新帧；宿主确认设备丢失后 abandon 立即退休。
- `render_vulkan_contract` 现在断言 Vulkan 与 D3D11 WARP 没有超过 2 级的像素；
- 变异检查 8 个全部被发现：提交时就释放帧资源、未提交的录制当作已上传、完成不退休 use、完成退休所有 use、录制之后才失败、不保留宿主内容、上传的布局转换不等待拷贝、abandon 不释放 use。其中上传那一个只有同步验证能发现：关闭同步验证（`--no-synchronization`）时测试通过；
- D3D11 产物与 10.4a 逐字节相同（计时套件除外）。

**10.4c：可移植的纹理缓存与 Vulkan 字形 atlas**：
- 新模块 `render/texture_cache.hpp`（属 `weft::render`）收拢两个渲染器共用的缓存策略，纹理本身仍是各后端的：
  - `AssetIdentities`：显示列表共享资产的稳定身份（原 D3D11 `asset_identity`）；
  - `TextureCache<Native>`：按（资产 id，版本）的 LRU 索引，条目与字节有界，每个条目持有一份 CPU 租约；淘汰只取没有帧或 use 持有的条目（一份租约、无 pin）；
  - atlas 候选、带边图像与区域 uv（原 D3D11 `place_on_atlas` 与 `atlas_region` 的可移植部分）；
  - 缓存与 atlas 常量从 `constants::render::d3d11` 移到 `constants::render::texture_cache`。
- D3D11 渲染器改用它，行为不变：缓存计数的成员改名（`cache.hits`、`cache.peak_bytes`、`cache.entries` 等），探针随之更新，产物逐字节相同。
- Vulkan 缓存渲染器（`cache_assets`）：
  - prepare 先给所有缓存命中加租约，再为未命中的资产淘汰、创建；同一列表里重复的资产共用一个纹理；
  - 小覆盖率资产放进 R8 atlas 页面，页面按需创建，创建失败的页面下次再建；
  - 页面是一个跟踪器资源（`ResourceKind::AtlasPage`），帧把用到的页面列为依赖。它的“上传”是首次写入时那次从 `UNDEFINED` 出发、丢弃内容的布局转换：两个都未提交的录制若都首次写入同一页，先执行的那个写入的区域会被后一个丢弃，所以另一个录制正首次写入帧所需的页面时，encode 抛 `Busy`（C ABI 返回 `BUSY`）、什么都不录；已写过的页面从 `SHADER_READ_ONLY` 转换，保留其他区域；
  - 区域按页面偏移上传，记录按区域 uv 采样，按纹理视图命名使同页字形合批；
  - `drain()` 释放所有缓存纹理与页面，适配器 shutdown 时调用。

验证：
- `ctest --preset windows-release --parallel 6` 104/104，`windows-debug --parallel 6` 104/104（290.6 s）；产物与 10.4b 逐字节相同（计时套件除外，含 D3D11 缓存探针的计数）；
- `render_vulkan_contract`：缓存渲染器的字形串在 RTX 5090 与 AMD Radeon 核显上与独立纹理逐像素相同，一次绘制，第二帧不上传；drain 后区域全部归还；
- `render_vulkan_backend_contract` 的缓存场景：另一录制正首次写入页面时返回 BUSY 且不录命令，提交后再录不上传；在途的字形串不被其他字形淘汰（准备失败、不留租约），完成后最旧的三分之一被淘汰、区域被复用，留在缓存里的三分之二在其他字形写入同一页后仍正确，重新准备时只上传被淘汰的三分之一；销毁适配器后所有纹理与页面释放；同步验证零告警；
- 变异检查：7 个，6 个被发现（页面首次写入不等待另一录制、区域上传到页面原点、atlas 图像采样整页、淘汰取走在途纹理、drain 保留页面、命中未在淘汰前加租约）；“每次写页面都从 UNDEFINED 转换”未被发现：RTX 5090 与 AMD 核显的驱动在这种转换后都保留了 R8 页面的内容，像素测试观察不到，这条规则靠代码与注释保证。

**10.4d：Vulkan 交换链窗口与性能**：
- `vulkan::Device` 可选 `presentation`：平台的 surface 实例扩展（Windows `VK_KHR_win32_surface`，Android `VK_KHR_android_surface`）、`VK_KHR_swapchain`，以及能向窗口呈现的图形队列（Windows 上按 `vkGetPhysicalDeviceWin32PresentationSupportKHR` 选）。
- `vulkan::Swapchain`（`src/render/backends/vulkan/swapchain.*`）：在平台创建的 surface 上建 sRGB BGRA8/RGBA8 交换链，垂直同步用 FIFO，否则 mailbox 或 immediate；`acquire`/`present` 报告过期或次优，由宿主重建；`win32_surface()` 为 Win32 窗口建 surface。
- 示例宿主 `probes/vulkan_window_host.hpp`（`VulkanWindowPresenter`）：UI 经 C ABI 画进渲染尺寸的离屏目标（不透明黑底），再 blit 进交换链图像并留黑边；两帧在途，每个槽有命令缓冲、fence 与获取信号量，每张交换链图像一个呈现信号量；fence 完成后向渲染器报告完成 serial，并读出 UI pass 的时间戳。
- 探针模式（`weft_d3d11_probe`，有 Vulkan 后端时编译）：`--cheetah-flow-window-vulkan`（交互）、`--cheetah-flow-window-test-4k-vulkan`（隐藏自测）、`--cheetah-flow-window-perf-vulkan`（隐藏 4K 滚轮基准；`--no-validation` 关闭验证层）。React 应用、布局与显示列表与 D3D11 的 4K 窗口相同。新测试 `vulkan_cheetah_flow_window_4k`，没有能呈现的 Vulkan 设备时以 77 跳过。
- 渲染器的增量优化：完成的帧把记录、裁剪、暂存缓冲与描述符池交回渲染器复用（每类最多 4 个，缓冲不超过 16 MiB，新建时按 2 的幂留余量）；View 常量是渲染器的一个常驻缓冲；`draw()` 的回读缓冲首次调用时才建，窗口的截图缓冲首次截图时才建。
- 工具：`tools/benchmark_flow_window_apis.py` 交替运行三种模式并输出中位数；`tools/cheetah-flow.bat` 菜单打开 D3D11 或 Vulkan 窗口、运行基准与 Vulkan 自测。

验证：
- `ctest --preset windows-release --parallel 6` 105/105，`windows-debug --parallel 6` 105/105（290.0 s）；产物与 10.4c 逐字节相同（计时套件与新的 Vulkan 窗口套件除外）；
- `vulkan_cheetah_flow_window_4k`（RTX 5090，Release 与 Debug）：React 点击、滚轮映射、4K 内部留边不派发、空闲帧不重建 UI、1000×420 缩放后的黑边与滚轮映射；4K CPU 像素对照 3680 个样本，最大差 1；122 个纹理在完成后全部退休；验证层含同步验证零告警；
- 变异检查 5 个全部被发现：复用的缓冲可能过小、复用的描述符池不重置、View 常量宽高互换、超出上限的缓冲被保留（新增大图测试）、窗口留边不清黑。

性能（2026-10-12，AMD Ryzen 9 9950X3D + RTX 5090，Windows 11；Release；隐藏窗口，客户区与渲染都是 3840×2160；Roboto 与 Noto Sans CJK SC，三张 1024×1536 原图；60 个 DOM 元素；预热 8 帧后测 120 帧滚轮；FIFO / 同步间隔 1，隐藏窗口不受刷新率限制；三种模式交替各 3 次取中位数，连续运行，未控温。复现：`python -I tools/benchmark_flow_window_apis.py build/windows-msvc/bin/Release/weft_d3d11_probe.exe <输出目录> 3`）：

| 模式 | 帧率 | 每帧总时间 P50/P99/最大 | 编码 P50/P99/最大 | 提交与呈现 P50 | UI GPU P50 | 冷启动首帧 | 峰值工作集 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| D3D11（调试层开） | 211.8 fps | 4.54 / 5.30 / 5.90 ms | 0.072 / 0.116 / 0.118 ms | 0.099 ms | 0.106 ms | 20.2 ms | 109.6 MiB |
| Vulkan（验证层与同步验证开） | 216.7 fps | 4.53 / 5.41 / 6.05 ms | 0.178 / 0.347 / 0.389 ms | 0.087 ms | 0.089 ms | 176.6 ms | 240.7 MiB |
| Vulkan（关验证层） | 228.8 fps | 4.34 / 4.83 / 5.66 ms | 0.062 / 0.088 / 0.120 ms | 0.048 ms | 0.090 ms | 174.1 ms | 227.4 MiB |

- 两边每帧的主要成本都是 owner 的布局（约 4.0 ms）；编码与 UI GPU 时间只算 UI pass，可以直接比较。D3D11 窗口每帧还用着色器合成并画 FPS HUD，Vulkan 窗口用 blit 合成、帧率在标题栏，所以“提交与呈现”与绘制调用数（D3D11 每帧约 23 次，Vulkan 3 次）不能直接比较。
- 复用帧资源之前，Vulkan（关验证层）编码 P50 0.20 ms，帧率 199.7–208.4。
- Vulkan 冷启动首帧里约 130 ms 是 4K 交换链创建（`vkCreateSwapchainKHR`，在 NVIDIA 驱动内）；其余约 45 ms 与更高的峰值工作集来自半透明原图在 CPU 上转成线性浮点再暂存（D3D11 在 GPU 上转换）。这两项留作后续优化。

**10.6a：Android arm64 交叉编译**（先于 10.5：NDK 已获批准下载，交叉编译能尽早暴露可移植性问题；可移植文字与路径仍是下一段）：
- 依赖：Android NDK r30（30.0.16248370），按用户 2026-10-11 的批准下载，SHA-1 与官方页一致，解压在仓库之外；适用 2026-04-28 版 Android SDK License Agreement。
- `CMakePresets.json` 新增 `android-arm64`（Ninja，NDK 工具链文件，arm64-v8a，android-29，Release）。29 是 NDK 系统字体 API 的最低级别，也是 Android 普遍提供 Vulkan 1.1 的版本。
- Android 平台层 `src/platform/android`（`weft::platform_android`）：`system_font_catalog()` 用 NDK 的系统字体迭代器列出文件、集合序号、字重、斜体与变体轴；迭代器不给族名，由新的可移植函数 `font_family_names()`（`text/font_catalog.hpp`，FreeType 读 name 表：排版族名 ID 16，否则族名 ID 1，各语言去重）补上。通用族按 Chrome Android：serif Noto Serif，sans-serif 与 system-ui Roboto，monospace Droid Sans Mono；其余族按迭代顺序作回退。
- Vulkan：`android_surface()` 为 Android native window 建 surface（`surface_android.cpp`，只在 Android 编译）；设备的 presentation 选项在 Android 上启用 `VK_KHR_android_surface`。
- clang 下的警告：`-Wno-missing-field-initializers`（与 clang 的 designated 变体）——聚合初始化有意留成员给默认成员初始化；`document.cpp` 的 `&&`/`||` 加括号（语义不变）；删去 `scene.cpp` 未用的 `inset_edges`；QuickJS getter/setter 的函数指针经 `void(*)()` 转换，说明它在 QuickJS 的 union 里按 cproto 的签名回调。

验证：
- `cmake --preset android-arm64` 与 `cmake --build --preset android-arm64`：从干净构建 24 s，零错误零警告；36 个测试可执行文件是 AArch64 ELF；`weft_platform_android_smoke` 链接 Android 平台层与 Android surface，依赖 `libandroid.so`、`libdl.so`、`libm.so`、`libc.so`，不依赖 `libvulkan.so`；
- 未验证：没有 Android 模拟器或真机，可执行文件都没有运行过；Activity、输入、窗口与 Vulkan 呈现都只到编译与链接；iOS 需要 macOS，未做；
- Windows：`ctest --preset windows-release --parallel 6` 105/105，`windows-debug --parallel 6` 105/105（282.8 s）；`font_catalog_contract` 新增族名对照（Roboto、Ahem、Noto Sans CJK SC 的英文与中文记录去重为一个、非字体文件报错）；产物与 10.4d 逐字节相同；
- 族名的变异检查 3 个全部被发现：逐条记录不去重、按小端读 UTF-16、读不了的文件返回空列表。

**10.5a：scene 的圆角溢出裁剪**（10.5 的第一段：先去掉数量最多的回退原因）：
- `scene_display_list()`：`overflow: hidden/auto` 的盒子有圆角时，内容除轴对齐裁剪外再加一层圆角裁剪，取边框盒的世界矩形；镜像变换把每个角的半径移到它落下的角，半径按两轴缩放。子树的裁剪链是外层链加本层，存成 `DisplayList::clips` 里连续的一段（外层在前）。盒子自己的背景与不透明度组用父链，文字与子节点用本层链。只有旋转或斜切盒子的溢出裁剪仍整帧回退（原因 `rounded/transformed clip reference`）。
- `d3d11::SceneRenderer`：每个槽位有一个按最大 pass 增长的结构化裁剪缓冲（t2），录制前上传 pass 规范化后的裁剪；只有图片条目仍计为回退。链超过 16 层时规划器返回 `Diag::PaintList`，渲染器计为带原因的回退。
- 语义：scene 的溢出裁剪沿用原有实现、D2D 参考与命中共同的边框盒。CSS 规定裁剪在内边距盒、圆角取内圆角，盒子有边框时两者不同；10.5b 同时修正这三处。
- 回退次数：
  - gallery 预览 210 → 47：剩 SVG 33、投影变换 8、颜色串 3、阴影 3。阴影原先被同一帧更早遇到的圆角裁剪原因遮住。预览按真实时钟播放，总 pass 数每次略有不同（本次 562）；
  - scene-native 1452 → 1089；
  - cluster GPU 252 → 231：spring-02 的 7 张截图 × 3 个版本改为原生绘制。

验证：
- `ctest --preset windows-release --parallel 6` 105/105，`windows-debug --parallel 6` 105/105（233.1 s）。
- 产物变化只在三处（计时类报告与按真实时钟播放的预览除外），Debug 与 Release 的 scene 像素相同：
  - cluster GPU spring-02：与原先的 D2D 绘制相比 1882 个像素变化。最大差 96 在最右一列：根宽 518.656 CSS px，按 2 倍缩放是 1037.3 设备像素；原生的轴对齐裁剪按像素中心判定，与 Chrome 对齐到像素的溢出裁剪一致，D2D 则把这半列抗锯齿成 alpha 96。其余最大差 34，在文字与进度条两端的抗锯齿上。与浏览器参考的均差 0.153 → 0.246，全部来自这一列（参考图在那里是元素后面的页面背景）；IoU 0.9893 → 0.9888，在参考容差内；
  - scene-native 的 css-07、css-08、css-20（各 3 帧）：每张 540–700 个像素超过 2 级，最大 22–32，在圆角裁剪边缘与文字上。与 Chromium 参考的逐通道均差变化在 0.03 级以内，有升有降；全套最差均差 2.327 与最小 IoU 0.8197 不变；
  - gallery 预览：原生帧不做 D2D 的局部重绘，局部重绘 139 → 0、D2D 绘制程序复用 588 → 8。
- `scene_cluster_platform` 新增裁剪链合同：嵌套两层时内层链重复外层裁剪；背景与不透明度组取父链，文字与子节点取本层链；`scale(-2, 1)` 下各角半径换到镜像的角并按轴缩放；轴对齐裁剪取交集；旋转的溢出裁剪仍是可见的回退。
- 变异检查 8 个全部被发现：镜像不换角、半径不随缩放、文字留在父链、嵌套链丢掉外层、组合成取本层链、接受旋转的溢出裁剪（以上由新合同发现）；像素着色器不绑定裁剪缓冲、裁剪不上传（scene-native 与 cluster GPU 探针发现）。

**10.5b：溢出裁剪按 CSS 取内边距盒**：
- 共享函数 `scene_values::padding_box()`：溢出裁剪是内边距盒，各角内半径等于外半径减去相邻边框宽度（水平半径减左右边框，垂直半径减上下边框，宽度不等时成椭圆角），任一半径为零的角是直角（CSS Backgrounds 3 §5.3）；结果经 `normalize_rounded` 按 CSS 规则缩小重叠的半径。scene 的边框都是实线，所以每条边框宽度都计入。
- 四处同时改用它：原生显示列表（轴对齐裁剪与圆角裁剪）、D2D 参考的逐帧绘制与保留绘制程序（`clip_geometry`：每角椭圆的路径几何，按 factory 缓存）、可移植命中（`SceneGeometry::hit_map`）与 D2D 参考命中。盒子自身仍按边框盒命中；svg 视口照旧按边框盒裁剪。
- 探针报告：scene-native 与 cluster GPU 的 `report.json` 增加 `fallback_reasons`（与预览的 `render-work.json` 同一函数），剩余回退逐项列出：scene-native 为 SVG 968、投影变换 121；cluster GPU 为 SVG 189、颜色串 21、阴影 21；预览为 SVG 33、投影变换 8、颜色串 3、阴影 3。

验证：
- `ctest --preset windows-release --parallel 6` 105/105，`windows-debug --parallel 6` 105/105（308.1 s）。
- 合同：
  - `scene_cluster_platform`：四边宽度不同、两角圆角的盒子，裁剪矩形与各角内半径逐项对照；外半径小于边框时裁剪是直角矩形；
  - `scene_contract`：子元素盖满带 8px 边框与 16px 圆角的父元素，边框上与内外圆角之间的点命中父元素（可移植命中与 D2D 参考命中都对照），内边距盒里命中子元素；D2D 的 CPU 栅格与 GPU 目标模式（保留绘制程序，WIC 目标）在这些点上都不画出子元素。
- 新测试发现一处真实缺陷：GPU 目标换 factory 时没有清掉新的裁剪几何缓存，绘制时报 `D2DERR_WRONG_FACTORY`；已修正。
- 产物（与 10.5a 相比，计时类与按真实时钟播放的预览除外）：css-08、css-19、css-20 与 spring-02 有“带边框的溢出裁剪”，像素变化，三条路径（D2D CPU、D2D GPU、原生）一致变化，Debug 与 Release 相同；scene-native、scene GPU、cluster GPU 的最差均差与最小 IoU 不变。与 Chromium 参考比：
  - css-19-24（打字机光标是右边框）：文字不再画到边框上，变化像素上的误差 122 → 0（原生）、110 → 17（D2D）；
  - spring-02：变化像素上的误差 21.4 → 6.2；
  - css-08、css-20：进度条子元素 `rounded-full` 的左端与父元素的内圆角重合，两层抗锯齿的覆盖率相乘，比参考暗，变化像素上的误差约 6 → 22–39。Chrome 在几何完全相同的 spring-02 上也是相乘的结果，在 css-08、css-20 上却只取子元素自身的覆盖率，所以保留相乘，差异在参考容差内；
  - css-19-63：光标是小数位置的右边框，Chrome 把它对齐到整像素，这里按覆盖率画半列。原先文字盖住了这一列，现在露出来；是原有的对齐差异。
- 变异检查 9 个全部被发现：显示列表按边框盒裁剪、内半径不减边框、内圆角保持圆形、单个零半径的角仍是圆角（显示列表合同）；可移植命中与参考命中按边框盒、D2D CPU 栅格与保留绘制程序按圆角边框盒、换 factory 不清裁剪几何（命中与像素合同）。

**10.5c：原生显示列表补齐两处取值**（两处都是 D2D 参考早已支持、原生却回退的值）：
- 颜色：`#rgb` 简写按参考的方式展开。原生的颜色函数原来只认 `black`/`white`，当初为保持产物逐字节不变显式关掉了简写展开，没有语义理由。
- 渐变色标：长度色标（如 `radial-gradient(circle, #333 1px, transparent 1px)`）换算成渐变线上的比例，渐变线与着色器的参数化一致：径向取平铺的半对角线，线性取 |w·sinθ| + |h·cosθ|，与参考相同。圆锥渐变不接受长度，仍是可见的回退（`conic gradient length stop`）。为此平铺尺寸改在色标之前解析。

验证：
- `ctest --preset windows-release --parallel 6` 105/105，`windows-debug --parallel 6` 105/105（305.6 s）。
- 产物（与 10.5b 相比，计时类与按真实时钟播放的预览除外）：只有 cluster GPU 的 anime-09（吸附网格的点阵背景）7 张截图改为原生绘制，Debug 与 Release 相同；与浏览器参考的均差下降（如 anime-09-63 的红通道 0.057 → 0.046，变化像素上的误差 19.8 → 14.8）。cluster GPU 回退 231 → 210，预览 47 → 44；scene-native 不变。
- `scene_cluster_platform` 新增：`#3a7`、`#f00`、`#000` 分别作背景、边框与文字着色的取值；径向与线性长度色标的比例（线性用非正方形平铺，能区分轴向）；圆锥长度色标回退。
- 变异检查 5 个全部被发现：不展开简写、径向用整条对角线、线性交换两轴、圆锥接受长度、长度不换算。

**10.5d：原生阴影**：
- paint IR：新负载 `Shadow`（颜色、偏移、模糊、扩展，以及按指定值、尚未相互缩小的四角半径）与条目种类 `Item::Kind::Shadow`；`check()` 校验负载范围与数值（有限、模糊非负），`append()` 重新编号阴影。
- 可移植阴影图 `render::shadow_image()` 与 `shadow_geometry()`（`src/render/shadow_image.*`），从 D2D 参考原样移出：
  - 展开形状（盒子按 spread 外扩，半径同加）的抗锯齿遮罩，σ = blur·scale/2 的可分离高斯（与 Chromium 相同），四周留三个标准差加 2 个设备像素；
  - 只保留盒子以外的部分，输出预乘 RGBA8；超过 1024×1024 像素返回 `Diag::PaintList`。
- 共享解析 `scene_values::box_shadows()`：层数上限 8、`x y [blur [spread]] color`、`rgb(r g b / a)` 与 `#0000`、模糊与扩展上限 64px，全部与原 D2D 相同；内阴影仍报 `WEFT_SCENE_RASTER`。D2D 参考改用这两处共享代码，产物逐字节不变。
- 显示列表：阴影条目紧挨在盒子之下，最后列出的先画（CSS 先列出的在最上）；节点有透明度且阴影与背景或文字同时存在时，整个节点作为一组隔离。
- 规划器：阴影是一张自带颜色的图像，记录的变换平移到阴影矩形的原点，尺寸取矩形尺寸。D3D11 scene 渲染器按数值缓存阴影纹理（与文字、图层同一条目与字节上限，计数 `shadow_builds`/`shadow_reuses`）。文档路径（D3D11、Vulkan 与 C ABI）不接受阴影条目。
- 回退：cluster GPU 210 → 189（剩下全是 SVG），预览 44 → 41；scene-native 不变（SVG 968、投影变换 121）。
- 顺带：`two_frame_probe` 故意抛异常的 present 回调改为读 volatile 条件后抛出。原先 LTCG 重新生成 `TwoFramePipeline::submit` 时会报 C4702（该实例中 return 不可达）。

验证：
- `ctest --preset windows-release --parallel 6` 105/105，`windows-debug --parallel 6` 105/105（315.3 s）。
- 产物（与 10.5c 相比，计时类与按真实时钟播放的预览除外）：只有 cluster GPU 的 anime-11（投掷小球，`shadow-lg`）7 张截图改为原生，Debug 与 Release 相同。与原 D2D 的差别在圆的边缘与文字：编码 pass 的抗锯齿按 fwidth 计宽，对角边缘更软。阴影本身是 10% 的黑色。与浏览器参考：IoU 0.9589 → 0.9619，均差 0.1442 → 0.1484。
- scene-native 探针新增阴影对照：两层阴影（一层模糊带扩展，一层硬边红色）画在小数偏移的圆角盒下。盒外 3522 个阴影像素与 D2D 的 CPU 栅格相比，原生最大差 2，D2D GPU 目标模式为 0。报告新增 `shadow_oracle_pixels`、`shadow_oracle_max_difference`。
- 合同：
  - `paint_contract`：阴影负载与数值检查、`append` 重新编号；阴影图的几何，盒下透明、硬阴影在盒旁、模糊对称衰减、扩展变宽与位图上限；规划记录的矩形映射与标志；
  - `scene_cluster_platform`：阴影在盒子之下、最后列出的先画、透明度成组、透明层略去、内阴影回退、gallery 阴影条目的取值。
- 变异检查 11 个全部被发现：
  - 由合同发现：丢掉阴影、按列出顺序画、透明度不成组、记录不平移、记录用盒子尺寸、盒下也画阴影、σ 取 blur、接受负模糊、`append` 不重新编号；
  - 由 scene-native 探针发现：上传时不换 RGBA/BGRA、渲染器不准备阴影图（崩溃）。

**10.5e：路径进入 paint IR，可移植的矢量光栅**（SVG 原生绘制的第一段；生产者与渲染器在 10.5f 接入）：
- paint IR：
  - `Path`：Move、Line、Cubic、Close 与点列，外加填充规则；弧线与二次曲线由生产者转成三次曲线；
  - `Stroke`：宽度，butt/round/square 线帽，miter/round/bevel 连接，斜接上限（默认 4，超过时按 SVG 改为 bevel），虚线图案与偏移；
  - `PathPaint`：路径加一种颜色，带 `stroke` 时描边，否则填充；条目种类 `Item::Kind::Path`。
  - `check()` 要求路径以 Move 开头、动词恰好用完点列、点为有限值、描边长度有限且非负、斜接上限不小于 1；`append()` 重新编号。
- 新模块 `src/vector`（`weft::vector`，私有依赖 FreeType，已 vendored）：
  - `mask_bounds()`：路径在设备像素中能涂到的范围：点的包围盒，加上描边最远处（半宽乘斜接上限或方帽对角）和 1 个抗锯齿像素，再与裁剪相交；
  - `Rasterizer::rasterize()`：在这个范围内输出 8 位覆盖率遮罩；
- 光栅化的处理：
  - 曲线先由 `FT_Outline_Decompose` 按 0.01 设备像素细分成直线，再交给 FreeType 的光栅器（精确面积覆盖）。FreeType 自己按约 1/6 像素展平，10px 的圆面积会小 0.7%；
  - 描边由 FreeType 的描边器在路径自身的坐标里完成（坐标乘以变换的最大拉伸，保住 26.6 定点精度），再映射到设备，所以非均匀变换会像 SVG 一样拉伸线宽；
  - 虚线在这里按 0.1 设备像素展平后切出，每个子路径重新开始；奇数图案重复一遍；零长度的虚线在 round/square 线帽下画成点；
  - 遮罩超过 2048×2048 像素或 FreeType 轮廓上限（65535 点）时返回 `Diag::PaintList`。
- 规划器、C ABI 与文档渲染器在 10.5f 之前明确拒绝路径条目（不再可能在规划器的批处理里死循环）。

验证：
- `ctest --preset windows-release --parallel 6` 106/106，`windows-debug --parallel 6` 106/106（297.0 s）；产物与 10.5d 逐字节相同（计时类与预览除外）。
- 新测试 `vector_raster_contract`（可移植，Android 上同样编成 AArch64 可执行文件，未运行）：
  - 填充：小数边缘按面积覆盖（0.75 → 191 级）；10px 圆的面积误差在 0.3% 内；nonzero 与 evenodd 在嵌套方形上的差别；Close 之后的线段从原子路径起点重新开始；
  - 描边：butt、square、round 线帽的范围；miter、bevel、round 连接的外角；斜接上限 4 让约 6° 的尖角变成 bevel，上限 100 时尖角伸出；纵向放大 3 倍时线宽也成 3 倍；零长度子路径只在 round/square 线帽下成点；
  - 虚线：交替、偏移、每个子路径重新开始、奇数图案（含偏移落在重复的后半）、零长度虚线成点；
  - 边界与裁剪：遮罩外圈全为 0；按裁剪切出的遮罩与不裁剪时同位置的覆盖率相同；裁剪外为空；超限返回诊断；平移的小数部分移动的是覆盖率；
  - 1500 条随机路径（含零长度段、Close 后续画、随机变换与描边样式、虚线）不失败且覆盖率不出边界；200 个随机凸多边形的面积与精确面积相差不超过 0.5% + 0.5 像素。
- `paint_contract`：路径的结构校验与 `append` 重新编号；规划器拒绝路径条目。
- 变异检查 14 个全部被发现：忽略填充规则、不细分曲线、半径取整个线宽、超限的斜接改为截断、忽略虚线偏移、奇数图案不重复（为此新增偏移落在重复后半的用例）、零长度子路径不画线帽、边界不含描边范围、边界不含抗锯齿像素、行序上下颠倒、Close 后的线段不重新开始、路径可不以 Move 开头、`append` 不重新编号路径、负虚线通过校验。

**10.5f：SVG 进入原生显示列表**：
- `scene_display_list()` 生产 SVG 内容，与 D2D 参考的绘制顺序一致：
  - svg 元素是一个盒子：视口（viewBox 与 preserveAspectRatio）把用户坐标映射进它的盒子，内容按它的圆角边框盒裁剪；
  - SVG 元素：g 与嵌套 svg 只是分组；rect、circle、ellipse、path、polygon、polyline、line 成为路径条目，填充与描边各一条，颜色乘以 fill-opacity 或 stroke-opacity，line 不填充；
  - text 的字形轮廓成为一条路径，按 x、y、dy、text-anchor 与基线（central 时为行盒中线）放置，在设备空间按完整变换光栅化，旋转标签不再重采样；含彩色字形的文字仍是图像；
  - 元素 opacity 成组，只有一个条目且无子元素时由条目自己带透明度；
  - transform 属性（否则 CSS transform）按 SVG 语法、原点 0 0；clip-path 只接受一个用户空间矩形，有 rx/ry 时成为圆角裁剪；
  - 可见的回退：旋转或斜切下的 clip-path（`rotated SVG clip reference`）、未知元素（`SVG element …`）、没有文字引擎时的 SVG 文字（`SVG text reference`）。
- 新文件 `scene/scene_path.*`：
  - `scene_svg_path()` 把 SVG 形状转成 `paint::Path`：弧线按中心参数化后切成不超过 90° 的三次曲线，二次曲线升阶；宽或高不为正的矩形、半径不为正的圆不绘制（SVG 规定）；
  - 弧线的中心化计算从 `scene_geometry` 移出，命中测试与绘制共用；
  - `scene_svg_stroke()` 解析描边，斜接上限取 SVG 的初始值 4。
- 文字轮廓：`Font::outline()` 取无提示轮廓（与字形图片同样的 FreeType 加载方式）；`SceneText::outline()` 把一段文字的轮廓拼成一条路径，原点在行盒左上，按字形排版缓存同样的键缓存，以 8 倍字号加载以避开 1/64 像素的定点精度。
- 规划器：路径条目在设备像素上对齐的遮罩矩形由 `vector::mask_bounds` 在裁剪与目标内求出，存进 `ScenePass::masks`，记录逐像素对应纹素并以路径颜色着色。`weft::render` 私有依赖 `weft::vector`。
- D3D11 scene 渲染器：用 `vector::Rasterizer` 光栅化路径遮罩，按内容（路径、描边、设备映射、遮罩矩形）缓存纹理；`scene_display_list` 取渲染器的文字引擎。
- 着色器的两处量化修正：
  - 着色遮罩（文字、路径）与盒子一样先把颜色量化成 8 位预乘；
  - 量化时半级进位（`floor(x + .5)`）。HLSL 的 `round` 是四舍六入五成双，alpha 0.7 × 255 = 178.5 会取 178；参考端 D2D 的 `std::round` 与 Chromium 的 SkColor 都取 179。旭日图半透明扇区因此整片差 1 级，修正后与参考的均差 0.201 → 0.062。
- D2D 参考的 SVG 斜接改为 SVG 语义：`D2D1_LINE_JOIN_MITER_OR_BEVEL` 加上限 4（D2D 的上限与 SVG 是同一个比值）。原先是 D2D 默认的截断斜接与上限 10，比 Chrome 多出尖刺；anime-04 的参考图在这些像素上与浏览器的误差 140–157 → 0.7–6.2。
- 回退：
  - scene-native 1089 → 121，只剩投影变换；
  - cluster GPU 189 → 21、预览 41 → 12。剩下的是此前被 SVG 原因遮住的旋转盒子溢出裁剪（cluster 21、预览 4）与投影变换（预览 8）。
- scene-native 探针的覆盖要求改为现状：原生 3900 次以上、回退只有投影变换、路径图像确有构建。

验证：
- `ctest --preset windows-release --parallel 6` 106/106，`windows-debug --parallel 6` 106/106（310.4 s）；Debug 与 Release 的 scene 像素相同；Android 交叉编译零警告。
- 与浏览器参考的均差（最差通道，原 D2D → 原生）：
  - scene-native：d3-00 0.553 → 0.511，d3-01 0.519 → 0.515，d3-02 0.035 → 0.037，d3-03 0.111 → 0.094，d3-04 0.040 → 0.006，d3-05 0.040 → 0.020，d3-06 0.052 → 0.019，d3-07 0.099 → 0.104；
  - cluster GPU：旭日图 0.059 → 0.062，技能树 0.129 → 0.121，时间轴 0.197 → 0.185，散点 0.183 → 0.173，K 线 0.273 → 0.260，anime-04 0.250 → 0.251，图标 0.263 → 0.284 与 0.051 → 0.054（圆角描边边缘的抗锯齿）；
  - 全套最差均差 2.327 与最小 IoU 0.8197 不变。量化修正也作用于原有原生帧：scene-native 有变化的帧中 24 帧更接近参考，6 帧差 0.002–0.005，总误差 6.74 → 6.22；render-stages 有 2 个像素差 1 级。
- scene-native 探针新增 SVG 对照：原生与 D2D 的 CPU 栅格比，整帧均差 0.40 级，实心区域最大 1 级，超过 64 级的像素 4 个；帧内含 evenodd 星形、miter/round/bevel 连接、butt/round/square 线帽、虚线、旋转加透明度的组、非均匀缩放的椭圆描边、半透明填充。实心区域的 1 级差来自 CPU 参考把半级舍去；另加一条按 Chromium 规则直接算出期望值的半透明填充量化检查。报告新增 `vector_oracle_*` 字段，产物保存两张对照图。
- `scene_cluster_platform` 新增 SVG 合同：视口变换与圆角视口裁剪、组透明度、填充与描边（含 stroke-opacity）、虚线、line 不填充、clip-path 矩形、文字轮廓按锚点与基线放置、三类回退、d3.arc 式弧线的三次曲线不偏离圆（r=300 时误差 < 0.05）。
- `paint_contract`：路径条目的遮罩矩形与记录映射，裁剪外的路径没有遮罩。
- 变异检查 12 个全部被发现：
  - 由合同发现：不成组、忽略视口、clip-path 不裁剪、忽略 text-anchor、给 line 填充、视口裁剪丢掉圆角、忽略 stroke-opacity、文字回到图像、路径记录不平移；
  - 由 scene-native 探针发现：渲染器不准备路径图像、量化改回五成双、参考端改回截断斜接。

**10.5g：裁剪在自身空间**：
- `RoundedClip` 增加 `to_local`：从目标像素到裁剪自身空间的仿射，矩形与圆角都在这个空间里。轴对齐盒子的裁剪仍是恒等映射，文档路径不变。
  - `normalize_rounded` 拒绝非有限或奇异的映射；`check()` 同样校验；`append()` 把映射与追加变换的逆复合，矩形保持不变；
  - `contains_rounded` 先映射再判定，CPU 覆盖率遮罩与命中随之支持。
- 着色器的 Clip 记录从 48 字节增至 80 字节：增加映射的两行，`invX.w` 是裁剪空间一个单位对应的目标像素，用作抗锯齿尺度（恒等映射时为 1，像素与原来逐字节相同）。Vulkan 渲染器按 `sizeof(SceneClip)` 上传，无需改动。
- 生产者：旋转或斜切盒子的溢出裁剪、svg 视口裁剪与 clip-path 矩形都在盒子自身空间裁剪，映射取盒子变换的逆；目标空间的轴对齐裁剪只保留外接矩形。原先的回退原因 `rounded/transformed clip reference` 与 `rotated SVG clip reference` 消失。
- D2D 参考：不带圆角的 SVG rect 改用 `CreateRectangleGeometry`。零半径的圆角矩形会把描边外角画圆，Chrome 是尖角。
- 回退：cluster GPU 21 → 0（630 次全部原生），预览 12 → 8（只剩投影变换），scene-native 仍为投影变换 121。

验证：
- `ctest --preset windows-release --parallel 6` 106/106，`windows-debug --parallel 6` 106/106（316.9 s）；Debug 与 Release 的 scene 像素相同；Android 交叉编译零警告。
- 先只加映射、不改生产者时，全部确定性产物与 10.5f 逐字节相同。
- 产物变化：
  - cluster GPU 的 anime-07（进度环，原先因旋转裁剪回退）改为原生，与浏览器参考的均差 1.091 → 0.311；
  - 参考端直角描边的修正改变了 cluster 参考图里 d3_interactive-02 约 1040 个像素，与浏览器的误差不变（变化像素上 7.9 → 8.0）。
- scene-native 的 SVG 对照帧加入旋转 25° 的溢出裁剪盒（圆角、边框、超出的子元素）与旋转下的 clip-path：整帧均差 0.58，实心区域最大 1；超过 64 级的像素 9 个，在旋转盒黑边与白底的高对比斜边上（编码 pass 按导数的 L1 范数加宽斜边的抗锯齿），上限因此设为 16。另检查描边矩形的外角两边都是描边色。
- 合同：
  - `paint_contract`：带映射裁剪的记录（映射行与跨度）、恒等映射记录、奇异映射被规划器与 `check()` 拒绝、`append()` 复合映射后同一点落在同一处、`contains_rounded` 在自身空间判定；
  - `scene_cluster_platform`：旋转盒子的溢出裁剪是内边距盒加内圆角，映射把盒子原点映回原点；嵌套圆角裁剪旋转后内层在自身空间；旋转的 clip-path 在元素空间。
- 变异检查 8 个全部被发现：着色器忽略映射与参考端圆角化直角（探针发现）；跨度不随映射、包含判定忽略映射、追加不复合、奇异映射通过校验（paint 合同发现）；旋转盒按外接矩形裁剪、映射取正变换（cluster 合同发现）。

**10.5h：投影平面**：
- paint IR 增加 `Plane` 项：一份嵌套显示列表，先在自身像素里平铺绘制，再经单应性矩阵（列向量 3×3）映射到目标。
  - `check()` 递归检查内容，最多嵌套 `constants::max_plane_depth`（2）层；要求尺寸为正，矩阵有限且可逆，四角深度大于 `constants::plane_min_depth`，即都在观察者前方；
  - `append()` 把矩阵与追加的缩放平移复合。
- 生产者：transform 含 `perspective(` 的节点成为平面项。
  - 内容是以该节点为根的子树：节点移到原点并去掉 transform，由同一生产者递归列出；
  - 矩阵是父级仿射乘 `scene_plane(n)`，与参考端的平面相同；
  - 侧立平面（|cos| < 1e-5）与参考一样不画；
  - 第三层嵌套和角点在观察者身后的平面整帧回退，原因分别是 `plane nesting bound`、`plane behind the viewer`；
  - 平面内容按元素盒子裁剪，溢出部分不画。参考端相同，Chrome 会画，已记入兼容表。
- 规划器：平面项按图像投影处理。图像是内容加 2 px 透明边距，按设备像素绘制。
  - 记录存图像映射的逆矩阵：前两行在 invX/invY，第三行（深度）在 outline，mode.x = 3；
  - 包围盒取投影后的四角与裁剪的交；
  - 边距的角若在观察者身后，规划器拒绝该项（`plane image behind the viewer or singular`）。
- 着色器：投影模式下除以深度，深度不为正时不画。透明边距让平面边缘经双线性过滤抗锯齿。
- D3D11 渲染器分两阶段：
  - 准备阶段递归规划每一层并建好所有图像。所有可能回退的步骤都在动用任何目标之前完成；
  - 绘制阶段先把每个平面的内容画进自己的图像（BGRA8，与组目标同格式），再由外层 pass 采样；
  - 平面图像与其内容中的组目标计入 scratch 预算，随槽位退休；
  - 平面内容每帧重画，尚无缓存。
- D3D11 文档渲染器、Vulkan 渲染器与 C ABI 拒绝平面项，C ABI 返回 `WEFT_RENDER_UNSUPPORTED_FEATURE`。
- 回退：scene-native 121 → 0（4120 次全部原生，平面 pass 123 次），预览 8 → 0，cluster GPU 保持 0。

验证：
- `ctest --preset windows-release --parallel 6` 106/106，`windows-debug --parallel 6` 106/106（310.4 s）；Android 交叉编译零警告。
- 产物：
  - 除 css-21 的三张截图与新增的平面对照外，确定性产物与 10.5g 逐字节相同。css-21 是翻牌卡片，原先整帧回退；
  - 预览与窗口截图随时间变化，10.5f 与 10.5g 两个基线之间同样不同；
  - css-21 三帧与浏览器参考的均差 0.132/0.133/0.133 → 0.128/0.140/0.135（帧 24/63/120），约 800 个像素变化，都在卡片边缘。
- scene-native 新增平面对照帧：圆角带边框的平面及其子元素、平面内嵌套的平面、透明度下的渐变平面、侧立平面。
  - 与 D2D CPU 参考相比，整帧均差 0.675，实心区域最大差 0；
  - 超过 64 级的像素 299 个，都在平面投影后的边缘：参考端采样平面时边缘不抗锯齿；
  - 上限设为均差 1.0、实心 2、边缘 400；
  - 非原生（D2D GPU）路径在同一帧上均差 0.612、实心 1、边缘 298；
  - 侧立平面的位置在两边都保持白色。
- 退休探针：16 次重入一个内容含组的平面。槽位存活时恰好持有 2 × 408 × 408 × 4 字节（平面图像加组目标），退休后为 0。
- 合同：
  - `paint_contract`：
    - 平面项的检查：越界、无内容、非有限、空尺寸、奇异、在观察者身后、内容递归、两层嵌套通过而第三层拒绝；
    - 追加后同一点落在变换后的位置；
    - 规划记录的逆映射把目标点带回图像点，包围盒含边距四角；
    - 边距在身后时列表合法，规划器拒绝。
  - `scene_cluster_platform`：
    - 透视盒子成为平面，内容在自身像素，矩阵含父级变换，平面带父级的圆角裁剪链；
    - 侧立平面不画，身后平面与第三层嵌套回退，平面内可嵌套平面。
- 变异检查 15 个全部被发现：
  - 着色器不除深度、嵌套 pass 不留边距、外层不绑定平面图像（探针发现）；
  - 图像映射不含边距、记录缺深度行、尺寸不含边距、检查放过身后或奇异平面、检查跳过内容、追加不复合矩阵、规划器画身后的边距（paint 合同发现）；
  - 内容保留盒子偏移、矩阵缺父级变换、侧立平面被列出、生产者列出身后平面（cluster 合同发现）；
  - 槽位 scratch 漏算平面（退休探针发现）。

**10.5i：显式图层原生绘制**：
- 显式图层的图像改由原生 pass 绘制，替代 D2D 参考光栅。
  - 节点放在透明边框里（每边 2 px），列出显示列表，由 10.5h 的嵌套 pass 画进图层自己的图像；
  - 图像仍按视图、节点身份、绘制源与缩放缓存，帧间复用；
  - 准备与绘制都在外层 pass 动用目标之前完成，嵌套 pass 的缓冲与组目标只服务这一次，由 D3D11 保活到 GPU 用完。
- 原生路径中只剩彩色字形文字经 D2D 参考光栅。新计数 `reference_builds` 记录参考光栅画出的图像。
- 图层的接受范围不变：图层内的 SVG、投影平面以及超出盒子的文字或子元素，仍整帧回退（`explicit native layer bounds reference`）。
- 取舍：图层里的文字改用与其他原生文字相同的 FreeType 覆盖率，不再是 DirectWrite。
  - 含图层的 scene 与浏览器参考的均差略升：css-09 为 0.168/0.175 → 0.180/0.186，css-15 为 0.264/0.335 → 0.267/0.342；
  - 门限是 3 级，仍有很大余量；
  - 换来的是原生路径只有一种文字实现，可以搬到 Vulkan 与移动端。

验证：
- `ctest --preset windows-release --parallel 6` 106/106，`windows-debug --parallel 6` 106/106（305.6 s）；Android 交叉编译零警告。
- 产物：
  - 变化只在含图层的画面：scene-native 的 css-09、css-15 各三帧，变化都在字形边缘；render-stages 的 retained 截图约 140 个像素，最多 11 级；
  - 其余确定性产物与 10.5h 相同。
- 新增图层对照：同一节点作为显式图层（整数设备像素偏移）绘制，与其内容内联绘制比较。
  - 原生路径逐字节相同，最大差 0；
  - D2D 路径最大差 1；
  - 上限设为 1。
- scene-native 与 render-stages 要求参考光栅只画彩色字形文字（`reference_builds == text_builds - text_portable_builds`）。预览中参考光栅画了 8 个彩色字形文字图像，render-stages 的 159 个图层图像全部原生绘制。
- 变异检查 5 个全部被发现：
  - 嵌套 pass 不画图层、图层内容不留边距（浏览器参考的容差发现）；
  - 参考光栅画图层，计数与不计数各一（图层对照发现；计数时 render-stages 的计数要求也发现）。

**10.5j：可移植的彩色字形**：
- 文字模块新增软件画布 `ColorCanvas`（`src/text/color_canvas.hpp`）：
  - 裁剪按精确面积覆盖：边累加有符号面积（font-rs 的做法），越出画布左右的边沿画布边竖直走，曲线按 0.01 px 展平；
  - 填充有纯色和 COLRv1 的三种渐变：线性（按 p0p2 旋转）、双圆径向（取最大参数）、扫掠；扩展方式有 pad、repeat、reflect；
  - 渐变按 OpenType COLR 的规定插值预乘颜色；
  - 组按 COLRv1 的 28 种合成模式合成（Porter-Duff、W3C 分离与 HSL 混合），只作用在当前裁剪内；
  - 颜色在编码 sRGB 的预乘浮点中混合，与 Chrome、DirectWrite 一致；
  - 画布不超过 512² 像素，组最多嵌套 32 层；超出的组不建，并报告溢出。这些调用来自 C 库的回调，所以不抛异常。
- `Font::rasterize_color`：经 HarfBuzz 的 paint 接口遍历字形的 COLRv1 图或 COLRv0 层，画在画布上。
  - 字体缩放取 em，坐标就是字体单位，由画布的基础变换换到像素，并带亚像素原点；
  - 彩色字体里没有 COLR 的字形，以前景色画出轮廓；
  - PNG 或 SVG 图像字形（CBDT、sbix、SVG 表）画不了，返回空，由调用方换路径。
- scene 文字图像：彩色字形按字形顺序互相 source over，叠在其他字形之上，再转回直通色。`color_glyphs` 现在只表示有画不了的图像字形；只有这种文字还走 D2D 参考光栅。
- 结果：
  - 原生路径在所有测试里都不再使用 D2D 光栅：预览中参考光栅的图像从 8 降为 0，文字图像 103 个全部可移植；
  - scene-native 与 render-stages 改为要求 `reference_builds == 0`。

验证：
- `ctest --preset windows-release --parallel 6` 107/107（新增 `color_canvas_contract`），`windows-debug --parallel 6` 107/107（239.9 s）；Android 交叉编译零警告。
- 与 DirectWrite（D2D 参考）的对照：
  - 文字图像中 emoji（🔥☠ 加文字）的预乘通道均差：1x 为 1.56，2x 为 0.93；alpha 软 IoU 为 0.960、0.976；
  - GPU 彩色文字对照帧（🔥⚔💎☠ 与文字）整帧均差 0.44，实心区域最大差 1，超过 64 级的像素 4 个；
  - 非原生（D2D GPU）路径同一帧差为 0。
- 与浏览器参考：
  - cluster 的 spring-01（含 emoji）改为原生后，各帧 alpha IoU 都升高（如 0.949 → 0.959），均差不变；
  - scene-native 的 css-11（剑、盾、宝石）均差 0.081/0.047/0.083 → 0.083/0.047/0.087，变化只在边缘抗锯齿。
- 其余确定性产物与 10.5i 相同。
- 合同 `color_canvas_contract`：
  - 矩形与嵌套裁剪、圆盘面积（误差 0.2% 内）、越出画布的边和在像素行中进入画布的边、变换与退化空间；
  - 线性渐变及 p2 旋转、同心与双圆径向渐变（取较大的圆）、扫掠方向、三种扩展、预乘插值；
  - 合成模式：SrcOver、DestOut、SrcIn、Clear、Multiply、Screen、Plus、Overlay、Luminosity，合成限于裁剪内，组嵌套上限；
  - Segoe UI Emoji 的 🔥 有多种颜色，亚像素原点平移墨迹，彩色字体里的轮廓字形用前景色。
- `scene_text_image`：
  - 彩色字形画出，并与参考光栅对照；
  - 重叠的彩色字形按顺序合成：后一个不透明处显示它自己的像素。
- 变异检查 12 个全部被发现：
  - 线性渐变忽略 p2、径向取较小的圆、直通色插值、reflect 当 repeat、扫掠反向、合成越出裁剪、overlay 当 hard light、边不在画布边拆分、忽略亚像素原点、忽略前景色（画布合同发现）；
  - 文字图像不画彩色字形、彩色字形相加不按 alpha（文字图像合同发现）。
  - 后两项最初未被发现：边的变异起初改的是拆分后的钳位，与原代码等价，改为去掉拆分；重叠合成原先没有测试，补了重叠字形的检查。

**10.6a：原生 scene pass 的可移植准备层**（Vulkan scene pass 的前一步，行为不变）：
- 新库 `weft::scene_native`（`src/scene/scene_native.hpp`）承担原生 scene pass 中与图形 API 无关的一半：
  - 编译显示列表、检查、规划；
  - 文字图像、阴影图和路径覆盖率等采样像素，按内容缓存，键、预算和清空策略与此前 D3D11 渲染器相同；
  - 投影平面和显式图层的嵌套 pass。图层的内容 pass 留在缓存里，后端第一次用到时才画进自己的图像。
- 每个图像和图层带唯一 id。后端按 id 保存纹理；缓存和正在采样它的 pass 都不再持有源对象时，回收该纹理。
- 计数 `SceneNativeWork` 在各后端相同。
- D3D11 渲染器只剩 GPU 部分：按 id 上传纹理、首次用到时画图层、嵌套 pass 和绘制提交。槽位改为租用图像与图层对象。
- 只有字形是 PNG/SVG 图像的文字仍经 D2D 参考光栅，由 D3D11 以回调提供。没有参考光栅的后端把它记为回退原因 `color glyph image reference`。

验证：
- `ctest --preset windows-release --parallel 6` 108/108（新增 `scene_native_contract`），`windows-debug --parallel 6` 108/108（240.0 s）。
- Android 交叉编译零警告，`scene_native` 在 Android 上编译。
- 全部确定性产物与 10.5j 逐字节相同；图层、平面、复用和清空计数也相同。
- 合同 `scene_native_contract` 覆盖：
  - 准备树的图像与平面，平面 pass 的尺寸与组深度；
  - scratch 字节：目标的组，以及平面图像和其中的组；
  - 缓存命中保持同一图像与 id，改动文字得到新 id；
  - 图层内容 pass 及其复用，id 唯一，回退原因计数。
- 变异检查 3 个全部被发现：
  - 图层借用前一图像的 id（浏览器参考容差发现）；
  - 未画过的图层不画（render-stages 发现）；
  - scratch 漏算平面图像（新合同发现）。这一项起初未被发现，补了合同之后才发现。
- 有一次全量 Release 运行中，4K 实时预览 `d3d11_warp_gallery_preview` 失败，没有留下信息。之后两次全量和四次单独运行都通过。此后全量运行改用 `--output-on-failure`，以便留下证据。

**10.6b：Vulkan 的 scene pass**：
- 新库 `weft::scene_vulkan`（`src/render/backends/vulkan/scene_renderer.hpp`）复用 `ScenePreparer`，用共享 scene 着色器的编码变体（SPIR-V，不带 `SPACE_LINEAR`）绘制：
  - 实例化记录，组画在暂存图像里；
  - 平面与显式图层由嵌套 pass 画进各自的图像；
  - 文字、阴影、路径图像按 id 上传一次，源对象不再存在时回收；
  - 所有目标都是编码预乘 BGRA（`B8G8R8A8_UNORM`），与 D3D11 scene 目标相同。
- `draw()` 是同步的：在自己的命令缓冲上记录一帧，提交并等待。
  - 返回时设备已不再使用这一帧的任何资源，帧内的缓冲、组图像、平面图像和描述符池随之销毁；
  - 失败时撤掉本帧加入缓存的图像；
  - 接入宿主命令缓冲并按完成 serial 退休的异步路径还没有做。
- 共享的 Vulkan 记录辅助（`vulkan/pipeline.hpp`）：着色器模块、图像屏障、清除/加载渲染 pass、预乘 source-over 管线、描述符集布局、采样器与描述符池。显示列表渲染器改用它们，行为不变，原有 Vulkan 测试全部通过。
- 着色器：编码 pass 也改用 fine 导数。原先用 `fwidth`，而 Vulkan 由实现决定 coarse 还是 fine。
  - D3D11 的编码产物随之变化，都在抗锯齿边缘：cluster 136 张、scene-native 64 张；
  - 与浏览器参考相比，分别有 88 张、40 张更近，39 张、23 张更远，变远的最多 0.0003、0.0005 级均值；
  - 顺带消除了 coarse 导数让盒子角像素变暗的问题：css-00 右下角从 160 恢复到 212，与 Chrome 一致。
- 新探针 `vulkan_scene_native`：26 CSS + 8 D3 的 102 个采样帧逐帧与 D3D11 WARP 原生 pass 比较，并与 Chromium 参考比较；开启验证层与同步验证。
  - RTX 5090：
    - 102 帧全部在 Chromium 容差内，最差均差 2.45，最小 IoU 0.82；
    - 与 D3D11 超过 2 级的像素共 162 个，最多 19 级。这些是 css-20 条纹进度条硬渐变停止点上的像素，GPU 的舍入决定它们落在哪一边；
    - css-25 有 5.4 万个像素红通道比 D3D11 高 1 级，原因是 8 位混合在半级处的舍入，它的 Chromium 均差因此从 2.33 升到 2.45；
    - 验证消息 0；平面 pass 3 次，图层构建 6 次、复用 12 次。
  - AMD Radeon 核显（`--adapter AMD`，手动运行）：与 D3D11 最多差 3 级，超过 2 级的只有 1 个像素，最差 Chromium 均差 2.328，验证消息 0。
  - 上限：与 D3D11 最多差 24 级，超过 2 级的像素不超过 400 个。

验证：
- `ctest --preset windows-release --parallel 6` 109/109（新增 `vulkan_scene_native`），`windows-debug --parallel 6` 109/109（240.1 s）。
- Android 交叉编译零警告，`scene_vulkan` 与编码 SPIR-V 都在 Android 上编译。
- 变异检查 7 个全部被发现：
  - 不画平面、缩放丢失、首实例丢失（Chromium 容差发现）；
  - 组图像继续而不清除、平面图像未转为采样布局（验证层发现）；
  - 编码 pass 改回 `fwidth`（与 D3D11 的差异上限发现）；
  - 新图层内容的图像不上传（缺纹理发现）。
- 未验证：Android 设备上的运行（没有 SDK 和模拟器）。

**10.6c：Vulkan scene pass 接入宿主提交**：
- `vulkan::SceneRenderer::encode(slot, frame, command, target, …)` 把一帧记录进宿主的命令缓冲。
  - 这一帧用到的东西都放在槽位里：缓冲、暂存图像、描述符池、目标的帧缓冲、所采样图像与图层的租约，以及本次上传或绘制进缓存的条目；
  - 宿主提交后调用 `submitted()`，完成后调用 `retire()`，从不提交的记录调用 `discard()`。
- 缓存中的图像只在源对象不再存在时回收。准备层的缓存和正在采样它的槽位都持有源对象，槽位只在完成后才释放，所以回收时没有在途的帧还在使用它。
- 一次记录上传或绘制进缓存的内容，在提交之前处于待定状态。
  - 另一次记录如果要采样它，就抛出 `Busy`，并且不记录任何命令（与显示列表渲染器的图集页处理相同）；
  - 被丢弃的记录，其上传随之离开缓存；
  - 记录按记录顺序提交。
- `draw()` 改为在自己的命令缓冲上走这一序列：记录、读回、提交、等待、退休。

验证：
- 新合同 `render_vulkan_scene_contract`：
  - 三帧在途、三个槽位，宿主自己提交和退休，8 帧与 `draw()` 的结果逐字节相同；
  - 采样未提交的上传时得到 `Busy`；上传所在的记录提交后（仍在执行），新的记录可以进行，两次结果相同；
  - 被丢弃的记录的上传，在下一次记录时重新上传；
  - 开启同步验证，零告警。
  - 在 RTX 5090 与 AMD 核显（`--adapter AMD`，手动运行）上都通过。
- `vulkan_scene_native` 的结果不变。
- `ctest --preset windows-release --parallel 6` 110/110，`windows-debug --parallel 6` 110/110（240.7 s）；Android 交叉编译零警告。
- 变异检查 4 个全部被发现：
  - 提交后不清除待定、丢弃的记录不移出缓存（都表现为 `Busy`）；
  - 从不抛 `Busy`（`Busy` 合同发现）；
  - 槽位不租用所采样的图像（验证层报告图像视图在使用它的命令完成前被销毁）。这一项靠三帧在途才暴露：两帧在途时，槽位总在它的图像被回收之前完成。

**10.6d：Vulkan 的 scene 窗口与槽位复用**：
- 新探针模式 `--scene-vulkan-window`（交互）与 `--scene-vulkan-window-test`（自测），`tools/gallery.bat` 第 7、8 项。自测是新 ctest `vulkan_scene_window`，没有能呈现的 Vulkan 设备时以 77 跳过。
  - 窗口一次显示一个 gallery scene（26 CSS + 8 D3），按时钟动画。
  - 每帧用 `encode()` 把 scene 记录进窗口自己的命令缓冲，画进与窗口等比的目标，再居中复制进交换链图像。BGRA8 sRGB 交换链直接接收编码字节。
  - 两帧在途，fence 完成后 `retire()`。
  - 点击、空格或右方向键切到下一个 scene，左方向键上一个，Esc 关闭；标题栏显示当前 scene。
- 槽位复用：一次记录结束后，槽位的缓冲、暂存图像与描述符池留作之后记录的备用（最多 64、16、16 个），槽位析构时销毁。
  - 缓冲取容量够的最小备用；新建时至少 64 KiB，按 2 倍增长，可作 pass 的任何用途。View 常量的描述符范围改为常量本身的大小，因为复用的缓冲可能超过 uniform 缓冲的范围上限。
  - 暂存图像按尺寸完全相同取用。
  - 描述符池取容量够的最小备用，并重置。
  - 新计数 `slot_objects`：为槽位新建的缓冲、暂存图像与池。

验证：
- 窗口自测（RTX 5090，验证层含同步验证）：34 个 scene 各显示 12 帧（固定时钟）。每个 scene 的最后一帧从交换链图像读回，与同一帧 `draw()` 的结果逐字节相同；零告警。
- `render_vulkan_scene_contract` 新增：三个槽位各自第一次记录之后，后续记录不再新建任何对象。
- `vulkan_scene_native` 不变：
  - RTX 5090 的统计与 10.6b 相同：超过 2 级的像素 162 个，最多 19 级；
  - AMD 核显（手动运行）的 102 张输出与 10.6b 逐字节相同。
- `ctest`：
  - `--preset windows-release --parallel 6`：110/111。`d3d11_warp_gallery_preview` 失败（见下方观察），单独重跑通过；
  - `windows-debug --parallel 6`：111/111（246.0 s）；
  - Android 交叉编译零警告。
- 变异检查 4 个全部被发现：
  - 备用缓冲不论大小都取用（`vulkan_scene_native` 越界访问崩溃）；
  - 备用暂存图像不论尺寸都取用（`vulkan_scene_native` 的 css-21 超出 Chromium 容差）；
  - 描述符池复用而不重置（合同中分配描述符集失败）；
  - 槽位不保留备用（合同的复用检查）。

性能测试条件：
- 硬件与系统：2026-10-12，AMD Ryzen 9 9950X3D + RTX 5090，Windows 11；Release。
- 画面：可见窗口，交换链 640×420。
- 样本：gallery 的 34 个 scene 各 12 帧，共 408 帧；固定时钟，关闭垂直同步。
- 连续运行，未控温。
- 复现：`weft_d3d11_probe --scene-vulkan-window-test <目录> [--no-validation]`。

| 验证层 | scene 首帧 P50 / 最大 | 其余帧 P50 / P99 / 最大 |
| --- | --- | --- |
| 开（含同步验证） | 1.62 / 10.7 ms | 0.70 / 25.1 / 26.4 ms |
| 关 | 0.77 / 11.1 ms | 0.23 / 21.4 / 24.5 ms |

- 帧时间从取得帧槽算到呈现返回，包括 owner 的 scene 步进、准备、记录与提交。
- 尾部全部来自 D3 的 d3-04 至 d3-06，慢帧的时间都花在 `encode` 里。临时计时显示：
  - 回收过期的缓存图像（源对象已不存在）时，每张图像单独 `vkFreeMemory`，约 0.18 ms 一张；d3-05 每 4 帧回收 100–110 张，约 19 ms；
  - 新建图像（`vkAllocateMemory` 等）约 0.06 ms 一张；d3-05 每帧新建 33 张路径图像，约 2 ms；
  - D3D11 驱动自己做子分配，没有这一成本；
  - 下一段改为小图像共享设备内存块。

观察：
- `d3d11_warp_gallery_preview` 在全量并行运行中第二次失败，第一次在 10.6a，当时没有输出。
- 这次的失败点：4K gallery 像素 oracle 第 2 帧 (861, 1754) 的红色通道，期望 186、实际 173。该点在 d3-04（D3 Animated Bars）一根柱子的圆角边缘。
- d3-04 的数据来自 `Math.random`，QuickJS-ng 以当前时间（`gettimeofday`）做种子。所以每次运行的柱高不同，网格采样点偶尔落在边缘上。
- 偏差的来源尚未查清：GPU 与 oracle 都是线性预乘的双线性采样。下一步先让 oracle 失败时保存证据。

**gallery 预览 oracle 的偶发失败（10.6d 之后）**：
- 背景：4K gallery 预览自测的像素 oracle 检查外壳的合成。
  - 64 个 scene 中，30 个非核心 scene 用的是 GPU 回读的像素；
  - 34 个核心 scene（26 CSS + 8 D3）用的却是 CPU 参考光栅（D2D）。这是原生 pass 之前的写法，当时核心 scene 的 GPU 图像也由 D2D 画出，两边完全一致。
- 原因：
  - 10.5 之后核心 scene 改由原生 pass 绘制，两种光栅器在抗锯齿边缘相差可达 14 级，而 oracle 的容差是 3 级；
  - d3-04 的柱子每次运行都不同（QuickJS-ng 的 `Math.random` 以时间为种子），固定的网格点 (861, 1754) 偶尔落在柱子的竖直边缘上。
- 证据：
  - 并行 6 份的压力运行，第 1 轮就复现（期望 83、实际 69）；
  - 失败时保存的对照图显示，偏差是整列边缘像素一致的。
- 修复：
  - oracle 对每个可见 scene 都采样 GPU 画出的像素（回读），只检查外壳的合成；
  - scene 自身的像素由各自的参考测试覆盖：原生 pass 对 Chromium 参考，Vulkan 对 D3D11；
  - oracle 失败时保存证据：整帧、失败点附近 oracle 与帧的并排对照，以及所采样的 scene 图像。
- 验证：修复后并行 6 份、10 轮共 60 次运行无失败；修复前 6 次中有 1 次失败。

**10.6e：Vulkan 小图像共享设备内存块**：
- `vulkan::RangeHeap`（`src/render/backends/vulkan/memory.*`）管理一块设备内存里的区间：
  - 分配时按对齐取第一个放得下的空闲区间；
  - 释放时与相邻的空闲区间合并。
- `vulkan::Device::image()`：
  - 不超过 2 MiB 的图像放进 16 MiB 的共享块；块按内存类型分开，每种类型最多保留一个空块；
  - 更大的图像仍然单独分配；
  - 块里只放 optimal tiling 的图像，不涉及 bufferImageGranularity；
  - 块的记账有互斥锁。
- 新计数：`memory_allocations()` 是累计的设备内存分配次数，`image_blocks()` 是当前的块数。窗口自测报告这两项。

验证：
- 新测试 `render_vulkan_memory_heap`：
  - 确定性用例：恰好放满、对齐留下的空隙、拒绝、重复释放抛异常；
  - 随机用例：6 轮各 3000 步的随机分配与释放，每一步都要求已分配区间与空闲区间恰好铺满整个堆，且相邻的空闲区间都已合并。
- 新测试 `render_vulkan_memory_device`（RTX 5090；AMD 核显手动运行）：
  - 随机尺寸的 500 张图像随机创建与销毁；
  - 每张存活图像清成自己的颜色后全部读回，各自保持不变；
  - 350 张存活图像只用了 12 次分配：1 个读回缓冲、9 张大图、2 个块；
  - 全部销毁后保留一个空块；
  - 验证层含同步验证，零告警。
- `vulkan_scene_native` 的输出（RTX 5090 与 AMD 核显）与 10.6d 逐字节相同；窗口自测通过。
- `ctest`：
  - `--preset windows-release --parallel 6`：113/113；
  - `windows-debug --parallel 6`：113/113（242.8 s）；
  - Android 交叉编译零警告。
- 变异检查 10 个全部被发现：
  - 堆合同发现 6 个：释放的区间不与前一个空闲区间合并、不与后一个合并、忽略对齐、丢掉对齐前的空隙、丢掉区间后的剩余、剩余与区间重叠；
  - 设备测试发现 4 个：销毁图像不释放区间、不共享块、唯一的空块也释放、图像绑定在块的起点（读回发现图像互相覆盖）。

性能测试条件：
- 硬件与系统：2026-10-12，AMD Ryzen 9 9950X3D + RTX 5090，Windows 11；Release。
- 画面与样本：窗口自测，交换链 640×420，34 个 scene × 12 帧共 408 帧，固定时钟，关闭垂直同步。
- 对照：“之前”是同一份代码把共享上限设为 0（每张图像单独分配，与 10.6d 相同）。两份程序交替各运行 3 次，取中位数；连续运行，未控温。

| 验证层 | 版本 | 每帧均值 | scene 首帧 P50 / 最大 | 其余帧 P50 / P99 / 最大 | 设备内存分配 |
| --- | --- | --- | --- | --- | --- |
| 关 | 之前 | 0.78 ms | 0.84 / 9.2 ms | 0.24 / 22.7 / 25.3 ms | 807 |
| 关 | 之后 | 0.33 ms | 0.62 / 8.9 ms | 0.23 / 1.10 / 1.37 ms | 27 |
| 开（含同步验证） | 之前 | 1.38 ms | 1.65 / 10.3 ms | 0.72 / 24.4 / 27.3 ms | — |
| 开（含同步验证） | 之后 | 0.92 ms | 1.46 / 10.0 ms | 0.71 / 2.72 / 4.49 ms | — |

- 设备内存分配次数是单次运行的累计值。
- 剩下的首帧尖峰（约 9 ms）是 css-11 的首帧：可移植准备层排版并光栅化它的文字，与图形后端无关。

**10.6f：Vulkan 适配器支持宿主纹理（外部纹理扩展）**：
- 新的原生载荷 `weft_render_vulkan_texture`（`include/weft/render_backend_vulkan.h`）：导入时借用宿主的 `VkImageView`，并带上 `VkDevice` 以便校验。
  - Vulkan 不能查询视图的属性，所以格式、尺寸与用法由宿主保证，测试中由验证层检查。
  - 格式：线性 RGBA8（`R8G8B8A8_UNORM`）、RGBA16F；RGBA32F 只在设备能线性过滤它时提供（核心 Vulkan 不保证 32 位浮点的线性过滤）。
  - 同步：宿主写纹理的命令在同一队列上先于每个采样它的帧提交，把图像留在 `SHADER_READ_ONLY_OPTIMAL`，并让写入对片元着色器的读取可见；帧的命令只读它。适配器不加全局屏障，否则每帧前都要排空管线。
- `vulkan::Backend`：
  - `enable_external(capacity, bytes)` 安装扩展；
  - 导入时校验载荷与设备，拒绝同一视图的第二次导入；
  - 显示列表帧与四边形帧都可以采样宿主纹理（下标排在资产之后）。
- `vulkan::Renderer`：帧带外部视图；批处理把每个宿主纹理当作单独的纹理；行从底部开始的纹理翻转记录的 uv。
- `Device::filters()`：格式能否被线性过滤采样。

验证：
- 新合同 `render_vulkan_external_contract`（RTX 5090；AMD 核显手动运行）：
  - 每种提供的格式各一张宿主纹理，外加一张行从底部开始的 RGBA8，由宿主在同一次提交中先写入；
  - 显示列表帧与四边形帧中，每个像素与宿主写入的纹素（按目标的 sRGB 编码）相差不超过 1 级，其余像素透明；
  - 调用方租约在 prepare 之后释放，纹理在帧完成时恰好退休一次；退休后同一视图可以再次导入；
  - 拒绝：另一个设备的载荷、缺视图、别的版本、过短的载荷、编码格式、同一视图的第二次导入、超出容量；没有被帧使用的导入在释放租约时立即退休；
  - 验证层含同步验证，零告警。
- `ctest`：
  - `--preset windows-release --parallel 6`：114/114；
  - `windows-debug --parallel 6`：114/114（240.9 s）；
  - Android 交叉编译零警告。
- 变异检查 8 个全部被发现：
  - 渲染器忽略从底部开始的行；
  - 宿主纹理的图片都绑定第一个视图；
  - 宿主纹理共用一个批处理名；
  - 适配器接受另一个设备的纹理；
  - 同一视图导入两次；
  - 释放的导入不腾出槽位；
  - 适配器丢掉 bottom-left 原点；
  - 帧的宿主纹理顺序颠倒。

风险：
- 渲染器把半透明的 CPU 资产上传成 `R32G32B32A32_SFLOAT` 并线性过滤采样。
- 核心 Vulkan 不保证这种过滤，部分移动 GPU 不支持；Android 上运行前需要改为能过滤的格式，或按设备选择。

## 第 11 步计划（2026-10-12）：UI 着色器效果、粒子与背景拷贝

用户决定：WebGL/WebGL2 与两例 Three.js 移出需求，3D 由宿主引擎渲染、经外部纹理合成；烟雾、发光、粒子这类小型效果在 UI 内做。着色器语言用 HLSL，挂接方式用 CSS。设计见 [架构](architecture.md)。

| 段 | 内容 | 验证 |
| --- | --- | --- |
| 11.1 | 效果基础：效果 ABI、离线编译工具、Effect 绘制项、D3D11 与 Vulkan 渲染器 | 输出可精确预测的测试效果在两个后端逐像素验证；裁剪、透明度、组合与等价图片路径对照 |
| 11.2 | CSS 挂接：`-weft-effect()`、自定义属性参数、注册与诊断、帧调度、scene pass | 样式与诊断的反例；固定时钟下的像素 |
| 11.3 | 示例效果与验收：烟雾、发光、溶解 | 4K 下的 GPU 时间；拟以 CRPG UI 的 nested tooltip 页面替换 gallery 中移出需求的两例 Three.js |
| 11.4 | UI 粒子 | 固定时钟下的像素；粒子数与 4K 性能 |
| 11.5 | 背景拷贝，然后 `backdrop-filter: blur()` | 组内与最外层的语义反例；宿主目标可读性的诊断 |

11.2 的顺序（11.1 后调查确定）：只有 scene 路径（gallery 的 CSS/D3 卡片与 cluster 的 React 页面）把 `background-image` 变成绘制项，文档页面不支持背景图片。所以先做 scene 路径：
- 11.2a：效果注册表与清单（参数名、类型、默认值，纹理，是否用时间）；scene 样式解析 `-weft-effect()`，从自定义属性取参数；未注册的名字与类型不符的参数给诊断；`scene_display_list` 按“背景色、效果、边框”的顺序输出；用时间的效果在可见时让帧继续推进。
- 11.2b：D3D11 与 Vulkan 的 scene 渲染器画效果：编码空间的着色器与管线，效果纹理的上传、缓存与按完成退休。
- C ABI 的效果特性位、文档页面的背景效果之后再做。

验收调整（用户 2026-10-12）：第一部分（11.1–11.3）以 CRPG UI 的 nested tooltip 页面验收。
- 页面替换 gallery 中的 webgl-02/03；去掉调参面板；源码按之前的方式只读捕获，烟雾在副本中改用 `-weft-effect`，原工程不改。
- 页面用到而尚未支持的能力一并补上：scene 文字换行、`position: fixed`、点线边框、`text-transform`、`tabular-nums`、`vh`、`overflow-y`、`#root` 之外的 portal、`Element.closest`、模板字符串里的 Tailwind 类名提取等。

11.3 的顺序（2026-10-13，调查页面与画廊管线后确定）：
- 11.3a：烟雾效果。原版 `SmokeBg` 的公式写成 HLSL，与 CPU 上照搬的原版 tick 逐像素对照。
- 11.3b：scene 内联排版。纯内联内容的块按 Chrome 的规则换行：跨元素的空白合并、UAX 14 断行、行盒高度（strut 与各内联框）、`text-align`；内联框按行分片（左右边只画在首尾片），命中与 `getClientRects` 按片；`inline-block` 与 SVG 作为原子内联按基线对齐；常见块元素（`h1`–`h6`、`p` 等）的 UA 样式；`text-align`、`white-space` 改为继承。用 Chrome 的逐行几何验证。
- 11.3c：CSS 能力：`position: fixed`、点线与虚线边框、`text-transform`、`font-style`、`font-variant-numeric: tabular-nums`、`vh`/`vw`、`overflow-x`/`overflow-y`、`-webkit-user-select`。
- 11.3d：运行时：`#root` 之外的 portal（scene 根改为 body）、`Element.closest`、页面自己的样式表、模板字符串与数据里的 Tailwind 类名提取。
- 11.3e：页面进画廊替换 webgl-02/03；Chrome 参照用原版画布烟雾；验收悬停嵌套、按住 3 秒钉住、拖动钉住的窗口。

用户决定（2026-10-13，11.3 完成后）：第 11 步继续做到 11.5；11.5 以画廊新增的一组半透明窗口验收（毛玻璃：模糊背后、半透明底色、饱和度与高光边；普通半透明；只模糊背后、不加底色），每例若干叠放、可拖动、可增减的窗口，背景每帧都在动，带窗口性能基准与 Chrome 像素对照。交互性能的三项优化留在待办。

11.4 的顺序（2026-10-13，按 scene pass 的现有结构确定）：
- 11.4a：粒子绘制项与渲染器。
  - paint IR：`ParticleEmitter`（发射形状、速度、方向、重力、阻力、寿命、角度与自转、尺寸与颜色随寿命的曲线、柔和圆点或精灵图集帧）、`ParticleEmission`（流的已过时间与停止时间，或爆发的数量与时长，种子，可选的目标点）、`Item::Kind::Particles`；`check` 与 `append`。
  - 无状态：每个粒子由发射序号、种子与已过时间解析得出；CPU 参考实现与着色器用同一组公式（整数哈希的随机数、线性阻力与重力的解析解、四点曲线）。
  - 一次发射是一个项、一条记录：参数放在效果参数所用的 16 个字段，精灵的区域与模式放在 uv 与 mode；绘制为一个实例的 6×N 个顶点，`SV_VertexID / 6` 是粒子序号，记录序号仍取自实例，与盒子相同。
  - 着色器：`scene.hlsl` 的 `WEFT_PARTICLES` 变体，顶点阶段计算粒子的四边形，像素阶段画圆点或精灵并乘颜色、裁剪链与透明度；D3D11 与 Vulkan 的 scene 渲染器各加一种批次。线性 pass、C ABI 与 D2D 参考光栅暂不画粒子，给诊断。
  - 验证：固定时钟下两个后端的像素对 CPU 参考；4K 下不同粒子数的 GPU 时间。
- 11.4b：挂接。
  - 发射器按名字注册（发射器与精灵）；CSS `-weft-particles: name` 在元素显示时持续发射，部分参数取自定义属性；JS `weft.emitParticles(element, name, options)` 一次性爆发，可让粒子飞向另一元素；
  - owner 维护每个元素的发射记录（开始、停止、过期后删除）；有存活粒子时帧持续推进，损伤按粒子能到的范围；每次发射与每帧的粒子数有上限和诊断。
  - 验证：发射记录的生命周期、参数、损伤与帧推进的反例；JS 爆发；固定时钟的像素。
- 11.4c：粒子的效果外观（效果的像素函数画每个粒子）；画廊里一个粒子演示与 4K 性能。
  - 取舍（2026-10-13，做 11.4c 时）：效果外观推迟，先做演示与性能。它要给每次发射多一条参数记录（发射器已占满效果参数的 16 个字段），并为每个效果离线编译“粒子顶点阶段 + 效果像素函数”的变体（每个后端、每种合成空间），两个后端各加管线；圆点与精灵图集已能画出火焰、烟、金币、火花与光环，没有页面需要它。之后有页面需要时再做。

11.5 的顺序（2026-10-13，11.4 完成后按 scene pass 的结构确定）：
- 11.5a：背景拷贝与 `backdrop-filter`。
  - CSS：scene 的 `backdrop-filter: none | <filter-function>+`：`blur(<length>)` 与颜色函数（`saturate()`、`brightness()`、`contrast()`、`grayscale()`、`invert()`、`opacity()`、`sepia()`、`hue-rotate()`）；其余函数与 `url()` 报诊断。
  - paint IR：`Item::Kind::Backdrop` 与 `DisplayList::backdrops`；`check` 拒绝非有限值、超过上限的模糊与不在组里的 `under`。
  - 规划器：`Step::Backdrop` 结束当前批次；渲染器把读的目标上元素外接矩形内的像素拷到临时纹理，按 Skia 的做法模糊（σ 大于 4 个设备像素时先缩小拷贝，11.5b 起按面积平均缩到 Skia 选的尺寸；水平、垂直两遍高斯，半径 3σ，边缘镜像），再按元素的圆角形状、裁剪链与透明度画回；颜色矩阵在编码空间、非预乘颜色上。
  - 语义：组内只拷组已经画的内容；最外层拷 pass 已经画的内容；宿主目标不可读（Vulkan 没有 `TRANSFER_SRC` 的图像）时背景透明并计入原因。
  - D3D11 与 Vulkan 都实现；D2D 参考光栅与线性 pass 拒绝；带 `backdrop-filter` 的子树不缓存成图层；有它的帧任何变化都整幅损伤。
  - 验证：两个后端对 CPU 参考（同一算法）的像素；组内与透明度的反例；不可读目标；层与损伤；CSS 值与诊断。
- 11.5b：与 Chrome 对照：静态用例（模糊半径、边缘、圆角、颜色函数、组内）在 Chrome 截图对照，定下边缘模式与容差。
- 11.5c：画廊的半透明窗口组（毛玻璃、普通半透明、只模糊背后）：每例若干叠放、可拖动、可增减的窗口，背景每帧在动；窗口自测、4K 性能基准（窗口数与 GPU/CPU 时间）、Chrome 逐帧像素对照。

## 第 11 步记录（2026-10-12）

**11.5c：画廊的半透明窗口组与 4K 性能**（2026-10-13）：
- 页面（`examples/crpg-gallery/native/GLASS_TESTS.js`、`glass-utilities.css`、`glass-keyframes.css`，新）：手写、不用框架，在 scene 平台上运行。三例：
  - 毛玻璃：`backdrop-filter: blur(18px) saturate(180%)`，14% 白色底色，顶边更亮的边框与一条高光线，外阴影；
  - 普通半透明：60% 的深色底色、边框与外阴影；
  - 只模糊背后：`backdrop-filter: blur(14px)`，无底色、无边框。
  - 背景每帧在动：四团漂移的光、转动的圆环、滚动的色块带，加一行静止的大字。窗口按序号散布在页面上；按标题栏拖动并置顶，拖动限在页面内，指针离开页面或取消时结束；− / + 在 1–32 之间增减（− 删最新的）。`__ceGlassPage` 给探针设定窗口数、读各窗口的标题与位置。
- 画廊：窗口新增 “Translucent Windows” 一组，三例各为一个 scene（第 67–69 个，1596×800），共 75 例、13 组，原生绘制 71 例；`window_section_count` 与 `window_page_count` 分开。窗口自测在第 76–92 帧跳到这一组：
  - 像素 oracle 与截图（`group-12.png`）；
  - 真实排队的拖动把毛玻璃例最下面的窗口拖到 (288, 168) 并置顶（owner 审计 `__ceGlassPage.state()`）；真实排队的点击 + 加到 5 个窗口；
  - pass 做了背景拷贝（`render-work.json` 新增 `backdrop_passes`），截图 `glass-drag.png`。
  - 静态包重建次数的上限改为命名常量 33：新组的两次跳转使重建从 28 次变为 30 次。
- Chrome 对照：
  - 用例（`examples/crpg-gallery/glass-cases.json`，新）：121 帧；第 40–60 帧拖动最下面的窗口，第 80、84 帧点 +，第 100 帧点 −；在第 0、36、72、120 帧截图并记录页面状态。
  - 截图（`tools/browser_glass_page_reference.mjs`，`tests/fixtures/glass-reference`，新）：本机 Chrome 155.0.8059.40 headless，页面时钟与脚本指针两个参考时钟逐帧步进；`#root` 与画廊卡片相同，盒模型与 scene UA 表一致。报告记录浏览器、GPU 与所用文件的 SHA-256（按 LF 行尾计算，`verify_gallery_fixture.py` 同样按 LF 核对，与检出时的行尾无关）。
- 测试 `scene_glass_page`（新）：
  - 行为：三例各 4 个窗口及其位置；+ 的位置（窗口自测用的常量）；毛玻璃与只模糊每个窗口一次背景拷贝，普通半透明没有；背景一帧之内有 24–32 万个像素变化而窗口不动；+ / − 与 1–32 的边界；按标题栏置顶并拖动，按窗口内容不拖动，拖动限在页面内，离开页面与取消都结束拖动；
  - 与 Chrome：12 个截图帧的窗口顺序与位置相同（差 0 px）；整帧按画廊 scene 规则（每通道平均差 0.71–0.96 级）；窗口的空白玻璃（避开文字与控件 3 px、边框与圆角弧 4 px）平均差 0.62–1.00 级，毛玻璃与只模糊最大差 6–11 级。普通半透明后面是清晰的内容，其边缘抗锯齿不同，只限平均差；
  - D3D11 WARP 与 Vulkan（RTX 5090）三例按画廊规则一致：最大差 3，超过 2 级的 4 个像素。
- 发现与修正：
  - Vulkan 每个背景遍各用一个缓冲放常量，而槽位只保留 64 个备用缓冲与 16 张备用临时图像。32 个玻璃窗口约需 128 个缓冲与 160 张临时图像，多出的每帧销毁再重建，渲染 CPU 15.2 ms。改为一次记录的所有背景遍常量放进同一个缓冲（按设备的 uniform 偏移对齐），槽位的临时图像按数量（256）与字节（64 MiB）保留：2.9 ms。
  - 粒子与玻璃基准的 Vulkan 计时一直开着验证层（含同步验证），目标图像也不可读（背景走透明分支）。现在关验证层计时，目标带 `TRANSFER_SRC` 并给出图像，并核对没有计入任何原因；验证仍由测试覆盖。11.4a、11.4c 的 Vulkan CPU 数字因此偏高，已在其记录中更正。
- 4K 性能（`weft_d3d11_probe --glass-page-benchmark-hardware <dir>`，`glass-page.json`；`d3d11_warp_glass_page_benchmark` 在 WARP 上只查它能跑）：
  - 条件：RTX 5090、Ryzen 9 9950X3D、Release；页面按 1920×1080 CSS px 排版，缩放 2 画进 3840×2160，窗口 360×232 CSS px（720×464 设备像素）；每档先跑 1 s 页面时钟，之后每帧推进 1/60 s。背景每帧都变，所以每帧都重新拷贝并过滤每个玻璃窗口后面的内容。热身 30 帧、采 240 帧；桌面照常使用（未隔离），GPU 55→60 °C、2.4–2.9 GHz；进程峰值工作集 296 MiB。
  - owner 一步（定时器、rAF、快照与 scene 更新）计 CPU；pass 计 CPU 与 GPU 时间戳。

| 例 | 窗口 | owner p50 / p99（ms） | D3D11 CPU p50 / p99 | D3D11 GPU p50 / p99 / 最慢 | Vulkan CPU p50 / p99 | Vulkan GPU p50 / p99 / 最慢 |
| --- | --- | --- | --- | --- | --- | --- |
| 毛玻璃 | 1 | 0.200 / 0.383 | 0.282 / 0.467 | 0.217 / 0.445 / 0.791 | 0.361 / 0.546 | 0.172 / 0.394 / 0.741 |
| 毛玻璃 | 4 | 0.300 / 0.562 | 0.447 / 0.651 | 0.291 / 0.871 / 0.874 | 0.562 / 0.831 | 0.287 / 0.513 / 0.862 |
| 毛玻璃 | 8 | 0.432 / 0.748 | 0.671 / 0.956 | 0.384 / 0.618 / 0.927 | 0.814 / 1.267 | 0.431 / 0.653 / 1.395 |
| 毛玻璃 | 16 | 0.853 / 1.428 | 1.148 / 1.362 | 0.573 / 0.813 / 1.150 | 1.500 / 2.155 | 0.726 / 1.289 / 1.298 |
| 毛玻璃 | 32 | 2.021 / 3.606 | 2.385 / 3.291 | 0.943 / 1.615 / 1.766 | 2.863 / 4.239 | 1.317 / 1.900 / 2.326 |
| 普通半透明 | 32 | 2.850 / 5.006 | 2.512 / 3.977 | 0.453 / 0.705 / 0.895 | 2.457 / 3.744 | 0.497 / 1.058 / 1.447 |
| 只模糊 | 32 | 2.004 / 3.449 | 2.231 / 3.070 | 0.749 / 1.311 / 1.327 | 2.712 / 3.998 | 1.059 / 1.618 / 1.645 |

  - owner 列是 D3D11 那一遍的；Vulkan 那一遍相近（毛玻璃 32 个窗口 2.12 ms）。普通半透明与只模糊的 1–16 个窗口见 `glass-page.json`，趋势相同。
  - 第一档的第一帧（启动尖峰）渲染 CPU 130 ms（D3D11）/ 160 ms（Vulkan）：着色器、管线与首次上传；之后各档的第一帧不超过 21 ms。
  - 半透明本身在 GPU 上便宜：32 个窗口时毛玻璃比普通半透明多 0.5 ms（D3D11）/ 0.8 ms（Vulkan）。CPU 随窗口里的元素（每个约 15 个）与每帧在动的背景增长，三种窗口相近，与交互性能优化的待办是同一类开销。未隔离，同一档两次运行的 owner p50 可差约 2 倍（例如普通半透明 16 个窗口 0.75 ms 与 2.18 ms）。
- 验证：13 个变异全部被发现（页面、样式、scene 选项、owner 审计、Vulkan 常量偏移）；`ctest` Release 与 Debug 各 143/143；Android 零警告。
- 限制：Chrome 对照只在本机的 Chrome 155 与 RTX 5090；有背景拷贝的帧任何变化都整幅损伤；移动端未验证。

**11.5b：`backdrop-filter` 与 Chrome 截图对照**（2026-10-13）：
- 用例（`tests/fixtures/backdrop-reference/cases.json`，新）：160×120 的条纹背景（五色竖条、深色底与一条白色横条）上的 11 个用例，见兼容性。截图（`tools/browser_backdrop_reference.mjs`，新）：本机 Chrome 155.0.8059.40 headless，经 CDP 逐用例设定设备缩放，两个 rAF 之后截图；页面是 data URL，不联网。`manifest.json` 记录浏览器、GPU（首个为绘制所用：RTX 5090，驱动 32.0.16.1062）与特性状态、`cases.json` 与每张截图的 SHA-256；重新截图逐位相同。
- 测试（`scene_backdrop_reference`，新）：同样的元素与内联样式经 scene 求解器与原生 D3D11 WARP pass 按用例的缩放画出，与截图逐像素比较。玻璃外（元素盒外扩 2 px）最大差不超过 2 级；玻璃内（内缩 2 px）的平均差、最大差与差 16 级以上的像素比例不超过各用例的上限（约为实测值加 50% 或几级的余量）；没有上限的用例算失败。截图清单的 `cases_sha256` 必须等于 CMake 配置时算出的 `cases.json` 哈希（`cases.json` 列为配置依赖），改了用例而没有重新截图时失败。并排对照图写到 `artifacts/backdrop-reference-<config>/`。
- 发现与修正：
  - Chrome 在元素边界镜像（Chrome 129 起的边缘模式），与 11.5a 相同；在视口边界镜像的模型差得多。组与嵌套的语义相同（平均差 0.07、0.04）。
  - 宽模糊偏暗：逐级双线性 2× 降采样在奇数尺寸上采样位置不对齐，丢失平均亮度，`blur(20px)` 平均差 9.16、最大 17。用 Python 模型对照了几种做法（`blur(20px)` 平均差）：直接 σ20 高斯 2.77；按面积缩到 Skia 的尺寸 2.81；Skia 式多级双线性缩放 6.03；线性光下多级 3.43；2 的幂块对齐 4.88。
  - 选按面积缩小：与直接高斯一样准，代价约为每个源像素读一次，与缩放比例无关。`ScenePass::Backdrop` 改为缩小后的尺寸与那里每轴的 σ（`size`、`sigma`），尺寸为每轴 `max(1, floor(边长 × 4/σ))`；`backdrop.hlsl` 的 `BACKDROP_AREA` 先横后竖，每个输出像素是它覆盖的源像素按覆盖比例的均值；CPU 参考同步；`blur(20px)` 改后 2.81 / 10。
- 结果（玻璃内平均 / 最大差，级）：blur-3 0.03 / 1，blur-20 2.81 / 10，blur-8-scale-2 1.57 / 7，rounded 1.50 / 9，saturate 2.33 / 12，grayscale-first 1.43 / 7，edge 1.59 / 5，opacity 0.83 / 4，group 0.07 / 2，nested 0.04 / 1，frosted 0.60 / 25（圆角边框的抗锯齿，0.08% 的像素差 16 级以上）。玻璃外全部为 0。
- 限制：截图来自一台机器上的一个 Chrome 版本；剩下的差异来自 Skia 的缩放方式与圆角抗锯齿，没有逐位复刻；硬件 GPU 上的原生结果由 `render_scene_backdrop_contract` 对 CPU 参考检查（Vulkan RTX 5090）。

**11.5a：背景拷贝与 `backdrop-filter`**（2026-10-13）：
- CSS（`src/scene/scene_filter.hpp/.cpp`，新）：`scene_backdrop_filter` 解析滤镜函数，`blur` 之前的颜色函数合成模糊前的矩阵，之后的合成模糊后的矩阵；诊断见兼容性。求解器在构建元素时校验（scene profile 诊断），SVG 内部元素上为诊断。`backdrop-filter` 进 scene profile（不继承）；它使元素成为固定与绝对定位子孙的包含块（`contains_fixed`）。
- paint IR（`src/paint/display_list.hpp`）：`ColorMatrix`、`Backdrop`（模糊、两个矩阵、圆角、`under`）、`Item::Kind::Backdrop`、`DisplayList::backdrops`；`check` 与 `append`。
- 显示列表（`scene_display_list.cpp`）：一般元素在阴影、背景之前输出 Backdrop 项；透明度小于 1 或子孙也有 `backdrop-filter` 的元素成为隔离组，Backdrop 项（`under`）放在组尾；行内元素只有各片段的盒子带它。子树带它时不缓存成图层。损伤：帧里有它时任何变化整幅损伤。D2D 参考光栅拒绝（`WEFT_SCENE_RASTER: backdrop-filter`）。
- 规划器（`scene_pass.hpp/.cpp`）：`Step::Backdrop` 与 `ScenePass::backdrops`（区域、模糊前矩阵、缩小后的尺寸与那里每轴的 σ（11.5b 起；原为 2× 降采样次数与剩下的 σ）、`under`）；记录：盒子的形状、模糊后矩阵（`effect_parameter` 0–4）、区域的设备 px（uv），`image_mode_backdrop`。`backdrop_*_constants` 给出各遍的常量（D3D11 与 Vulkan 共用）。
- 着色器：`backdrop.hlsl`（新）：全屏三角形，缩小（11.5b 起按面积）、高斯一遍、颜色矩阵三个变体，镜像采样；`scene.hlsl` 的合成模式按设备坐标采样模糊结果，乘圆角形状、裁剪链与透明度，再过矩阵。
- D3D11：`SceneRenderer::backdrop` 从目标的资源 `CopySubresourceRegion`，各遍画进按格式、尺寸与链中位置复用的临时纹理（两帧不用就释放），之后恢复 pass 的状态；`under` 用 destination-over 混合。线性 pass 与 C ABI 拒绝（`UNSUPPORTED_FEATURE`）。
- Vulkan：每次背景结束渲染过程，把读的图像转成 `TRANSFER_SRC` 拷出，各遍在自己的渲染过程里画进槽位的临时图像（用法加上拷贝），再以 load 继续；`Target::image`（新）让宿主给出可读的图像，没有时背景透明并计入 `backdrop of an unreadable target`；`draw()` 的 `readable` 参数供测试。新增镜像采样器、背景的描述符布局与三条管线、destination-over 的 scene 管线。
- 验证：
  - `render_scene_backdrop_contract`（新）：D3D11 WARP 与 Vulkan（RTX 5090）对 CPU 参考（同一算法，`tests/support/backdrop_reference.hpp`，背景取 pass 自己画的无元素帧）：blur 3px、20px（11.5b 起缩到 18×14）、缩放 2 下 8px、圆角、模糊后 saturate 与 brightness、模糊前 grayscale、目标边缘，相差不超过 2 级；透明度 0.6 带内容的元素（组与 `under`），内容之外为背景按透明度合成，内容处为内容按透明度；透明度组里的背景只读组的内容（组外的条纹不进背景）；Vulkan 不可读目标的背景透明并计入原因；Vulkan 对 D3D11 最大差 2；
  - `scene_backdrop_contract`（新）：滤镜函数与矩阵、组合顺序、封顶与单位、诊断；显示列表中各情形的位置（普通、透明度组、子孙也过滤）；固定定位子孙的包含块；图层；损伤；D2D 拒绝；求解器的诊断；svg 根与 SVG 内部元素；
  - `paint_contract`：上述拒绝、`append`、记录、步骤与常量；`render_d3d11`：C ABI 拒绝。
- 限制：有背景的帧任何变化都整幅损伤；读背景的 UI 着色器效果尚未做。

**11.4c：画廊的粒子页面与 4K 性能**（2026-10-13）：
- 页面（`examples/crpg-gallery/native/PARTICLE_TESTS.js`、`particle-utilities.css`、`particle-keyframes.css`，新）：手写、不用框架，在 scene 平台上运行。篝火（`-weft-particles` 的火焰、余烬与烟）、宝箱（每 2.5 s 自己打开一次，点击也开：火花，加上飞向顶栏金币计数的 24 枚金币，金币到达后计数增加）、符文（光环，悬停时 `:hover` 把 `--particles-rate` 从 60 提到 240）、全页漂浮的微粒（四层，每层每秒至多 12000 个，按页面设定的密度分配；底部按钮减半或加倍）。`__ceParticlePage` 给探针设定密度、开箱、读点击次数。
- 粒子（`probes/gallery_particles.hpp`，新）：flames、embers、smoke、coins（32×32 sRGB 金币精灵）、sparks（32×32 覆盖率四芒星）、aura、motes；精灵在代码里画。
- 画廊：窗口新增 “UI Particles” 一组（第 66 个 scene，1596×800，与提示页面同尺寸），共 72 例、12 组，原生绘制 68 例。目录外页面列在 `gallery_window_pages`（清单），D2D 参考光栅画不了的 scene 由 `gallery_reference_raster` 判断。窗口自测在第 62–72 帧跳到这一组：像素 oracle 与截图（`group-11.png`），真实排队的点击打开宝箱，owner 审计点击次数，帧里有飞行的金币，篝火区域有亮像素（`particles-loot.png`）。
- 验证：
  - `scene_particle_page`（新）：挂载时四个流；2.5 s 自动开箱的火花与金币，金币的目标是计数图标中心（按两者的几何算）；1.45 s 后计数为 24；点击开箱；悬停符文时流因覆盖值重启；密度 30000/s 分到三层（12000、12000、6000），帧里约 15 万个粒子；D3D11 WARP 与 Vulkan 按画廊 scene 的规则对照（RTX 5090：最大差 4，超过 2 级的约 80 个像素），同一时钟的两次运行帧相同、像素相同，去掉粒子后有 1.7 万个像素变化，飞行中的金币（宝箱盖与锁之外的 13 枚；盖与锁是宝箱的内容，画在它的粒子之上）都画在模型算出的位置，篝火里有亮像素；
  - `d3d11_warp_gallery_preview`：上面的窗口步骤；`d3d11_warp_particle_page_benchmark`：基准在 WARP 上能跑。
- 4K 性能（`weft_d3d11_probe --particle-page-benchmark-hardware <dir>`，`particle-page.json`）：RTX 5090、Ryzen 9 9950X3D、Release；页面按 1920×1080 CSS px 排版，缩放 2 画进 3840×2160；每个密度先跑 6 s 页面时钟（微粒最长活 5 s），之后每帧推进 1/60 s：owner 一步（定时器、rAF、快照与 scene 更新）计 CPU，pass 计 CPU 与 GPU 时间戳；热身 30 帧、采 240 帧；桌面照常使用（未隔离），GPU 57→61 °C、2.7–2.9 GHz；进程峰值工作集 180 MiB。

| 微粒/秒 | 帧内粒子 | owner p50 / p99（ms） | D3D11 CPU p50 / p99 | D3D11 GPU p50 / p99 / 最慢 | Vulkan CPU p50 / p99 | Vulkan GPU p50 / p99 / 最慢 |
| --- | --- | --- | --- | --- | --- | --- |
| 300 | 1 838 | 0.098 / 0.280 | 0.120 / 0.287 | 0.124 / 0.684 / 0.690 | 0.600 / 0.998 | 0.108 / 0.307 / 0.674 |
| 3 000 | 15 345 | 0.097 / 0.213 | 0.116 / 0.236 | 0.129 / 0.576 / 0.687 | 0.511 / 0.775 | 0.111 / 0.332 / 0.343 |
| 12 000 | 60 348 | 0.097 / 0.231 | 0.117 / 0.212 | 0.148 / 0.703 / 0.863 | 0.496 / 0.710 | 0.126 / 0.348 / 0.723 |
| 24 000 | 120 343 | 0.099 / 0.438 | 0.120 / 0.277 | 0.174 / 0.576 / 0.653 | 0.505 / 0.709 | 0.151 / 0.384 / 0.724 |
| 48 000 | 240 338 | 0.097 / 0.275 | 0.121 / 0.201 | 0.223 / 0.605 / 0.790 | 0.647 / 0.989 | 0.198 / 0.617 / 0.762 |

  - owner 列是 D3D11 那一遍的；Vulkan 那一遍的 owner p50 0.15–0.19 ms。每个密度的第一帧（启动尖峰）：第一个密度 D3D11 CPU 6.2 ms、Vulkan 5.0 ms（着色器与管线、首次上传），之后的密度 0.5–1.3 ms；GPU 与稳态相近。
  - 帧内粒子是各发射画的实例数（含已死、在顶点阶段丢弃的）；微粒小（4–7 设备像素），所以 24 万个只要约 0.2 ms，与 11.4a 中 20–36 设备像素的圆点不可直接比较。owner 不随粒子数增长：粒子在 GPU 上解析，owner 每帧只重建带粒子的节点。
  - 更正（11.5c）：表中 Vulkan CPU 是开着验证层（含同步验证）测的。关验证层重测（同一机器，`particle-page.json`）：Vulkan CPU p50 / p99 按表中密度依次为 0.161 / 0.306、0.138 / 0.235、0.170 / 0.289、0.142 / 0.285、0.172 / 0.282 ms，与 D3D11 相近；GPU 不变。
- 基准重构：`particle_benchmark.hpp`（新）给 11.4a 的计时循环一个帧来源参数，两种基准共用。

**11.4b：粒子的 CSS 与 JS 挂接**（2026-10-13）：
- 注册表（`src/scene/scene_particles.hpp/.cpp`）：`SceneParticles::add`（名字唯一；精灵为覆盖率或 sRGB 颜色，距离场拒绝；值经 `paint::check`）、`resolve`（`--particles-*` 覆盖值；没有覆盖时返回注册的同一对象，覆盖后再检查）；`scene_emitter_area` 把相对区域换成元素 px；`scene_particle_reach` 给出一次发射在任何时刻可能画到的范围。
- 求解器（`src/scene/scene.cpp`）：
  - 每个元素的状态记着发射记录（流或爆发、开始与停止时刻、数量与时长、种子、飞行目标），每帧按 `-weft-particles` 开始、保留或停止流，取出等待的爆发，删掉粒子已全部死亡的记录，输出 `SceneNode::particles`（已过时间按帧时间算）；
  - `display: none` 的元素与其子孙丢弃发射记录与等待的爆发；SVG 内部元素上的粒子为诊断；
  - 静态子树：画着粒子、有发射记录、有等待的爆发或写了 `-weft-particles` 的元素不整棵复用（求解器的参考帧在布局不变时不更新，可能还带着旧粒子）；
  - 飞行：带目标的爆发在它第一帧的布局（或复用的布局）之后定位：按 SceneGeometry 的方式求两个元素的平面（`scene_plane`），目标中心或点经发射元素平面的逆映射到它的 px；
  - API：`set_particles`、`emit_particles`（立即校验名字、数量、时长、点与等待上限）；`reset` 清空等待的爆发。
- 显示列表：发射区域按元素的盒换成 px。损伤（`scene_damage`）：带粒子的元素加入每次发射的可达范围，经元素几何的映射（求解器自己的几何没有映射，按 rect 平移）；祖先是透视平面时整幅。
- 脚本：`ScriptDom::install_particle_host` 与元素方法 `__ceEmitParticles`，scene 平台的 `weft.emitParticles`。画廊会话（`GallerySceneOptions::particles`）安装宿主；有等待的爆发时，下一次 flush 更新 scene。
- 验证：
  - `scene_particles_contract`（新）：注册与拒绝、全部覆盖值与写错的值；流的开始、保持、因覆盖值重启、`none` 停止、按最长寿命消失；速率 0；隐藏的祖先与重新显示；移除与重新加入；静态子树复用，参考帧带着旧粒子时不取回；爆发的开始、覆盖值与结束；飞向目标中心、点、经旋转、隐藏的目标、之后不变；等待上限与每帧上限；同输入同帧；脚本 API 与错误；可达范围的性质测试（160 个随机发射器、约 27 万个粒子位置全部在内）与紧致性；损伤（平移、旋转、透视）；
  - `render_scene_particles_contract`：求解器给出的流与飞向元素的爆发（固定时钟），D3D11 WARP 与 Vulkan 对 CPU 参考光栅相差不超过 3 级，Vulkan 对 D3D11 最大差 2。

**11.4a：粒子绘制项与渲染器**（2026-10-13）：
- paint IR（`src/paint/display_list.hpp`）：`ParticleEmitter`、`ParticleEmission`（流或爆发；已过时间与停止时间用 double）、`Particles`、`Item::Kind::Particles`、`DisplayList::particles`。`check` 拒绝非有限值、颠倒的范围、非正寿命、负的速率/速度/阻力/尺寸/通道、超过 1 的 alpha 与柔和度、越界或距离场的精灵、0 帧或超过 1024 帧、停止晚于已过时间、空爆发、超过 65536 个粒子的发射（流按最长寿命 × 速率 + 1 算）；`append` 重编号载荷与精灵。
- 模型（`src/paint/particles.hpp/.cpp`，新）：`particle_timing`（流：可能存活的个数、最新粒子的序号模 2²⁴、它的年龄与发射间隔，双精度算出；爆发：个数与已过时间）、`particle_random`（lowbias32 整数哈希）、`particle_state`（是否存活、位置、尺寸、角度、颜色、帧）。运动：无阻力时抛物线，有阻力时线性阻力的解析解，飞向目标时沿二次曲线、缓入缓出。
- 规划器：一次发射一条记录，`render::ParticleField` 给 16 个字段命名（映射、时序、数量/时长/种子/形状、区域、寿命与速度、方向/扩散/阻力/柔和度、重力与自转、角度与尺寸因子、四个尺寸、四个颜色、精灵、目标）；`Step::Particles` 一次绘制存活的粒子，没有就不绘制。
- 着色器：`scene.hlsl` 的 `WEFT_PARTICLES` 变体，两个阶段都换掉；sRGB 转换函数移到效果与粒子共用的段落。顶点阶段按模型算出粒子，圆点的四边形多出一个设备像素给抗锯齿；像素阶段画柔和圆点（柔和度与一个像素的抗锯齿取大）或精灵帧（钳在帧内半个纹素，不取邻帧），乘颜色、裁剪链与透明度，编码 pass 转成编码预乘。
- 渲染器：D3D11 与 Vulkan 的 scene 渲染器各有粒子着色器（管线），精灵绑在效果的第一个纹理位置（t3 / 绑定 13），画完换回 scene 的着色器（管线）。预处理器按效果纹理的格式准备精灵。线性 pass、C ABI 与 D2D 参考光栅拒绝粒子。
- scene：`SceneEmitter`、`SceneEmission`（`src/scene/scene_particles.hpp`，新），`SceneNode::particles`；显示列表把它们画在元素的背景与边框之后、内容之前；带粒子的子树不缓存成图层；损伤为整个目标。CSS 与 JS 的挂接在 11.4b。
- 验证：
  - `particle_model`（新）：随机数均匀（10 万个，均值 0.5014，方差 0.0830）、流与爆发的时序（一天长的流仍精确）、存活范围、同一粒子跨帧的身份、运动对数值积分（阻力 0 与 0.8，三个时刻，差小于 0.001 px）、四种形状、曲线、帧、飞行；
  - `paint_contract`：上述拒绝、`append`、记录的 16 个字段与批次；
  - `render_scene_particles_contract`（新）：D3D11 WARP 与 Vulkan（RTX 5090）对 CPU 参考光栅（读同一显示列表，按 2×2 四元组取细导数）：柔和圆点、流与飞行在缩放 1 和 2 下相差不超过 3 级；硬边圆点在旋转、缩放、0.6 透明度与圆角溢出裁剪下除抗锯齿像素外相差不超过 3 级，裁剪外不画；图集四帧、覆盖率精灵与它转 90° 后的两半；粒子之后的盒子照常；Vulkan 对 D3D11 最大差 2；另查图层、损伤与 D2D 的拒绝；
  - `render_d3d11`：带粒子的 C ABI 列表帧返回 `UNSUPPORTED_FEATURE`。
- 4K 性能（`weft_d3d11_probe --particles-benchmark-hardware`；`d3d11_warp_particle_benchmark` 在 WARP 上只查它能跑）：RTX 5090、Ryzen 9 9950X3D、Release，3840×2160（缩放 2），柔和圆点直径 20 设备像素、alpha 0.5、每帧移动，热身 30 帧后采 240 帧，桌面照常使用（未隔离），GPU 56→69 °C、2.6–2.7 GHz；进程峰值工作集 97 MiB。

| 粒子数 | D3D11 GPU p50 / p99 / 最慢（ms） | D3D11 CPU p50 / p99 | Vulkan GPU p50 / p99 / 最慢 | Vulkan CPU p50 / p99 |
| --- | --- | --- | --- | --- |
| 1 000 | 0.008 / 0.010 / 0.013 | 0.007 / 0.047 | 0.010 / 0.012 / 0.379 | 0.142 / 0.241 |
| 10 000 | 0.022 / 0.026 / 0.825 | 0.008 / 0.067 | 0.024 / 0.034 / 0.432 | 0.116 / 0.220 |
| 50 000 | 0.087 / 0.318 / 0.726 | 0.008 / 0.030 | 0.083 / 0.087 / 0.737 | 0.101 / 0.195 |
| 100 000 | 0.176 / 0.827 / 0.909 | 0.011 / 0.053 | 0.169 / 0.736 / 0.762 | 0.159 / 0.207 |
| 200 000 | 0.352 / 0.969 / 1.096 | 0.017 / 0.035 | 0.331 / 0.920 / 0.938 | 0.191 / 0.288 |
| 400 000 | 0.710 / 1.332 / 1.388 | 0.027 / 0.064 | 0.661 / 1.274 / 1.589 | 0.253 / 0.417 |

  - 每个计数的第一帧（启动尖峰）：D3D11 CPU 0.63 ms（首次准备），Vulkan CPU 1.31 ms；GPU 与稳态相同。
  - GPU 时间大致与粒子数成正比（填充率）；p99 的尖峰来自桌面合成等共享负载，未隔离。
  - 更正（11.5c）：表中 Vulkan CPU 是开着验证层（含同步验证）测的。关验证层重测：Vulkan CPU p50 / p99 按表中粒子数依次为 0.018 / 0.070、0.019 / 0.076、0.020 / 0.077、0.027 / 0.088、0.061 / 0.085、0.074 / 0.118 ms。

**11.3e 之二：嵌套提示页面进画廊窗口**（2026-10-13）：
- 窗口的目录：原始目录（`case-list.js` 的 72 例，其清单、观察审计与 Chrome 参照都不变）去掉两例 Three.js（webgl-02/03），最后加一组 “CRPG Tooltip”，即嵌套提示页面，共 71 例、11 组；`preview.js` 与 `gallery_preview_observe` 各自按同一规则生成，窗口自测核对两者的每张卡与标签。
- scene 清单：页面是第 65 个 scene（cluster 之后，尺寸同 Chrome 参照的 1596×800），`catalog_index` 仍指原始目录（页面不在其中，取目录末尾）；窗口按组与序号找每个 scene 的卡片。cluster GPU 探针与交互剖析只跑 cluster scene（`cluster_scene_end`）。
- `probes/gallery_scene_options.hpp`（新）：每个 scene 的运行方式，页面用自己的样式表、包、html 为根、body 的底色与 UI 着色器效果；测试、窗口与 owner 线程共用。
- 窗口自测（`d3d11_warp_gallery_preview`）：
  - 十组走完后，在空闲的第 40–54 帧跳到页面一组：像素 oracle 与截图（`group-10.png`），真实排队的鼠标移动悬停第一个关键词，owner 审计页面 body 上打开的浮层，截图（`tooltip-hover.png`）里第一张卡的烟雾像素不是卡片底色；再移开指针、回到第一组，后续步骤与此前相同；
  - 第 1 帧另查每个 scene 的尺寸等于它的槽位；卡片几何 71×4 = 284 项。
  - 自测为每个可见 scene 另做的 CPU 参考光栅不画效果（报诊断），页面的槽位保留占位图；oracle 本来就用 GPU 读回的图。
- 计数：窗口 71 例，原生绘制 67 例（64 个 scene、2 例 Canvas、页面），诊断卡片 4 例（Lottie）；HUD、标题、报告与 `tools/gallery.bat` 的文字随之更新。surface 退休探针按 scene 数计算提交次数（209）。

**11.3e 之一：嵌套提示页面与 Chrome 逐帧对照**（2026-10-13）：
- 用例 `examples/crpg-gallery/tooltip-cases.json`（新）：
  - 视口 1596×800，361 帧（60 Hz）；
  - 验收动作：悬停关键词、进入浮层、悬停其中的关键词、停留到钉住、按标题拖动钉住的窗口，再悬停一张卡里的装备关键词（它的浮层带自己的烟雾）；
  - 截帧都是 4 的倍数：原版画布烟雾 66 ms 画一次，参考时钟每 4 帧运行一次，截帧时画布正是该帧的时间。
- 参照 `tools/browser_tooltip_page_reference.mjs`（新）：
  - Chrome 155 以真实设备缩放 1 运行 `TOOLTIP_REFERENCE.js`（原版画布烟雾）与 gallery.css，body 为页面的根，底色同画廊；
  - 时钟：`probes/page_reference_clock.js`（新，画廊参考时钟的副本，CSS 动画从元素出现时计时）与输入时钟；烟雾纹理加载完才开始；
  - 每个截帧记录截图、每个元素的 rect、绘制用的计算样式（颜色经画布换成 8 位 sRGB，Chrome 把页面的颜色序列化为 oklch/oklab）与页面状态；
  - 清单记录用到的文件哈希，`verify_gallery_fixture.py` 校验；
  - 两次采集的截图除一帧有 12 个像素差 1 外逐字节相同。
- 会话：`GallerySceneOptions` 的根样式改为挂载前运行的 setup 脚本。回放以 html 为根，body 的内联样式与 Chrome 页面相同；scene 的 UA 样式中 html 与 body 为块。
- 样式编译：
  - 页面的 html 规则（preflight 的行高 1.5 与字体）改名保留（`:host` 在文档中不匹配）；其中 scene 没有、在此也无效果的声明（文字放大、tab 宽度、字体特性与变体的 normal、点击高亮）记入清单后去掉；
  - 页面代码用 `.replace()` 由词拼出的类名（分隔线与关键词色条：border→bg、text-→bg-）也作为候选。gallery.css 只给某处源码写出的类名样式，这里只有 `bg-amber-400`，结果与 Chrome 相同。11.3d 之二时以为拼出的类名在 Chrome 里同样没有样式，第一张卡标题下的分隔线因此缺失，由下面的样式对照发现。
- 字体回退：
  - `shape_with_fallback` 可带每个字体的角色（列表、单色回退、彩色回退）。列表字体按顺序优先；列表之外，默认按文字呈现的 emoji 或带 VS15 的簇先取单色回退字体，默认按 emoji 呈现或带 VS16 的先取彩色回退字体；该类无字体覆盖时取任一覆盖它的回退字体；
  - `match_font_faces` 报告列表给出的字体数，scene 文字据此给出角色（回退字体按是否彩色）；
  - Emoji 与 Emoji_Presentation 属性由 QuickJS-NG 的 libunicode 给出（`src/text/unicode_properties`）；
  - 起因：Chrome 在 Windows 上对 🗡（U+1F5E1）用 Segoe UI Symbol（宽 24 px），此前我们用 Segoe UI Emoji（32.95 px），页面几何偏 8.95 px。
- `scene_tooltip_page` 的回放：按用例逐帧推进与输入，在每个截帧对照 Chrome：
  - 每个元素的 rect，差不超过 1/64 px；
  - 绘制用的计算样式：背景色、颜色、透明度、字重与字形、各边边框的宽度、样式与颜色，预乘颜色差不超过 2；
  - 原生 pass 的整帧像素：通常的均差 ≤ 3、IoU ≥ 0.75；
  - 每块烟雾只显示烟雾的像素：烟雾元素去掉边框与圆角，再去掉树序在它之后、画在上面的东西（有自身文字、没有子元素或是 svg 视口的元素整块去掉，其余元素只去掉画出的边框，都带抗锯齿的余量）与它不在其中的浮动面板（及阴影）；至少 1000 像素，与 Chrome 画布相差超过 1 级的像素不超过 1%。
  - 边框要去掉：盒子的边框按小数位置的覆盖率绘制，Chrome 先对齐到整像素（11.2 的 css-19-63 记录过），小数位置的 1px 分隔线与 Chrome 相差 2 级以上。
- 效果对齐：效果改为画进对齐到设备像素的矩形（各边取最近的像素，旋转、斜切、镜像的盒子不对齐），这是 Chrome 画图像与画布内容的方式。此前装备浮层位于 y = 417.375，Chrome 的画布从 417 画起，我们的烟雾整体低 0.375 px，有符号的差随烟雾的纵向梯度为 +0.3 级，约 2% 的像素相差超过 1 级；对齐后与卡片相同。
- 结果：9 个截帧 rect 全部相同；样式 0 处不同；整帧均差 0.59–0.85、IoU 0.98–0.99；卡片烟雾 1.9–4.2 万像素、均差 0.09–0.18，装备浮层的烟雾 7.2 千像素、均差 0.13；相差超过 1 级的像素至多 0.044%。烟雾提前一个 66 ms tick 时有 34%–57% 的像素相差超过 1 级。
- 彩色文字 oracle 里的 ⚔、☠ 默认按文字呈现，现在取单色字体；加上 VS16 后与之前逐字节相同。
- 新单测：emoji 呈现选字（VS15、VS16、列表优先、无该类字体时的回退、角色数不符的诊断）；字体列表给出的字体数；效果在缩放 1、2 下对齐到设备像素，旋转时不对齐。
- 变异检查 16 个全部被发现：忽略 emoji 呈现、忽略 VS16、忽略 VS15、列表字体不优先、无该类字体时不回退、列表字体数为 0、单色与彩色角色对调、scene 文字不传角色、所有 emoji 都按文字呈现、Unicode 区间奇偶颠倒、不运行 setup 脚本、html 与 body 不是块、页面样式表去掉 html 的行高、编译时不加入拼出的类名、效果不对齐、烟雾提前一个 tick。首轮“提前一个 tick”存活：原来的阈值（均差不超过 1）过松，改为按像素计的份额。

**11.3d 之二：嵌套提示页面在 scene 中运行**（2026-10-13）：
- 页面源码：
  - `examples/crpg-gallery/src/tooltip/NestedTooltipDemo.tsx`：捕获的页面去掉烟雾调参面板（连同它的状态与滑块函数），其余照抄；
  - `examples/crpg-gallery/src/tooltip/SmokeBg.tsx`：原版的导出与参数不变，画布换成带 `-weft-effect(crpg-smoke)` 背景的同类名盒子，画布参数作为效果的自定义属性，时钟起点为挂载时刻。
- `tools/build_tooltip_page.mjs`（新）：
  - 用不变的 harness 打包一个组 `TOOLTIP_TESTS`，唯一的用例渲染该页面；`./NestedTooltip` 解析到捕获的组件（不改），`./SmokeBg` 按包选择；
  - `TOOLTIP_TESTS.js` 用效果版（原生），`TOOLTIP_REFERENCE.js` 用原版画布（Chrome）；
  - 输入输出哈希写入 `native/tooltip-metadata.json`。已有的包与 `metadata.json` 不变。
- 样式：`tools/compile_gallery_cluster_styles.py` 的选择逻辑改为函数，并多输出 `tooltip-utilities.css`、`tooltip-keyframes.css` 与清单：
  - 页面源码的每个词都当候选，与 Tailwind 对捕获源码的候选扫描一致，所以模板字符串与数据里的类名都有（运行时拼出的类名在 Chrome 里同样没有样式）；
  - 带上页面的基础规则（html、body、#root 的尺寸、字体、颜色，惰性的 user-select），略去只设光标且用了 `:not()` 的按钮规则（记入清单）；
  - 逻辑方向的 inset 按从左到右映射为物理边。
  - cluster 的输出逐字节不变，只是清单里编译脚本的哈希更新。
- 会话：`GallerySceneOptions`（样式表及同族关键帧、包、scene 根 `#root` 或 `body`、根的样式、UI 着色器效果）；`body` 为根时页面门户到 body 的浮层进入 scene。
- 层叠：scene 的 `background` 简写在层叠中拆为颜色与图像两个分量（`var()` 代换后判断归属）。此前在层叠之后拆分，页面样式表里 body 的 `background: transparent` 盖过了内联的 `background-color`。
- `verify_gallery_fixture.py` 加查页面包与样式的输入输出哈希。
- `scene_tooltip_page`（新）：页面挂载后三张物品卡的烟雾都是效果；悬停关键词，浮层在 body 上、位于关键词右侧 8px 且上移 4px、宽 16rem；进入浮层再悬停其中的关键词，第二层浮层打开；停留 3 秒后钉住为窗口；按标题拖动后窗口精确跟随；原生 pass 画出浮层与 body 上的画廊底色。
- 变异检查 6 个全部被发现：根固定为 `#root`、不注册效果、不设根样式、背景从不归为图像、背景简写不拆、不映射逻辑 inset。

**11.3d 之一：`Element.closest` 与固定盒子的裁剪检查**（2026-10-13）：
- `script_dom.cpp`：`closest(selectors)` 从元素自身沿祖先元素向上找第一个匹配的，没有时为 null；选择器列表先编译，不支持的选择器即使无匹配也报诊断。CRPG 页面的钉住窗口用它判断 mousedown 是否落在窗口或关键词上。
- 固定盒子的裁剪检查改为只要求逃出的裁剪容纳包含块显示出的部分（根上是视口；包含块自身裁剪时是它的内边距盒）。拖动钉住的窗口不夹在视口内，可以拖出一半；`#root` 与视口同大，两者画出来相同，此前却报诊断。
- 测试：`scene_contract` 加 `closest` 的匹配、空结果与两种诊断；`scene_fixed_browser_contract` 加一个视口外一半的固定盒子。
- 变异检查 4 个中 3 个被发现：不从自身开始、从不匹配、要求容纳全部；“走到文档节点”与原实现等价（文档不匹配任何选择器，DOM 测试已断言它不匹配 `*`）。
- 参考时钟：已有参照所用的 `probes/gallery_reference_clock.js` 把 CSS 动画都从 0 计时，中途出现的动画（如悬停浮层的 fadeIn）与 scene 不同（scene 从元素出现时计时）。该文件的哈希被参照清单锁定，所以不在原地修改；新页面的参照另用从出现时计时的时钟（11.3e）。

**11.3c 之三：`position: fixed`**（2026-10-13）：
- 求解器：
  - 收集布局盒时带上固定盒子的包含块：主运行的顶层（视口），或最近的带 `transform`、`will-change: transform` 的盒子；inline-block 的子运行里没有时为空，遇到固定盒子即诊断。
  - 固定盒子以 `position: absolute` 的样式副本挂到包含块下由 Yoga 排；之后用布局原点坐标换回父元素坐标（二者之间没有变换）。
  - “内容须放下”的 auto 溢出检查改用布局坐标，固定盒子对照其包含块。
  - 布局后检查固定盒子逃出的裁剪是否完整容纳它；某轴 inset 全为 auto 而父元素不是包含块时诊断。
- Yoga 适配器：`will-change: transform` 与 `transform` 一样构成包含块。
- 绘制、命中、几何不变：固定盒子留在树中的位置。
- `tools/browser_scene_fixed_reference.mjs`（新）与 `scene_fixed_browser_contract`（新）：一页 18 个元素的 client rect 与 Chrome 相同；命中与原生 pass 像素；三种诊断、两种应接受的结构。
- 产物与 11.3c 之一相同（只有按时序变化的窗口与计数类产物不同）。
- 变异检查 10 个全部被发现：排在父元素里、不换回父元素坐标、变换不构成包含块、求解器忽略 `will-change`、适配器忽略 `will-change`、接受静态位置、inline-block 的父元素当包含块、不检查裁剪、跨包含块累积裁剪、在父元素坐标里检查是否放下。首轮存活 1 个（适配器的 `will-change`：参照里该盒子本身是绝对定位），补了静态的 `will-change` 盒子。

**11.3c 之二：文字样式与相关 CSS**（2026-10-13）：
- 属性表：`-webkit-user-select`、`font-variant-numeric`、`text-transform`；scene 接受 `overflow-x`、`overflow-y`，`font-style`、`font-variant-numeric`、`text-transform` 为继承属性。
- `text-transform`：
  - `text/case_mapping.{hpp,cpp}`（新）：UTF-8 文字按 Unicode 完整映射转大写或小写，小写的 Σ 按 Final_Sigma；非 UTF-8 字节原样保留。
  - 映射表取自 QuickJS-NG 的 libunicode：同一份未修改的源码另编成 `weft_unicode_case`，不链接引擎。
  - 求解器在继承之后变换元素自身的文字与匿名文字节点，排版、绘制、命中都看到变换后的文字；其他取值报诊断。
- `font-variant-numeric`：
  - `SceneFont::numeric` 为八个取值的位；`Font::shape` 与 `shape_with_fallback` 接受整段的 OpenType 特性，SceneText 按位传入 `lnum`、`onum`、`pnum`、`tnum`、`frac`、`afrc`、`ordn`、`zero`；
  - 文字段缓存键带上这些位；同组两值或未知词报诊断。
- 合成倾斜：
  - 字体目录标为非斜体的字面遇到斜体请求时，字形带 `oblique` 标记；
  - `Font::rasterize`、`rasterize_color`、`outline` 接受 skew（FreeType 轮廓变换、彩色画布的仿射）；D2D 参考光栅对该段字形加绕基线的错切；
  - 斜度为 Skia 的 1/4（`constants::text::synthetic_oblique_skew`）。
- 文字图像留白：`paint::Raster::overhang`。斜体与倾斜文字四周多留 0.35em（`italic_overhang_em`），预处理器与 scene pass 按它放置图像。此前倾出盒子的笔画超过 2px 留白就被裁掉，真斜体字面也一样。
- 视口长度：
  - `expand` 接受视口，把 vw、vh、vmin、vmax 及 s/l/d 形式换成 px；
  - 节点状态记下是否用过视口长度，视口变化时只把这些节点标脏，即使 DOM 没变也重新展开；
  - 关键帧里出现即报诊断。
- `overflow-x`、`overflow-y`：scene 的 `overflow` 简写在层叠前展开到两轴，`expand` 再合成一个值：两轴相同取该值，有 clip 或 scroll 时保留以便诊断，都为 hidden 时为 hidden，其余为 auto。
- `scene_inline_browser_contract`：新增 6 个 Chrome 用例（共 29 个、126 行，最大差 0 px）；测试把 Chrome 记录的 DOM 字符按同样的映射变换后再比较行文字。
- `scene_text_style_contract`（新）：
  - Impact 的正体与合成倾斜在参考光栅、原生 pass、Chrome 截图（`tools/browser_scene_oblique_reference.mjs`）上的斜率；字体自带斜体时不再倾斜；
  - 大小写映射的边界情形；视口变化而 DOM 不变时长度跟随；溢出两轴合成与先后顺序；`-webkit-user-select`；
  - 七种诊断。
- 产物与 11.3c 之一相同（只有按时序变化的窗口与计数类产物不同）。
- 变异检查 20 个全部被发现：
  - 文字变换 5 个：元素文字不变换、匿名文字不变换、每个映射只取一个字符、没有 Final_Sigma、不继承；
  - 数字变体 3 个：不传特性、缓存键不带变体、接受同组两值；
  - 合成倾斜 5 个：从不倾斜、原生不倾斜、参考光栅不倾斜、自带斜体又倾斜、不留白；
  - 视口长度 4 个：视口变化不重新展开、vmin 当 vmax、不认 s/l/d 形式、关键帧接受视口长度；
  - 溢出两轴 2 个：混合时取 hidden、简写不展开；
  - `-webkit-user-select` 被拒 1 个。

**11.3c 之一：点线与虚线边框**（2026-10-13）：
- `scene_border.{hpp,cpp}`（新）：一条点线或虚线边在设备像素上的形状，按 Chromium 的 `BoxBorderPainter::DrawDashedOrDottedBoxSide` 与 `StyledStrokeData`：边框盒按 `ToPixelSnappedRect` 对齐，边宽向下取整；虚线 3:2 或 2:1、间隔取 `SelectBestDashGap`；1–3 设备像素的点为方点，两端按 `EnforceDotsAtEndpoints` 补点；更宽为圆点（零长段加圆头，线两端内缩半个宽度，周期减 0.01 以保住末点）；只容得下一个间隔的线整条画出。结果映回盒子坐标。
- 样式：
  - scene 的 `border` 简写接受全部线型关键字，缺样式时为 none；新增 `border-top/right/bottom/left` 简写；
  - `border-style` 与 `border-width` 一样在层叠前按一到四个值展开到四边（此前没有展开，被静默忽略）；
  - `none`、`hidden` 的边宽算作 0；不支持的线型与带圆角的点线、虚线边框给诊断。
- 显示列表：先画背景，再画点线与虚线的形状（各为一个盒子，圆点是圆角盒子），最后画实线边；实线边的盒子保留点线、虚线边的宽度但颜色透明，所以角上按斜接分割。设备比例由调用方传入（原生预处理器传 pass 的 scale）。
- D2D 参考光栅：同一套形状；圆点用 0.01 设备像素的容差展平（D2D 默认的四分之一像素会让 4 像素的点少一成面积）。
- `scene.hlsl`（编码空间）：没有圆角、没有旋转的盒子：
  - 外缘覆盖率与边框的内缘都改按像素面积算（此前距离场的抗锯齿让 1 像素的盒子只有 75% 覆盖、向四邻渗出约 9%）；
  - 边框各边的楔形判断用夹到盒内的位置，盒外那圈抗锯齿像素因此取所在边的颜色（此前取背景色）。
  - 圆角与旋转的盒子不变。试过让边框按两层（背景、边框各带覆盖率）叠加：更接近 D2D 参考，但 Chrome 对带圆角、边框不透明的盒子会避免背景外渗，画廊里 d3_interactive 的圆角像素因此离 Chrome 更远（差 98 → 372），所以保留单层。旋转盒子的外缘仍取背景色：原生的线性斜坡比 D2D 宽，D2D 的边缘又在精确几何之内，改成边框色时两者差得更多，单独处理。
  - 线性 pass 不变。
- `tools/browser_scene_border_reference.mjs`（新）：每个缩放比例单独启动浏览器（`--force-device-scale-factor`），视口固定为页面大小。DevTools 模拟缩放是把 1 倍的画面放大，点在 CSS 像素上排列，不是高 DPI 屏上的画法。`tools/browser_border_patterns.mjs`（新）打印逐行图案表。
- `scene_border_patterns`（新）：119 条边（点线 1–4、虚线 1–3 像素，17 种长度）与 Chrome 155 的逐像素图案相同。
- `scene_border_browser_contract`（新）：
  - 17 个用例在缩放 1、2 下与 Chrome 截图对照，参考光栅与原生 pass 各 34 次；Chrome 画成纯色的像素必须同色，圆点另比较墨量与每个点的质心；
  - 写法：Tailwind 的 `border-b border-dotted`、四值 `border-style`、缺样式的 `border`；三种诊断；
  - 离格带边框盒子的每个像素与按面积的精确值相差不超过 1 级。
- 产物：原生画面中变化的像素全部更接近参考光栅（D3D11 170 个、Vulkan 154 个，在 css-15、css-19 等）；画廊 cluster 变化的 220 个像素中 216 个更接近 Chrome（framer-04 等）；gallery-preview 的窗口截图本来就随时序变化。
- 变异检查 19 个全部被发现：
  - 着色器 3 个：外缘按距离场、内缘按距离场、盒外像素不归边；
  - 显示列表 3 个：实线边不对点线边斜接、点画在实线边之上、点按缩放 1 排列；
  - 参考光栅 2 个：圆点粗展平、点不按目标像素排列；
  - 样式与求解器 5 个：`border-style` 不展开、缺样式当 solid、边简写写到四边、none 保留宽度、接受带圆角的点线；
  - 图案 6 个：不补端点、奇数宽度不居中、盒子截断而非四舍五入、虚线间隔总取较少的段数、圆点不内缩、末点不减周期；
  - 首轮存活 2 个（内缘按距离场、末点周期），补了离格盒子的精确覆盖用例与长圆点线的逐点质心。

**11.3b：scene 内联排版**（2026-10-13）：
- `scene_inline.{hpp,cpp}`（新）：内联格式化上下文。内容收集一次（空白折叠、整形、UAX 14 断行机会），之后可按任意宽度排行：
  - 贪心填行，宽度用 LayoutUnit，行尾空格不计；开始边随后、结束边随前；
  - 行盒由根 strut、行内框 strut、文字行盒与原子内联外边距盒在同一基线上合成；`text-align` 偏移按 LayoutUnit 截断，溢出时起始对齐；
  - 输出文字片段、行内框片段（首/末标记）与原子内联矩形。
- 求解器：
  - 文字自身的元素、`#text`、以及子项全为行内级的块容器（借一个匿名块盒）由内联排版度量；行内后代不再是 Yoga 盒子，原子内联另起一次布局（适配器新增收缩适配宽度 `fit_width`，百分比按包含块解析）；
  - `SceneNode` 新增 `lines`（文字行：文本、行盒、基线）与 `fragments`（行内框片段），随盒子一起复用与跟随参照帧；
  - 行内上下文里的空白文字节点保留为 `#text`，匿名文字只继承可继承属性；`text-align`、`white-space` 改为继承；UA 表补块元素与 button；
  - `SceneSolver` 改为直接持有 `SceneText`。
- 绘制：显示列表与 D2D 参考光栅逐行画文字（单行且在盒内时图像仍是整个节点盒，多行时每行图像对齐节点坐标的整数像素，字形保持子像素位置），按片画行内框；原生预处理器的文字图像缓存键加行号。
- 几何：命中与 `getClientRects` 按片；transform 不作用于非原子行内元素。
- Yoga 适配器：列方向 flex 项里会换行的文字，宽度要由布局决定时，若 flex-basis 与 height 都是 auto，basis 就是该宽度下的内容高度，也就是自动最小高度，于是令它不收缩（`flex-shrink: 0`），只排一遍（原先报诊断）；其他 basis 仍报诊断。架构约定不允许为某个功能再排第二遍。
- 画廊样式编译器带上 Tailwind preflight 的替换元素规则；`vertical-align` 加入属性表。
- `scene_inline_browser_contract`（新）：23 个用例与 Chrome 155 的元素片段、每行文字、行首行尾位置与内容区顶部逐一相同（49 个矩形、102 行，最大差 0 px）。工具 `tools/browser_scene_inline_reference.mjs` 生成参照。
- `scene_inline_contract`（新）：同内容复用布局（含全新帧从参照帧取回行）、改文字后重排；按片命中（包围盒里片外的点命中父元素）与 client rects；分片边框的首尾；逐行绘制；D3D11 原生 pass 与参考光栅逐行墨迹质心差小于 1 px；四种诊断。
- 画廊产物变化：anime-13 与 Chrome 的平均差 0.56 → 0.19，K 线 0.236 → 0.234；css-07 居中改按 LayoutUnit 截断（亚像素）；css-19 打字机文字不再被图像边界截断；其余为亚像素差。
- 变异检查 21 个全部被发现：
  - 排版 10 个：不要根 strut、不要行内框 strut、片段宽度截断而非向上取整、居中当右对齐、右对齐当左对齐、行尾空格参与填行、结束边占换行机会、nowrap 看两侧而非公共祖先、不折叠空白、`<br>` 后保留行首空格；
  - 原子内联与字体 2 个：原子内联总按下边缘对齐、行盒只看主字体；
  - 求解器 5 个：片段首尾互换、行内上下文丢掉空白文字节点、`text-align` 不在继承表（锚点：列方向 flex 容器里的匿名 flex 项）、复用布局时不带行、参照帧不跟随行；
  - 绘制与几何 3 个：不分片画边框、按包围盒命中、只给一个 client rect；
  - 适配器 1 个：宽度不确定的列里忽略自动最小高度；
  - 首轮存活的 3 个（行内框 strut、复用时的行、继承表）补了用例：外层行高更大而只含小字号子框、颜色动画中的段落、匿名 flex 项居中。

**11.3a：烟雾效果**（2026-10-13）：
- `examples/crpg-gallery/effects/smoke.hlsl`：原版 `SmokeBg` 的 tick 写成效果：
  - 48×64（可设）画布的每个像素取一个云纹理素和一个噪声纹理素（最近邻、按原版取正模），经 gamma 表、噪声与渐隐；字节截断与原版一致；
  - 画布双线性拉伸到盒子上，与页面缩放画布的方式相同；
  - 参数：缩放与 gamma、四个速度、渐隐方式与 90° 旋转、画布尺寸、稀有度颜色、时钟起点（原版在挂载时开始计时）。
- `weft_gallery_effects`（新）：编出 `crpg_smoke` 效果，初始值取原版的默认参数与 common 颜色。
- `render_crpg_smoke_contract`（新）：与 `tests/support/crpg_smoke.hpp`（CPU 上照搬原版 tick：double 运算、Float32 渐隐表、`|0` 截断、JS 的 `%`）逐像素对照，D3D11 WARP 与 Vulkan（RTX 5090）：
  - 8 种情形：开始时、25 个 tick 后、更晚、时钟在 2 秒后才开始、宽扁的 chip（右侧渐隐、旋转 90°、96×24）、无噪声、拉伸 3 倍、按非整数比拉伸的 chip；
  - 每种情形与原版字节的最大差都是 1，没有像素超过 2 级；颜色等于稀有度颜色乘 alpha。
- 变异检查 8 个全部被发现：忽略时钟起点、取最近的画布像素、不加像素中心偏移、从不旋转、总是顶部渐隐、噪声按云的尺度移动、没有 gamma 表、噪声不取模。
- `ctest`：Release 125/125，Debug 125/125；产物与 11.2 相同。

**11.1：效果基础**：
- paint IR：
  - `EffectProgram`：名字、读取的参数个数（最多 16 个 float4）与纹理张数（最多 4 张），以及四份字节码：D3D11 的 DXBC 与 Vulkan 的 SPIR-V，各有编码与线性两种合成空间；
  - `Effect`：程序、时间（秒）、参数、纹理（列表资产的下标）与圆角；新增 `Item::Kind::Effect`，列表的 `programs` 持有程序；
  - `check` 拒绝越界的载荷与程序、多于程序读取的参数、与程序不符的纹理张数、越界或带符号距离的纹理、非有限值与负圆角；`append` 重编号程序与纹理。
- 规划器：
  - 效果记录沿用图片记录的几何与裁剪，时间放在 `extra.x`，圆角与盒子相同；
  - 16 个参数放进效果记录不用的字段（`render::effect_parameter`）；
  - 同一程序、同一组纹理的相邻效果合为一次绘制；
  - 线性 pass 中，没有圆角和旋转的效果与盒子一样，只覆盖像素中心在盒内的像素。
- 着色器：`scene.hlsl` 的 `WEFT_EFFECT` 变体是效果的像素阶段。
  - 从记录取出 `EffectInput`，调用效果函数，再乘圆角、裁剪链覆盖率与透明度；
  - 编码空间把线性预乘结果转成编码预乘；
  - 纹理在 t3–t6，`effectRepeat`（线性过滤、重复寻址）在 s1；
  - 原有着色器的字节码不变。
- 构建：`cmake/WeftEffects.cmake` 的 `weft_add_effect(target name source PARAMETERS n TEXTURES n)`。
  - 用 FXC（ps_5_0）与 DXC（SPIR-V）各编两种空间，/O3，警告即错误；生成 `weft::effects::<name>()`；
  - 编译前由 `cmake/check_effect.cmake` 检查源文件：拒绝资源声明、寄存器绑定、不带 `static` 的全局量（HLSL 会把它放进常量缓冲）、include 与 namespace。架构原设计用反射检查，改动原因见 [架构](architecture.md)。
- 渲染器：
  - D3D11：按程序懒建像素着色器并缓存；效果批次绑定效果的着色器、纹理与重复采样器，画完恢复；
  - Vulkan：描述符集多出 4 个纹理绑定与 1 个采样器绑定；效果管线按（程序、目标格式）懒建并缓存；效果批次绑定它，画完换回当前 pass 的管线；
  - 两者：效果采样的资产总是单独的纹理，不进图集；它的缓存键带单独的标志位，所以同一资产作为图片时的图集区域不会被效果取到；
  - `d3d11::render_list` 在渲染器缓存资产时按资产身份生成键，与 C ABI 的列表帧相同。
- C ABI 暂不接受效果项（`UNSUPPORTED_FEATURE`），等它有自己的特性位；scene 渲染器暂不画效果（11.2）。
- CRPG UI 的两张烟雾纹理（`cloud.png`、`tiling-noise.png`，各 2048²）已只读捕获到 `tests/fixtures/crpg-gallery-assets`，清单带 sha256；`tools/verify_gallery_fixture.py` 一并校验。供 11.3 使用。

验证：
- 测试效果（`tests/effects`）：uv、参数（时间选出要显示的参数）、纹理（数据与颜色）、重复（`effectRepeat`）、颜色（与盒子等价）。
- D3D11（WARP 与 RTX 5090）与 Vulkan（RTX 5090 与 AMD 核显）逐像素对 CPU 期望，不超过 1 级：
  - uv 效果放在分数坐标的盒子上，只覆盖像素中心在盒内的像素；
  - 16 个实例一次绘制，各显示自己的参数，覆盖全部 16 个记录字段与时间；
  - 数据纹理与颜色纹理逐纹素；重复采样在第二块与末纹素处回绕，相邻两个纹素各占一半；
  - 效果之后的盒子照常绘制；
  - 缓存渲染器先把数据纹理作为图片放进图集，之后的效果仍取单独的纹理，图集放置次数不变。
- 效果与等价的盒子放在组、圆角裁剪、旋转与圆角下：D3D11 最大差 0（986 像素），Vulkan 最大差 0，Vulkan 对 D3D11 WARP 最大差 1。调试层与验证层零告警。
- C ABI：带效果的列表帧返回 `UNSUPPORTED_FEATURE`，不产生句柄。
- paint 合同：拒绝路径、`append` 重编号、全部 16 个参数的记录字段、时间与圆角、按程序和纹理合批。
- 效果检查：7 个样例各自检查结果与拒绝原因（接受注释、`static` 全局量、类型与辅助函数；拒绝寄存器绑定、常量缓冲、纹理、采样器、不带 `static` 的全局量、include）。
- 原有着色器的字节码逐字节不变。
- `ctest`：Release 121/121，Debug 121/121（328.2 s），新增 7 个效果检查测试；Android 交叉编译零警告。
- 产物：确定性产物与 10.6d 基线逐字节相同；不同的只有 6 组实时窗口套件（见第 5 步记录），以及 10.6e 起改版的 Vulkan 窗口报告格式。
- 变异检查 33 个全部被发现：
  - paint 合同 4 个：接受带符号距离的纹理、接受少于程序的纹理、`append` 不重编号程序、不重编号纹理；
  - 规划器 5 个：参数 14 与 15 换字段、丢掉时间、丢掉圆角、合批不看纹理、合批不看程序；
  - 着色器 6 个：uv 用转置的尺寸、时间取自透明度、参数 14 取自背景字段、分数坐标的边缘改为抗锯齿、忽略透明度、忽略圆角裁剪；
  - D3D11 5 个：纹理寄存器错一位、不绑重复采样器、之后的绘制沿用效果着色器、效果纹理进图集、缓存键不带标志位；
  - Vulkan 7 个：纹理绑定错一位、用钳制的采样器、之后的绘制沿用效果管线、组内用目标格式的管线、效果纹理进图集、缓存键不带标志位、不写效果的描述符；
  - C ABI 1 个：接受效果项（校验把它当图片取 `images`，越界崩溃）；
  - 效果检查 5 个：保留行注释、接受寄存器绑定、接受资源类型、接受不带 `static` 的全局量、接受 include。

限制：
- 效果编进宿主程序（`weft_add_effect`），还没有效果包与清单；参数只有个数，名字、类型与默认值随 11.2 的 CSS 挂接加入。
- 编码空间的变体已经编译，但还没有绘制路径使用它（scene 渲染器在 11.2）。
- 效果的着色器与管线缓存持有程序，直到渲染器销毁；卸载效果尚未实现。
- Android 设备上未运行。

**11.1 之后：图表悬停掉帧（2026-10-13）**：用户手测发现鼠标在 sunburst 与 K 线上快速扫动时掉到 60–80 FPS。原因、修复与测量见 [交互剖析](gallery-interaction-profile.md#悬停扫动与-scene-图像缓存2026-10-13)，要点：
- scene 预处理器的图像缓存整个清空、小于单帧工作集，复用为 0；改为按最近使用淘汰并保留当前帧的图像（`src/scene/scene_image_cache.hpp`），上限按实测工作集定为 1024 条、32 MiB；
- D3D11 scene pass 的视图常量一批写入、按偏移绑定；
- 画廊窗口的 D3D11 调试层可以关闭（`--no-validation`），`tools/gallery.bat` 的交互入口默认关闭；
- 新的 live 基准 `sunburst-sweep`、`kline-sweep` 复现扫动，`-sampled` 后缀采样渲染线程；
- 变异检查 8 个全部被发现：缓存淘汰当前帧的图像、查到的图像不更新最近使用、不更新所属帧、删除不扣字节、预处理器不开始新帧；每次绘制都绑第一组常量、一批都用第一次绘制的常量、切换目标前不解绑图像。

**11.2：CSS 挂接与 scene 渲染器**（11.2a 与 11.2b 一起完成）：
- 清单：`weft_add_effect` 的参数可以具名，写作 `名字:类型=初始值`（类型 number、vec2、vec3、vec4、color；初始值是 CSS 文本，逗号代空格），纹理可以具名，`ANIMATED` 表示函数读时间。生成的程序带 `named_parameters`、`named_textures`、`animated`；`paint::check` 要求清单与个数一致。
- 注册（`src/scene/scene_effects.hpp`）：宿主用 `SceneEffects::add(名字, 程序, 纹理)` 注册。
  - 拒绝：重复的名字、没有清单的程序、纹理个数不符、带符号距离的纹理、类型不符的初始值；
  - `resolve` 按清单从元素计算后的自定义属性（`--名字`，已继承、已解析 `var()`）取每个参数，没有或无效时用初始值；未注册的名字、类型不符的值是 scene 诊断；
  - 值的类型：number 一个数，vecN 恰好 N 个数，color 是 CSS 颜色，转成直通的线性 RGB 与 alpha。
- scene 求解器：`set_effects` 接入注册表。
  - `background-image: -weft-effect(名字)`，`background` 简写同样识别；括号内必须是一个名字；
  - 结果放在 `SceneNode::effect`，参与节点相等、绘制来源相等与损伤；
  - 读时间的效果让帧持续推进（`animated`）并每帧重绘，不作为静态子树整体复用；参数变化只重绘、不重排；`scene_damage` 每帧把它的盒子算作损伤。
- 显示列表：先画背景色的盒子，再画效果，再画只有边框的盒子，与 CSS 背景图片的层次相同；效果铺满边框盒、按圆角裁剪，时间取帧时间；同一列表里的程序与纹理按租约共享。被列为显式图层、但子树里有效果的节点在 pass 里画，不缓存成图层图像。
- 预处理器：`ScenePrepared::effects` 给出每个效果的程序与纹理图像。
  - 效果纹理按资产缓存（与其他图像同样按最近使用淘汰）；
  - 格式：覆盖率按数据存一字节，不透明 sRGB 原样存、采样时先解码，半透明的转成线性预乘浮点；
  - `SceneImage` 新增格式字段，像素字段 `bgra` 改名 `pixels`。
- D3D11 scene 渲染器：按图像格式建纹理；效果的编码空间着色器按程序缓存；效果批次绑定着色器、t3–t6 的纹理与 s1 的重复采样器，画完换回 scene 着色器。
- Vulkan scene 渲染器：按格式建图像；效果管线按程序缓存；描述符写入绑定 13–16 与 21；画完换回 scene 管线。设备不能线性过滤 32 位浮点图像时，含半透明颜色纹理的帧回退，原因计数为 `effect float texture filtering`。

验证：
- `scene_effect_contract`（新）：生成的清单；各类型的值与拒绝；注册的拒绝；初始值、设置值、继承与无效值；`-weft-effect()` 的三种错误与未接入注册表的诊断；显示列表的层次、时间、圆角与盒子大小；两个元素共享程序与纹理；显式图层中的效果；读时间与不读时间的效果在保留帧里的重绘、重排与损伤。
- `scene_native_contract`：效果纹理的三种格式与按资产复用。
- `render_scene_effects_contract`（新）：D3D11 WARP 与 Vulkan（RTX 5090）的编码空间 scene pass。
  - 效果与等价的盒子放在组、圆角溢出裁剪、旋转、圆角与透明度下：最大差都是 1（约 980 像素）；
  - 数据纹理、不透明 sRGB 纹理、半透明线性纹理（重复采样）与帧时间：盒子内部与 CPU 期望相差不超过 2 级（边缘一圈由抗锯齿决定，已由与盒子的对比覆盖）；
  - 效果之后同一 pass 里的盒子照常绘制；
  - Vulkan 对 D3D11 WARP 最大差 1；调试层与验证层零告警。
- `ctest`：Release 124/124，Debug 124/124（319.9 s），新增 `scene_effect_contract`、`render_scene_effects_contract`；Android 交叉编译零警告；产物与掉帧修复后相同，没有新的差异。
- 测试中发现并修复：求解器的参照帧（只在重新排版时更新）同步节点时没有复制 `effect`，带读时间效果的子树会被整体复用，帧停下，参数也可能停在旧值。参照帧跟上之后的那一帧暴露了它；Debug 构建的一致性自检对此会报 `WEFT_SCENE_CORRUPTION`。
- 变异检查 28 个全部被发现：
  - 清单 2 个：初始值保留逗号、丢掉 `ANIMATED`；
  - 注册与取值 4 个：忽略设置的值、颜色不转线性、接受少于清单的纹理、向量接受更少的数；
  - 求解器 6 个：简写不识别效果、读时间的效果让帧停下、读时间的效果整棵复用、效果变化不重绘、名字接受列表、参照帧保留旧效果；
  - 显示列表 4 个：边框画在效果下面、时间为零、带效果的图层被缓存、程序与纹理不共享；
  - 损伤 1 个：读时间的效果不算损伤；
  - 预处理器 3 个：效果纹理每次都重建、半透明当作不透明、数据与 sRGB 互换；
  - D3D11 3 个：纹理寄存器错一位、之后的绘制沿用效果着色器、效果绘制没有常量；
  - Vulkan 5 个：之后的绘制沿用效果管线、纹理绑定错一位、用钳制的采样器、线性纹理当半精度、拒绝能过滤的浮点纹理。

限制：
- 文档（flow）页面不支持任何背景图片，因此也没有效果；D2D 参考光栅不画效果，回退到它的 scene 会失败。
- C ABI 仍拒绝效果项；画廊会话还没有注册效果（11.3 起）。
- 参数只来自自定义属性，自定义属性不参与 CSS 过渡与动画的插值。
- “可见时才推进”依赖宿主只推进可见的 scene；零面积或被完全裁掉的效果仍让帧推进。
- Android 设备上未运行。

## 第 12 步计划（2026-10-14）：交互性能优化

用户决定（2026-10-14，第 11 步完成后）：按约定的顺序做待办里的三项交互优化。

| 段 | 内容 | 验证 |
| --- | --- | --- |
| 12.1 | K 线：SVG 元素的派生值按元素缓存，文字图像按内容缓存，路径覆盖率按内容摘要建键；宿主在渲染线程之外释放旧的场景帧 | 带缓存与不带缓存的显示列表随机差分；像素不变；`kline-sweep` 前后 |
| 12.2 | 散点图：`<circle>`、`<rect>` 等基本形状画成带圆角的盒子，不经光栅化 | 像素与 Chrome 参照；`scatter-drag` 前后 |
| 12.3 | 小图像进图集合批绘制；路径覆盖率每像素 1 字节；Vulkan scene pass 每次绘制一个描述符集的开销 | 像素不变；`kline-sweep` 与 `scatter-drag` 前后；D3D11 与 Vulkan |

测量方法（各段相同）：`weft_d3d11_probe --gallery-live-benchmark-hardware <dir> <场景> --no-vsync --no-validation`，windows-profile 构建（Release 加 PDB），4K 窗口，RTX 5090 + Ryzen 9 9950X3D，桌面照常使用（未隔离），每个场景连跑 3 次取中位数。被测卡片的重绘时间取渲染 trace 的 `paint <卡片>` 区间；采样分析（场景名加 `-sampled`）会干扰计时，只用来看热点的比例。

## 第 13 步计划（2026-10-14）：与引擎通信

用户决定（2026-10-14，第 12 步之后）：第 12 步剩下的图表热点转为低优先级（图表特有的问题由业务侧负责）；下一步做与引擎的通信：血条这类每帧变位置的元素不能靠每帧推 JSON，另外要能调用引擎函数。完整的技术路线（接口草案、线程、迁移、验收）见 [与引擎通信](engine-communication.md)。

现状：
- 原 CRPG UI（UE 工程 CRPGFramework，Ultralight 1.4）的三条通道都按字符串名字：
  - JS → 引擎：`window.ue.sendEvent(name, obj)`，只发不收，引擎侧转成 JSON 文本再分发；
  - 引擎 → JS：把 JSON 文本拼进脚本求值，`window.__ue_dispatch('fn', <json>)`；
  - 视图模型：`window.__vm_update(delta)`（只有测试页在用）；
  - 没有请求-应答，用成对的事件代替（如 `request_settings` → `loadSettings`）；约 68 种消息，约 40 种是正式功能。
- 血条、伤害数字、状态标记：引擎每 16 ms 在 C++ 里投影，把全部实例（id、位置、缩放、是否在屏幕内、是否在相机后、远近顺序、视图模型字段）整份作为 JSON 推送（`__floating_ui_batch`），React 每帧重渲染全部实例；屏幕外的处理（隐藏、贴边、箭头角度）在 JS 里算。
- Weft 上的同一做法（`weft_d3d11_probe --hud-benchmark-hardware <dir>`，HUD_TESTS：名字加血条，每帧改 transform，约二十分之一的单位每帧掉血；RTX 5090 + Ryzen 9 9950X3D，固定 CCD0，Release）。“零传输”是页面自己算出同样的值：把数据交给 JS 的任何方式（包括共享内存）都不会比它更省：

| 单位 | 方式 | 引擎拼 JSON | 脚本与改 DOM | 样式、布局与场景更新 | 渲染 CPU | 每帧合计 |
| --- | --- | --- | --- | --- | --- | --- |
| 100 | 推 JSON | 0.05 ms | 0.9 ms | 4.6 ms | 1.0 ms | 6.6 ms |
| 100 | 零传输 | 0 | 1.1 ms | 4.5 ms | 1.0 ms | 6.6 ms |
| 200 | 推 JSON | 0.09 ms | 1.8 ms | 9.2 ms | 2.0 ms | 13.0 ms |
| 200 | 零传输 | 0 | 2.2 ms | 9.0 ms | 1.9 ms | 13.0 ms |
| 400 | 推 JSON | 0.17 ms | 3.6 ms | 20.6 ms | 4.0 ms | 28.3 ms |
| 400 | 零传输 | 0 | 4.4 ms | 20.0 ms | 3.8 ms | 28.2 ms |

  - 传输不是瓶颈：零传输与推 JSON 一样慢。贵的是之后每个单位每帧改一次 transform 带来的样式、布局与场景更新（每个约 50 µs）。200 个单位已超出 60 Hz 的预算。
  - 所以数据要交给原生侧直接用（锚点由渲染线程读），而不是交给 JS 再写 DOM；共享内存只在 JS 不必逐项改 DOM 时才有用（如画布小地图、图表的大批数值）。
  - 每帧还有一笔按整个文档算的固定开销：400 个单位（约 1,600 个节点）只改 1 个时，样式、布局与场景约 5 ms，渲染约 4 ms；之后每改一个单位再加约 45 µs。这对所有页面都是问题（13.5 增量管线）。
  - 完整的技术路线、接口草案与验收见 [与引擎通信](engine-communication.md)。
  - M0 文档至多 2,048 个节点（`max_document_nodes`），每个血条 4 个节点时至多约 500 个。

设计：
1. 锚点（每帧变位置的元素）：
   - 引擎每帧调用一次，交一个紧凑数组：锚点 id、x、y、缩放、深度和标志（可见、屏幕外、相机后），任意线程，新值覆盖旧值。
   - 页面在元素上写 `data-anchor="<id>"`（React 的 JSX 直接可写），可选元素上对准锚点的位置（如底边中点）。
   - 渲染线程画每一帧时取最新的锚点值，平移、缩放或隐藏这些元素：不跑 JS、不求样式、不布局，也不比 3D 画面晚一帧。owner 的命中测试用同一张表；同一容器里的锚点元素按深度排序（近的在上）。
   - 页面内容没变时，渲染线程保留上一帧的显示列表，只重算锚点元素。
   - 位置由引擎投影（和现在一样）：Weft 不需要知道相机和世界坐标。
   - 这是给渲染线程的共享内存：引擎写一份定长记录的数组（三缓冲，不加锁，新值覆盖旧值），渲染线程直接读，每个锚点约 32 字节。
2. 桥接（事件、调用、视图模型）：
   - 值带类型（null、布尔、整数、浮点、字符串、字节、数组、对象），在原生侧直接转成 JS 值，不拼脚本文本（现在未转义字符串的问题随之消失）；也接受 JSON 文本，由原生侧解析。
   - 引擎 → JS 的事件：`weft.engine.on(name, fn)`。JS → 引擎：`weft.engine.send(name, value)` 只发；`weft.engine.call(name, value)` 返回 Promise，引擎可以稍后完成。
   - 视图模型：按字段标脏，每次推进合成一批交给 JS（之后可以不经 JS 直接绑定到 DOM）。
   - 线程：任意线程都可以发事件、完成调用（入队，下一个安全点交付）；处理函数在 owner 线程同步执行，不能重入 View。
   - 兼容：提供 `window.ue.sendEvent` 与 `window.__ue_dispatch` 的兼容层，现有 CRPG UI 不改代码就能接上。
3. C ABI 与宿主：以上能力进 C ABI（句柄、定宽类型、版本化结构体，不暴露 STL、异常或无所有权说明的裸指针），探针里写一个模拟引擎的宿主来驱动；真实的 UE 插件留到之后（M5）。

| 段 | 内容 | 验证 |
| --- | --- | --- |
| 13.1 | 锚点：先做 CPU 参考路径（显示列表应用锚点），再做 GPU 路径（记录留在显存，每帧只上传锚点表与顺序缓冲，着色器变换）；命中、深度顺序、缺失与屏幕外规则；HUD 基准增加锚点模式 | 与 JS 设 transform 的同一画面、GPU 与 CPU 两条路径逐像素比较；命中；画的是最新值；400 个单位只移动时 owner ≤ 0.1 ms、渲染 CPU ≤ 0.1 ms |
| 13.2 | 桥接：带类型的值、双向事件、调用与 Promise、视图模型批次、跨线程队列、错误与生命周期；CRPG 兼容层 | 合同测试（类型往返、重入、View 销毁后才完成的调用、多线程投递）；与 JSON 的对比基准 |
| 13.3 | C ABI 与模拟引擎的宿主样例；对外的 View 能加载页面、推进、取帧 | C ABI 测试；样例跑 HUD 与事件 |
| 13.4 | 用 CRPG 的浮动 UI（血条、伤害数字）验证：同样的组件改用锚点与事件 | 与原 JSON 路径对比耗时与画面 |
| 13.5 | 增量管线：去掉按整个文档算的固定开销；UI 线程只处理变化的节点，渲染线程按子树保留显示列表与记录 | 1,600 个节点每帧改 1 个元素：owner ≤ 0.2 ms、渲染 CPU ≤ 0.2 ms；随机变更下增量与全量逐字节相同 |

用户确认（2026-10-14）：锚点用 `data-anchor` 属性声明；API 用自己的 `weft.engine.*`，另加 CRPG 兼容层；位置继续由引擎投影。锚点元素的裁剪与 `position: fixed` 相同（不受祖先 overflow 裁剪），GPU 路径因此可以只用一个变换移动整个锚点子树。

## 第 12 步记录（2026-10-14）

**12.3：图集合批、1 字节覆盖率与 Vulkan 描述符集**（2026-10-14）：
- 原因（12.2 之后）：K 线一帧约 58 次绘制、散点图约 30 次：每张小图像（SVG 刻度文字的字形轮廓、tooltip 的文字）各建一张纹理、各画一次；路径覆盖率每像素存 4 字节；Vulkan 每次绘制分配并写一个描述符集。
- 新的测量入口：`weft_d3d11_probe --scene-interaction-benchmark-hardware|-warp|-sampled <dir>`（`probes/scene_interaction_benchmark.cpp`）。
  - 内容：画廊同一张 K 线卡与散点图卡、画廊实时基准的指针轨迹，每帧推进 1/60 s，同一串帧在 D3D11 与 Vulkan 上各画一遍（4K 目标、2 倍缩放），记渲染 CPU、GPU 时间，以及每帧的绘制、文字与路径重建数。画廊窗口只走 D3D11，Vulkan 只能这样比；`-sampled` 只采样计时的 pass。
  - 指针轨迹每秒重复。每帧沿轨迹走 7.31 ms（画廊里 K 线约每秒更新 137 次），运行内不会回到已画过的位置；按 1/60 s 走时 60 帧后全是缓存命中，测不到重建。
  - 桌面未隔离时，同一版本几次运行相差可达 40%。比较时把进程固定在一个 CCD（逻辑处理器 0–15）、高优先级，几个版本交替运行，每轮从下一个版本开始（固定先后时，后跑的版本约快 5%），取中位数。
- 1 字节覆盖率（`scene_native.cpp`）：
  - 路径覆盖率直接用光栅器的结果；只含可打印 ASCII 的单色文字本来就画成白字再着色，取其 alpha 存为 1 字节；彩色文字与阴影仍是 BGRA。
  - 记录仍走着色遮罩分支，读取的通道从 a 改为 r（`scene.hlsl`）：1 字节图像的值在 r 里。起初改走着色器已有的覆盖率模式，算式相同，但编译器结合乘法的顺序不同，透明度不为 1 时差 1 级；只换通道则指令不变。
  - RTX 5090 上画廊集群的 210 张截图与改动前逐字节相同。WARP 上逐字母旋转的文字（anime-06）每张至多 600 个像素差 1 级：WARP 对单通道与四通道格式的双线性过滤精度不同。
- 图集（`render/atlas_packer`、`scene_native`、两个后端）：
  - 预处理器按格式分两种页：1 字节覆盖率页 2048×2048、BGRA 页 1024×1024（每页 4 MiB），各至多 8 页。宽不超过半页、高不超过 128 纹素（含 1 纹素边框，边框复制边缘纹素）的文字、阴影与路径图像放进页里，其余用独立纹理；页满时也退回独立纹理（计入 `atlas_refusals`）。
  - 区域不搬移，只在图像不再被任何缓存、pass 或后端租约持有之后才回到页里（每次 prepare 开始时回收）。
  - `AtlasPacker` 改为页内可重用：释放的区间回到所在行，与相邻空区合并；变空的行与相邻空行合并，较矮的图像可以切开它；页尾的空行交还；比图像高出一半以上的已用行留给新开的行。文档渲染器的字形图集同样受益。
  - 合批：新的 `ScenePass::Step::Atlas`：条目顺序上相邻的盒子与同一页上的图像（以及没有图像、什么也不画的空路径）合成一次实例绘制；组、效果、粒子、背景模糊、平面、图层、独立纹理的图像和换页都打断它，没有批次的条目（画面外的背景模糊、没有存活粒子的发射）也打断。
  - D3D11：页纹理用 DEFAULT 用法，新图像按区域写入（`UpdateSubresource`），即时上下文保证写在之前采样它的绘制之后。
  - Vulkan：页创建时用一次单独的同步提交转成着色器可读布局；之后上传图像的录制在复制前后做“可读 → 传输目标 → 可读”的转换，所以从未提交的录制不会改变页的布局。
- Vulkan 描述符集：同一 pass 内按“采样的图像与效果纹理”去重，采样相同的绘制共用一个集，与上一次绘制同集时不再绑定。图集之后每帧只剩约 9 次（K 线）和 3 次（散点图）绘制，这一项在 RTX 5090 上测不出差别。
- 像素：图集里的纹理坐标是“页内偏移 + 局部坐标 × 尺寸”，最低几位的舍入与独立纹理不同，硬件双线性权重在部分边缘像素上差一个量化台阶，做不到与独立纹理逐字节相同（计划写的是“像素不变”）。
  - 新测试里开、关图集的差异：D3D11 WARP 至多 2 级、每帧 230–380 个像素。
  - 产物与 12.2 基线：含文字、路径或阴影的截图在边缘像素上变化：画廊集群 98 张至多 2 级（每张至多约 1,400 个像素）；scene-native 20 张、Vulkan 6 张、tooltip 页 9 张、粒子页 2 张、render-stages 4 张至多 1 级；scene-border 2 张 2 级；半透明页 12 张至多 3 级（文字的 1 级差异经背后模糊扩散，每张约 6,000–8,800 个像素）；shape-boxes 的路径对照 1 级、8 个像素。CaseCheetah 窗口（文档渲染器）的截图 1–2 级、每张至多 630 个像素：它的字形图集也用 `AtlasPacker`，“高出一半以上的行留给新行”让部分字形换了位置，去掉这条规则时逐字节相同。所有 Chrome 对照仍通过；其余只有已知按时序变化的套件（窗口 HUD、trace 时间戳、异步合并与退休计数）。
- 验证：
  - `render_scene_atlas_contract`（新）：同一帧开、关图集逐像素比较（标签与盒子、单色与彩色文字、阴影、大小 SVG 路径、半透明组、旋转的标签，以及在页上紧挨它、边缘纹素不透明的被裁剪路径），后续 4 帧换成别的文字，占用前面图像交还的区域；绘制 295 → 75 次。Vulkan 在验证层（含同步验证）下：两次录制都往同一页上传、都未提交时依次提交，以及一次录制从未提交后重新录制，与 D3D11 至多差 2 级，零告警。
  - `scene_native_contract`：各类图像的格式与位置、记录的 uv、Atlas 批次只含盒子与本页图像且不跨过没有批次的条目、关闭图集时记录除 uv 外逐字节相同且绘制更多、仍被持有的图像不让出区域而没人持有的让出、图集满时退回独立纹理。
  - `paint_contract`：`AtlasPacker` 的区间重用、合并、行的合并与切分、页尾交还，以及三页 2 万步随机分配与释放互不重叠。
  - 20 个变异中 19 个被发现：遮罩读错通道、文字遮罩取颜色通道、区域不留边框偏移、合批跨过没有批次的条目、跨页合批、仍被持有的区域被回收、记录不采样页、关闭图集、D3D11 与 Vulkan 的区域偏一个纹素、Vulkan 页在可读布局里复制、页从未转为可读、描述符集只按图像去重、空区与空行不合并（两个方向）、空行不切分、页尾空行不交还、已用行不限高度。文字遮罩取颜色通道、空区向后不合并、空行向下不合并起初存活，已补用例（覆盖率保留抗锯齿，两个方向的合并）。“边框只填 0、不复制边缘”与现有内容等价：文字与阴影图像四周本来透明，路径图像逐纹素采样；边框的作用是隔开相邻区域（“区域不留边框偏移”被发现）。
  - `ctest`：Release 147/147、Debug 147/147（新增 `render_scene_atlas_contract` 与 `d3d11_warp_scene_interaction_benchmark`）；Android 零警告。
- 测量（RTX 5090 + Ryzen 9 9950X3D，windows-profile，固定在 CCD0、高优先级，交替运行取中位数；“1 字节”与“图集”是这一步的中间版本）：

| 场景 | 后端 | 版本 | 渲染 CPU p50 / p95 / p99（ms） | GPU p50（ms） | 每帧绘制 |
| --- | --- | --- | --- | --- | --- |
| K 线扫动 | D3D11 | 12.2 | 0.907 / 1.629 / 1.732 | 0.109 | 57.6 |
| K 线扫动 | D3D11 | 12.3 | 0.828 / 1.260 / 1.340 | 0.102 | 8.7 |
| K 线扫动 | Vulkan | 12.2 | 0.844 / 1.290 / 1.365 | 0.092 | 57.6 |
| K 线扫动 | Vulkan | 12.3 | 0.806 / 1.173 / 1.276 | 0.088 | 8.7 |
| 散点图拖动 | D3D11 | 12.2 | 0.777 / 0.895 / 1.255 | 0.158 | 30.0 |
| 散点图拖动 | D3D11 | 12.3 | 0.655 / 0.740 / 0.855 | 0.096 | 3.4 |
| 散点图拖动 | Vulkan | 12.2 | 0.764 / 0.849 / 1.261 | 0.079 | 30.0 |
| 散点图拖动 | Vulkan | 12.3 | 0.716 / 0.791 / 0.862 | 0.078 | 3.4 |

  - 上表是 12.2 与 12.3 交替 6 轮（每轮 240 帧采样），两组的单次 p50 互不重叠。四个版本（12.2、1 字节、图集、12.3）交替 4 轮时噪声约 ±8%，只能看大方向：D3D11 K 线 p95 1.81 →（1 字节）1.38 →（图集）1.27 ms，散点图 p50 0.84 → 0.74 → 0.66 ms；描述符去重测不出。

  - 画廊实时基准（4K 窗口、垂直同步关闭，同样固定 CCD）：

| 场景 | 版本 | FPS | 渲染 CPU p50 / p95 / p99（ms） | 卡片重绘 p50 / p95 / p99（ms） |
| --- | --- | --- | --- | --- |
| K 线扫动 | 12.2 | 583 | 1.393 / 2.756 / 3.391 | 0.989 / 1.694 / 2.069 |
| K 线扫动 | 12.3 | 623 | 1.327 / 2.531 / 3.079 | 0.832 / 1.522 / 1.815 |
| 散点图拖动 | 12.2 | 608 | 1.549 / 2.486 / 2.939 | 0.596 / 0.941 / 1.112 |
| 散点图拖动 | 12.3 | 635 | 1.438 / 2.263 / 2.809 | 0.593 / 0.852 / 0.910 |

  - 交替 5 轮；单次之间相差约 ±15%。固定 CCD 后的数字比 12.1、12.2 记录里（未固定）的低，两者不能直接比较。

  - 首帧（场景第一帧，含建页）：D3D11 K 线 10.2 → 5.9 ms，Vulkan 6.9 → 5.2 ms；进程峰值工作集 288 → 244 MiB。
- 剩下的（12.3 之后两个场景分开采样，D3D11，另记了每次路径光栅化的耗时）：
  - K 线（一帧 0.84 ms）：虚线十字线的光栅化约 32%（水平线 2458×12 每次约 320 µs、平均每帧 0.73 次，竖直线约 110 µs、每帧 0.38 次），它们放不进图集的大纹理约 10%；显示列表约 29%，其中 SVG 值缓存的逐项校验只占 7%；tooltip 文字图像约 12%；规划约 7%。
  - 散点图（一帧 0.63 ms）：刻度文字的字形轮廓随平移每帧重画约 41%（每帧约 8 个、每个约 40 µs，曲线先展开成很细的线段）；显示列表约 50%，主要是拖动时属性变了的元素重新解析（缓存未命中），逐项校验约 2%。
  - 估计：虚线的水平、竖直线画成一串盒子，K 线约 −40%；路径按形状与亚像素位置缓存（不随平移失效），散点图约 −35%；元素只重新解析变了的属性，散点图约 −15%；owner 给节点内容戳只省逐项校验，K 线约 −7%。

**12.2：SVG 基本形状画成盒子**（2026-10-14）：
- 原因（12.1 之后）：散点图拖动时每个圆的设备变换都变，每帧约 108 张路径重新光栅化并各建一张纹理，重绘 p50 5.19 ms。
- 形状（`scene_svg_shape`，`scene_path.hpp`）：rect、circle、ellipse、line 的几何属性按路径的规则解析一次，路径（`scene_svg_path`）与盒子共用，两者不会对同一属性有不同的理解。
- 显示列表（`scene_display_list.cpp`）：
  - 填充：直角或圆角半径相同的 rect、正圆（含 rx 与 ry 相同的 ellipse）画成盒子；
  - 描边：没有虚线时，这些形状的描边画成边框盒子，向外长出半个描边宽度，圆角也长出同样多；直角的 rect 只在斜接（直角在任何不小于 √2 的斜接上限内）或圆角连接时可以，切角连接仍是路径；
  - 线段的描边画成沿线的盒子，平头、方头（两端各长半个描边）或圆头（半径为半个描边）；水平与竖直的线段直接画成轴对齐的盒子，按像素的覆盖份额抗锯齿，与路径的覆盖率逐级相同；
  - 椭圆、两个半径不同的圆角、切角连接、虚线、长度为 0 的线段仍画成路径。
- 着色器（`scene.hlsl`）：盒子判定像素属于哪一条边时，先把位置夹进盒内，旋转与圆角的盒子也一样。原来只对轴对齐的直角盒子这样做，旋转或圆角盒子外侧抗锯齿带上的像素不属于任何一条边，取不到边框色，外缘就是锯齿。CSS 里旋转或圆角的带边框元素同样受益。
  - 与 Chrome 核对（2026-10-14）：画廊原生测试里旋转 25° 的带边框盒子，外缘一个像素 Chrome 155、修正后 148、修正前 249、D2D 参考光栅 214。D2D 参考在这圈像素几乎没有边框色，所以该对照的离群像素上限从 16 提到 40（`max_vector_oracle_outliers`）。
- 验证：
  - `render_scene_shape_boxes_contract`（新）：16 种情形（直角与圆角矩形的填充与描边、斜接与圆角连接、圆、比圆还宽的描边、三种线帽的斜线、水平与竖直线、细于一个像素的竖线、旋转与非等比缩放）画成盒子，与同轮廓的路径（覆盖率）逐像素比较：
    - 只有水平与竖直边的情形最大差不超过 4 级（实测 0–2）；
    - 其余最大差 64 级以内、平均差 0.02 以内（实测最大 22–51、平均不超过 0.012），差在曲边与斜边的抗锯齿：盒子按距离、路径按面积；
    - 椭圆、两个半径不同的圆角、切角、虚线、零长度线段仍是路径；
    - Vulkan 与 D3D11 最大差 1。
  - `scene_cluster_platform`：SVG 列表里的矩形填充、矩形描边与圆改为盒子，位置与尺寸。
  - 15 个变异全部被发现（圆的位置、描边不外长、圆角不外长、圆角连接变直角、切角当盒子、方头不延长、圆头变方、线的方向、虚线当盒子、椭圆当圆、零长度线、描边盒子被填满、水平线横竖颠倒、水平与竖直线仍转向、着色器修正撤回）。
  - 画廊的 Chrome 对照全部通过；css-24 与 Chrome 的平均差 0.1149 → 0.1143。半透明窗口页与 Chrome 的对照与 11.5c 相同。
  - `ctest` Release 与 Debug 各 145/145；Android 零警告。产物与 12.1 基线相比，含 SVG 形状与圆角、旋转边框的截图在边缘像素上变化：画廊 cluster 最大 38 级（散点图的圆），原生 scene 最大 116 级（上面的旋转边框），提示页 80、玻璃页 88、粒子页 57 级，每张至多几千个像素。
- 测量（各 3 次中位数，`kline-sweep` 的 12.1 数字为同法测得）：

| 场景 | 版本 | FPS | 渲染 CPU p50 / p95 / p99（ms） | 卡片重绘 p50 / p95（ms） | 每次重绘的路径光栅化 / 绘制调用 |
| --- | --- | --- | --- | --- | --- |
| 散点图拖动 | 12.1 | 151 | 6.62 / 7.51 / 8.06 | 5.19 / 5.75 | 108 / 112 |
| 散点图拖动 | 12.2 | 618 | 1.56 / 2.34 / 2.77 | 0.65 / 0.98 | 0.4 / 28 |
| K 线扫动 | 12.1 | 618 | 1.05 / 3.16 / 3.76 | 1.48 / 2.18 | 3.3 / 547 |
| K 线扫动 | 12.2 | 601 | 1.23 / 3.32 / 4.13 | 1.48 / 2.42 | 1.9 / 62 |

  - 散点图每秒的图表更新 148 → 330 次。
  - K 线的绘制调用减为九分之一，重绘时间不变：12.2 之后采样，SVG 值缓存的逐项校验约占 26%，变化路径的光栅化约 13%，新纹理约 9%。

**12.1：K 线的 SVG 与文字缓存**（2026-10-14）：
- 原因（12.1 之前的采样，K 线每次图表更新重绘约 3.4 ms）：
  - SVG 属性与样式每帧重新解析，约 15–20%（数值、长度、变换、颜色、路径数据）；
  - tooltip 每次 `innerHTML` 都生成新节点，按节点身份缓存的文字图像每次更新约重建 12 张（复用 0），再各建一张纹理；
  - 路径图像的缓存键是路径全部点的字节串，每帧每张路径都要拼接并比较，约 5%；
  - 旧场景帧在渲染线程析构（整棵树逐节点释放），约 12%。
- SVG 元素的派生值（`SceneDisplayCache`，`scene_display_list.hpp`）：
  - 包括元素自己的变换与透明度，文字的行框原点、尺寸、填充与字形轮廓，路径与填充、描边颜色和描边样式；
  - 按视图与元素身份保存，元素的标签、命名空间、属性、样式与文字逐项相同才复用（各场景 VM 的身份会重复，所以要带上视图）；
  - 次序与异常照旧：奇异变换的元素在透明度、文字与路径之前跳过，取值失败的元素不进缓存；
  - 上限 4096 条、16 MiB，超出时按最近使用淘汰之前各帧的条目。
- 文字图像按绘制来源（`scene_paint_source_key`，不含身份）、缩放与边距建键：同样的文字换了节点也复用。
  - 键与 `scene_paint_source_equal`（不计身份）严格对应：0 与 −0 同一写法，NaN 与任何键都不同；带效果或粒子的节点不共享。
  - 显式图层仍按节点身份缓存，文字与图层分成两个缓存。
- 路径覆盖率的键：
  - 由路径内容摘要（每个路径对象只算一次，对象存活期间按地址记住）、点数、填充规则、描边参数（虚线按摘要）、设备映射与遮罩组成，数值按位比较；
  - 命中时再核对路径（同一对象，或动词与点逐一相同）和虚线，所以摘要碰撞只会重画，不会画错。
- 宿主（`probes/frame_reclaimer.hpp`，新）：画廊窗口把场景槽位不再保留的帧和被替换的 owner 发布交给一个回收线程析构。场景帧是不可变值，任何线程都可以释放；排队超过 64 个时就地释放。
- 计数：`SceneNativeWork` 新增 `svg_value_builds`、`svg_value_reuses`，探针的 `render-work.json` 同名。
- 验证：
  - `scene_display_cache_contract`（新）：
    - 40 个随机场景各 30 步随机编辑（属性、样式、文字、新身份，偶尔取值失败）：带缓存与不带缓存的显示列表逐字节相同，其中 45 帧走回退；
    - 未变的元素全部复用，改一个属性或换身份只重取一个；两个视图同身份互不干扰；取值失败的元素不进缓存；
    - 源键：17 种单项编辑与 400 对随机编辑，键相同当且仅当相等性成立；
    - 路径：同一对象与同点的另一对象复用；点、虚线、填充规则、设备映射（含亚像素）、描边宽度、遮罩各自重画；同动词同包围盒的两条路径交替绘制各画一次。
  - `scene_native_contract`：相同文字的其他节点复用图像；文字改变或换缩放时另建；原有的淘汰反例改为换文字。
  - 19 个变异全部被发现。其中“文字位置置零”是共享代码的错误，带缓存与不带缓存两边一样，由 `scene_cluster_platform`、`d3d11_warp_scene_native` 与 `d3d11_warp_gallery_cluster` 发现。
  - `ctest` Release 与 Debug 各 144/144；产物与 11.5c 基线相比场景像素全部相同，render-stages 的图像淘汰数 35 → 31；Android 零警告。
- 测量（`kline-sweep`，各 3 次的中位数）：

| 版本 | FPS | 图表更新/s | 渲染 CPU p50 / p95 / p99 / 最大（ms） | K 线重绘 p50 / p95（ms） | present p95 / p99（ms） | 每次重绘新建文字图像 |
| --- | --- | --- | --- | --- | --- | --- |
| 12.1 之前 | 406 | 148 | 1.10 / 5.29 / 5.82 / 7.01 | 3.42 / 3.99 | 5.31 / 5.89 | 11.7 |
| 12.1 | 618 | 208 | 1.05 / 3.16 / 3.76 / 5.68 | 1.48 / 2.18 | 3.20 / 3.80 | 0.6 |

  - 回收线程单独 A/B（同一构建、各 3 次）：K 线重绘 p50 1.54–1.62 ms，就地析构 1.92–2.00 ms。
  - 第一次重绘（启动尖峰）两版都约 23 ms。
- 剩下的（12.1 之后采样）：
  - SVG 值缓存的逐项校验约 14%：每个元素都要比较属性与样式字符串。可由 owner 给节点一个内容戳，把校验降为常数；
  - 变化路径的光栅化约 11%，新纹理约 9%：图集合批（12.3）。
- 散点图拖动在 12.1 之后：渲染 CPU p50 6.62 ms，重绘 p50 5.19 ms，每帧约 108 张路径重新光栅化，是 12.2 的起点。

## 第 13 步记录（2026-10-14）

**13.1a：锚点的 CPU 参考路径**（2026-10-14）：
- 接口（`src/scene/scene_anchor.hpp`）：
  - `SceneAnchor`：key、x、y（View 的逻辑 px）、scale、depth、flags（隐藏、相机后、屏幕外）、保留位，32 字节，与 13.3 的 C 结构 `weft_anchor` 相同。标志为 0 时可见：技术路线草案的 `WEFT_ANCHOR_VISIBLE` 改成了“隐藏”位，零初始化的记录可见，宿主不会因为忘了置位而什么都看不到。
  - `SceneAnchorChannel`：一次写替换全部锚点，写线程上按 key 排序（同一 key 取最后一条；排序时 `reserved` 暂存原来的位置，不另外分配）。渲染线程与 owner 两个读者各一个三缓冲：写者填自己的空闲表，用一次原子交换发布并取回被替换的表；读者用一次原子交换取最新的表，在下次取之前独占它。非法值（不是有限数、负的缩放、未知标志、保留位非零）或超过 65,536 个时什么也不发布。
  - `SceneAnchoring`：求解器建节点时解析元素的 `data-anchor*` 属性，存进 `SceneNode::anchoring`（参与节点相等与几何相等的比较，改 pivot 等于几何变化）。不合法的值、svg 内部元素上的锚点是场景诊断，整帧拒绝，与其他语法错误一致（技术路线原写“元素隐藏”）。只看 `data-anchor-` 开头的属性，未知的选项是诊断，`data-anchored` 这类只是开头相像的属性不管。
- 定位（`scene_anchor_map`，显示列表与几何共用）：
  - 映射 = 平移到锚点 × 缩放（`data-anchor-scale: on`）× 平移负的 pivot。元素自己的 CSS 变换（绕 transform-origin）先作用，所以出现时的缩放、伤害数字的上浮照常。
  - 锚点在 View 外（边界算在里面）、相机后或标记屏幕外：`hide` 隐藏，`show` 照画；`clamp` 总是把元素（自身变换后的外接矩形）移进 View 减去边距的范围，不只在屏幕外时，所以锚点越过边缘时没有跳变；放不下的轴居中。
  - 深度：同一父元素下、同一 z-index 的锚点元素按深度由远到近，占用它们原来的位置；深度相同的、非锚点元素保持原序（`scene_anchor_order`）。
  - stay：每个使用方（预处理器的每个 view、`SceneGeometry`）记下锚点元素上一次拿到的锚点（按元素身份，连同 key），表里没有这个 key 时用它；key 变了不沿用；不在帧里的元素在下一次列表时丢掉记录。
- 显示列表：锚点元素的父矩阵换成锚点映射，裁剪重置为根盒（不继承祖先的矩形与圆角裁剪链），子孙随它。有锚点子孙的节点不做成图层（锚点元素本身可以：图层图像随锚点移动）；有锚点子孙的透明度组按根盒合成，不被祖先的裁剪切掉。锚点行内盒、透视平面里的锚点元素整帧回退（原因 `anchored inline box`、`anchored element in a perspective plane`）。D2D 参考光栅没有锚点表，不画锚点元素。
- 命中与几何（`SceneGeometry`）：同一套映射与顺序；锚点元素不受祖先裁剪；隐藏的不命中，几何是原点处的空盒（如 `display: none`）；没有 offsetParent，偏移是锚点给的位置，并作为子孙的 offsetParent。
- 渲染器：`ScenePreparer::prepare`、D3D11 的 `paint`、Vulkan 的 `encode` 与 `draw` 多一个锚点表参数。画廊会话（`GallerySceneSession`）接了宿主的锚点通道：每次推进与输入前取 owner 的表，命中用它，JS 读几何时按它重算。
- 基准：`--hud-benchmark-hardware|warp` 新增锚点模式（`HUD_TESTS.js` 的 `__ceHud.anchor`、`__ceHud.health`）：宿主每帧写全部锚点，渲染 pass 开始时取最新的表（计入渲染 CPU）；掉血按 JSON 推送变化的血条，改宽度或 `scaleX`；锚点模式每档开始时重建名牌，档与档之间互不影响。
- 测量（RTX 5090 + Ryzen 9 9950X3D，固定 CCD0，Release，4K，每档 240 帧）：

| 400 个名牌，每帧 | 引擎写锚点与 JSON | 脚本 | UI 线程 p50 / p99 | 渲染 CPU p50 |
| --- | --- | --- | --- | --- |
| 只移动（锚点） | 0.012 ms | 0 | 0.025 / 0.034 ms | 4.2 ms |
| 移动（锚点）+ 20 个血条改宽度 | 0.017 ms | 0.16 ms | 10.7 / 13.2 ms | 4.0 ms |
| 移动（锚点）+ 20 个血条改 `scaleX` | 0.018 ms | 0.33 ms | 7.1 / 10.1 ms | 4.1 ms |
| 对照：推 JSON，每帧改 400 个 transform 与 20 个宽度 | 0.17 ms | 3.9 ms | 23.3 / 32.1 ms | 4.2 ms |

  - 掉血时 UI 线程的开销来自整页：求解器只要有一处影响布局的变化就重新布局整页（`layout_frame`）；只改 transform 也要重新求样式、建场景并复制全部盒子（`copy_boxes`）。改 `scaleX` 省去布局（10.7 → 7.1 ms），剩下的属于 13.5。
- 验证：`scene_anchor_contract` 与 `render_scene_anchor_contract`（内容见 [status](status.md)）；51 个变异全部被发现；第一轮存活的 2 个（测试节点的圆角简写不起作用、命中的 stay 记忆被几何读取掩盖）补了反例。
- 与技术路线不同：标志位取反（见上）；不合法的属性整帧诊断；`clamp` 总是生效；参考光栅不画锚点元素。技术路线已同步修改。

**13.1b：锚点的 GPU 路径与保留的 pass**（2026-10-14）：
- 帧的版本号：`SceneFrame::revision`，求解器在更新后布局或绘制的内容变了（`Work::paint_changed`、`geometry_changed`）、帧有动画或第一次更新时取一个新的（进程内唯一的计数），否则沿用；`reset()` 之后重新取。0 表示没人担保内容，渲染器不保留。在求解器之外改了帧内容的代码要清零（`scene_particle_page` 曾复制帧删掉粒子后再画，画出了保留的 pass）。
- 延后的显示列表（`SceneAnchorInput::deferred`）：
  - 锚点元素的父矩阵为单位矩阵、裁剪为不限（元素自己的裁剪照常），记下元素的锚点设置、边框盒尺寸、自身变换与它的绘制项范围；范围里的绘制项 `paint::Item::anchor` 为锚点元素序号加 1（内层锚点元素覆盖外层）；
  - 范围里的透明度组不随锚点移动：组的合成用根盒裁剪、不带圆角裁剪链（组里的绘制项各自带着裁剪）；
  - 同一父元素、同一 z-index、至少两个锚点元素的兄弟记为一个排序组（每个兄弟的绘制项范围、锚点元素与在父元素中的位置）；构建时按当时的深度排好；
  - 范围里有背景模糊、粒子、透视平面或效果时，这些项在 CPU 上按目标位置计算，不能延后：预处理器改用 CPU 参考路径（每帧准备，不保留）。
- 规划器：记录的 `clips.z` 存锚点序号；锚点序号超过 pass 的锚点数是 `Diag::PaintList`；锚点元素的路径遮罩不按目标裁切（元素空间里可以有负坐标）；`crop` 不作用于锚点记录（有 `crop` 时不延后）。
- 预处理器：`prepare(..., defer)` 返回可保留的 pass（锚点元素、排序组、根盒尺寸）；每个排序组的范围落在一个按任意顺序都能画的批次里（盒子批次或同一图集页）时可重排。`place()` 每帧按表（与 stay 记忆）解析每个锚点元素，得到 {缩放, x, y, 显示}；缩到 0 的元素隐藏；按深度（相同时按 DOM 顺序）重排排序组，变了就写出全部记录的顺序，内层组先排；不可重排的组顺序变了返回 false，渲染器重新准备这一帧（`kept_rebuilds`）。
- 着色器（`scene.hlsl`，只在编码空间的场景 pass；线性 pass 的接口不变）：顶点阶段按顺序缓冲（t7）取记录，锚点记录（t8）的四个角乘缩放加平移，缩小时补足设备像素的抗锯齿外延，隐藏的收成目标外的一点；像素阶段把像素位置换回元素空间，抗锯齿宽度乘锚点缩放（圆角裁剪、矩形盒的像素覆盖）。
- 渲染器：每个视图保留一份 pass（版本号、缩放、目标尺寸、图层相同时重画）；D3D11 的槽位与 Vulkan 的槽位各自记住装着哪份保留 pass 的记录与裁剪（Vulkan 用槽位自己的缓冲，不随录制回收），重画时跳过记录上传与透视平面内容（D3D11）、只传锚点与顺序。`passes` 照旧计每次绘制，`kept_passes` 计其中重画保留 pass 的次数。
- HUD 基准：锚点各档也在 Vulkan 上跑（`--hud-benchmark-hardware`）。测量（4K，固定 CCD0，Release，每档 240 帧）：

| 400 个名牌只移动（锚点） | UI 线程 p50 / p99 | 渲染 CPU p50 / p99 | GPU p50 |
| --- | --- | --- | --- |
| D3D11，CPU 参考路径（13.1a） | 0.025 / 0.034 ms | 4.16 / 5.17 ms | 0.08 ms |
| D3D11，GPU 路径 | 0.004 / 0.019 ms | 0.021 / 0.114 ms | 0.06 ms |
| Vulkan，GPU 路径 | 0.006 / 0.029 ms | 0.042 / 0.111 ms | 0.23 ms |

- 验证：见 [status](status.md)；33 个变异全部被发现；第一轮存活的 3 个里两个补了反例（缩到十分之一的元素、Vulkan 换版本号），一个与重置后整帧重建等价，删去了那行代码；Debug 运行发现的释放后读取（stay 记忆在排序前被替换）已修正并补了反例。
- 与技术路线不同：顺序缓冲是全部记录的顺序（不只锚点子树），跨批次的重排直接重新准备；保留 pass 的范围比锚点更广（版本号不变的任何帧都重画保留的 pass）。

**13.2：桥接**（2026-10-14）：
- 值（`src/bridge/bridge_value.hpp`，库 `weft::bridge`，只依赖 `weft::base`）：MessagePack 的子集，自己写的读写器（约 800 行，无第三方库）；`valid_value` 查完整的值（类型、UTF-8、键、深度 64、大小 1 MiB、没有多余字节）；`value_from_json` 是严格的 RFC 8259 解析，`value_to_json` 用于诊断与测试。上限在 `constants/bridge.hpp`。
- 通道（`src/bridge/engine_channel.hpp`）：`EngineChannel` 一把锁，任意线程 `emit`（可选 latest）、`complete`、`set_model`（各有 JSON 版本）；owner `open_call`、`close_call`、`take`、`reset`。返回 `ChannelStatus`：Ok、Invalid（名字或值不合法）、Full（超过上限，什么也不收）、Stale（调用不在打开状态）。满了时完成不关闭调用，宿主可以稍后再完成。视图模型按模型、字段合并（同一批里重复的键只算一次字节）。
- ScriptDom（`script_dom.hpp`）：`install_engine(shared_ptr<EngineChannel>)`、`install_engine_function(name, fn)`、`take_engine_messages()`、`deliver_engine()`（返回 `EngineDelivery`：批次、事件、完成、stale 的个数与处理函数的异常）。原生方法 `__ceEngineSend`、`__ceEngineCall`、`__ceEngineInvoke`、`__ceEngineClose` 挂在节点原型上（JS 模块经 `document` 调用）；宿主绑定 `__ceEngineHost` 的 `event`、`settle`、`model`、`errors`。同步函数运行期间 `check_owner()` 拒绝任何进入（`WEFT_ENGINE_REENTRY`）。值的转换见 [技术路线 6.1](engine-communication.md#61-值)。
- JS 模块：`src/script/js/engine_bootstrap.js`（`ScriptPlatformModule::Engine`，`weft.engine`）与 `crpg_bridge_bootstrap.js`（`CrpgBridge`）；`weft` 由各模块合并（场景平台的 `weft.emitParticles` 改成合并，与安装顺序无关）。
- 基准：`weft_bridge_benchmark --quick|--full <dir>`（`probes/bridge_benchmark.cpp`，可移植，不用 GPU）：CRPG 的“JSON 拼进脚本求值”对比桥接（引擎给 JSON、引擎写 MessagePack），同一个按 CRPG ue-bridge 写的页面；`bridge_benchmark_quick` 是它的短跑测试（三种方式的校验和必须相同）。
- 与技术路线不同：
  - 交付顺序定为视图模型的批次先于事件与完成（处理事件时看到的是新的模型）；
  - latest 替换同名的最后一个未交付事件，并留在它原来的位置；
  - 视图模型的句柄多一个 `name`，订阅者拿到 `(value, changed)`；第一批之前 `value` 是 undefined；
  - 从 JS 转出时按 JSON.stringify 处理 toJSON 与 Proxy 包着的数组，孤立代理项变成 U+FFFD，−0 保持浮点；
  - 同步函数在 13.2 是 C++ 的 `std::function`，结果是 bridge 值的字节（C ABI 的写入器在 13.3）；
  - CRPG 兼容层把视图模型 `vm` 转给 `__vm_update`（CRPG 一个 widget 只绑一个视图模型）。

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
