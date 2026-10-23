# Weft 构建与 M0 探针

当前交付的实验模块按 `src/<模块>` 组织，每个模块是一个静态库 `weft::<模块>`：`base`、`core`、`render`、`canvas`、`script`、`text`、`style`、`layout`、`scene`、`input`；Windows 另有 Direct2D 参考 raster `weft::canvas_d2d`、`weft::scene_d2d`，DirectWrite 字体发现 `weft::platform_win32`，D3D11 参考渲染器 `weft::render_d3d11`（渲染器、C ABI 适配器、同步与两帧宿主；HLSL 在构建时由 fxc 编译内嵌，见 `cmake/WeftShaders.cmake`），以及原生场景 pass `weft::scene_d3d11`。scene 的文字（HarfBuzz/FreeType）、几何与命中属于可移植的 `weft::scene`。公开 C ABI 头位于 `include/weft/`，模块内部头以 `"<模块>/<文件>.hpp"` 从 `src/` 根引用。CMake 辅助函数 `weft_add_library`、`weft_add_executable`、`weft_add_test`、`weft_add_python_test` 在 `cmake/WeftTargets.cmake`，固定第三方依赖在 `cmake/WeftThirdParty.cmake`。受限 React 页面与原样 CaseCheetah 九宫格已串联 native DOM、CSS profile、布局、字体/图片、GPU 和 Windows 窗口；一般 HTML → CSS 页面加载尚未实现。公开头文件是实验接口，当前不交付稳定 SDK 或第三方符号隔离承诺。

## Windows x64

需要 CMake 3.25 以上、Visual Studio 2022 C++ 工具链与 Windows SDK。Vulkan 后端（`weft::render_vulkan`，`WEFT_RENDER_VULKAN`）另需 Vulkan SDK 1.4.363.0：构建时用它的 DXC 1.9.0.5512 把共享着色器编成 SPIR-V（`cmake/WeftSpirv.cmake` 核对版本），测试时用它的验证层；没有这个 DXC 时该后端自动不构建。头文件是固定版本的 vendored Vulkan-Headers，运行时打开驱动自带的加载器，不链接 SDK 的库。Metal 后端的着色器（`WEFT_SHADERS_METAL`，2026-10-17）另用同一 SDK 的 spirv-cross（revision `vulkan-sdk-1.4.357.0-39-gf11ba9f0`，`spirv-cross.exe` SHA-256 `6702f1d74b2aaff02cff452f7c96bddb947b8d93fee24af6af0df2692c2e0ea9`，Apache-2.0）把 DXC 的 SPIR-V 写成 MSL（`cmake/WeftMetal.cmake` 核对 revision，不符即停，除非 `WEFT_SPIRV_CROSS_REVISION` 指名另一个）；有 DXC 与 spirv-cross 时默认开，Windows 与 Android 构建里也生成并检查 MSL；Metal 适配器本身尚未实现。当前验证版本见 [status](status.md)。Python 3 用于离线依赖校验、fixture 完整性、浏览器几何差分和 GPU 颜色合同比较；缺少 Python 时 CMake 不注册相应 Python 测试，必须另行执行，不能把少量测试通过当作完整验证。C++ 源要求 C++20；QuickJS 与 C 调用 smoke test 使用 C11。

在项目根目录执行：

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
cmake --build --preset windows-release --parallel 4
ctest --preset windows-release
```

程序位于 `build/windows-msvc/bin/Debug` 或 `Release`。Visual Studio 是多配置生成器，实际编译模式以 `--config` / build preset 为准；第三方配置日志中的默认 `CMAKE_BUILD_TYPE=Release` 不能替代该选择。

`portable-ninja` preset 供已初始化本机工具链的环境使用，目前没有单独验证；它不构成 Android/iOS 配置。

## Android arm64 交叉编译（2026-10-12）

需要 Android NDK r30（30.0.16248370，`android-ndk-r30-windows.zip`，727,980,788 字节，SHA-1 `9bf167a1985fa7d4a036186b78f702eab9179408`，取自 dl.google.com，适用 2026-04-28 版 Android SDK License Agreement）、Ninja，以及构建 SPIR-V 用的 Vulkan SDK 的 DXC（主机工具，与 Windows 构建相同）。`ANDROID_NDK_HOME` 指向解压后的 NDK 根目录：

```powershell
$env:ANDROID_NDK_HOME = "F:/SDKs/ndk-r30/android-ndk-r30"
cmake --preset android-arm64
cmake --build --preset android-arm64
```

产物在 `build/android-arm64`：全部可移植模块、Vulkan 后端（含 Android 窗口的 surface）、Android 平台层 `weft::platform_android`（NDK 系统字体迭代器，API 29），以及 36 个测试可执行文件（arm64-v8a ELF，`ANDROID_PLATFORM` android-29，`c++_static`）。Vulkan 在运行时由 `libvulkan.so` 打开，不链接它。这台 Windows 机器只能编译与链接；可执行文件没有在模拟器或真机上运行过。D3D11、Direct2D 与 DirectWrite 部分、探针只在 Windows 构建。

`windows-profile` preset 构建到 `build/windows-profile`：代码生成与 Release 相同（`/O2 /Ob2`、`/OPT:REF,ICF`），另带 PDB，供采样剖析符号化（例如 `--gallery-owner-sample`，见 [交互性能](gallery-interaction-profile.md#owner-采样剖析)）。

## macOS arm64（2026-10-21）

不需要 Xcode：Command Line Tools 加 Homebrew 的 `cmake`、`ninja` 与 `llvm`（clang 23.1.4）。Command Line Tools 的 libc++（SDK 15.2）没有 `double` 的 `std::from_chars`（`bridge_value.cpp`），所以编译器与 libc++ 用 Homebrew LLVM 的：

```sh
brew install cmake ninja llvm
LLVM=/opt/homebrew/opt/llvm
cmake --preset portable-ninja -B build/portable-llvm \
  -DCMAKE_C_COMPILER=$LLVM/bin/clang -DCMAKE_CXX_COMPILER=$LLVM/bin/clang++ \
  "-DCMAKE_EXE_LINKER_FLAGS=-L$LLVM/lib/c++ -L$LLVM/lib/unwind -lunwind -Wl,-rpath,$LLVM/lib/c++ -Wl,-rpath,$LLVM/lib/unwind"
cmake --build build/portable-llvm
ctest --test-dir build/portable-llvm
```

Release 加 `-DCMAKE_BUILD_TYPE=Release`（另一个 `-B` 目录）。构建的是全部可移植模块、`weft::platform_macos` 与非 GPU 的测试和探针；没有 GPU 后端（Metal 适配器 M.3 尚未实现；`WEFT_SHADERS_METAL` 需要 Vulkan SDK 的 DXC 与 spirv-cross，未装时不生成 MSL）。Apple 上的构建差异都在 CMake 里：

- 全部目标 `-ffp-contract=off`：arm64 上的 clang 默认把 `a * b + c` 合成 FMA，参考数据出自不合成的 MSVC x64（Yoga 的 flex 收缩 60 - 40 得 19.9999981）。
- Yoga 另加 `-frtti`（`cmake/WeftThirdParty.cmake`；上游默认 `-fno-rtti -fvisibility=hidden`）：否则它的目标文件带有隐藏的标准异常 type info，静态链接后整个程序的 `catch (const std::exception&)` 都绑到这些副本，Apple 的 libc++abi 按地址比较 type info，于是什么也接不住。依赖文件与 `metadata.json` 未改。
- 一个字体文件的上限 128 MiB（其他平台 32 MiB）：PingFang 的集合 75 MiB。

系统字体由 `src/platform/macos`（CoreText 列出，FreeType 找集合里的下标与名字）提供，见其头文件。

Metal 后端与窗口示例（2026-10-21）：生成 MSL 要 Vulkan SDK 1.4.363.0 的 DXC 与 spirv-cross（macOS 版有同样固定的版本；`vulkansdk-macos-1.4.363.0.zip` 解压后用安装器的命令行装进用户目录即可，不要 sudo），配置时 `VULKAN_SDK` 指向它的 `macOS` 目录，Vulkan 后端关掉（Metal 不经 MoltenVK）：

```sh
./vulkansdk-macOS-1.4.363.0.app/Contents/MacOS/vulkansdk-macOS-1.4.363.0 --root ~/VulkanSDK/1.4.363.0 \
  --accept-licenses --default-answer --confirm-command install
export VULKAN_SDK=~/VulkanSDK/1.4.363.0/macOS
cmake --preset portable-ninja -B build/macos-metal -DWEFT_RENDER_VULKAN=OFF ...   # 其余参数同上
cmake --build build/macos-metal
open build/macos-metal/bin/weft_macos_demo.app
```

`WEFT_SHADERS_METAL` 随之打开，Apple 上再构建 `weft::scene_metal` 与 `samples/macos-host`（`weft_macos_demo.app`：窗口里游戏的地面与单位上是 engine-host 的 HUD，按 1–5 换成 logo 的四个版本，R 重新载入，V 开关垂直同步（默认关：窗口里约 260 fps，受系统合成器交还 drawable 的速度所限，全屏 F 约 450 fps），`--page` 也接受 hud、logo、color、dark、light、static；`--shot <png>` 无窗口画若干帧写 PNG，`--capture <png> --quit-after <s>` 把窗口 3 秒时的画面写成 PNG）。Objective-C++ 用同一个 clang；LLVM 23 的类消息存根要较新的链接器，CMake 在支持时加 `-fno-objc-msgsend-class-selector-stubs`。

Xcode 27.1 的 Apple clang 21（macOS 27 SDK 的 libc++ 有 `double` 的 `std::from_chars`）同样能构建上面的全部目标，不必装 Homebrew LLVM：不给编译器与链接参数即可（2026-10-23，Apple M5，macOS 26.6；`ctest` 同为 68/76）。测试脚本要 Python 3.10 以上（`zip(strict=)`），Xcode 自带的 3.9 不够，用 `-DPython3_EXECUTABLE=/opt/homebrew/bin/python3`。

## iOS arm64（2026-10-23）

Xcode 27.1 的 iOS SDK 与 Apple clang，MSL 由同一份 Vulkan SDK 生成。iOS 上 `double` 的 `std::from_chars` 从 iOS 26 才有，最低版本 26.0。模拟器用 Ninja：

```sh
export VULKAN_SDK=~/VulkanSDK/1.4.363.0/macOS
cmake -G Ninja -B build/ios-sim -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
  -DWEFT_BUILD_TESTS=OFF -DWEFT_BUILD_PROBES=OFF -DWEFT_RENDER_VULKAN=OFF
cmake --build build/ios-sim --target weft_ios_demo
xcrun simctl boot "iPhone 18 Pro"
xcrun simctl install booted build/ios-sim/bin/weft_ios_demo.app
xcrun simctl launch --console-pty booted dev.weft.ios-demo          # 可加 --page hud|logo|color|dark|light|static
```

`-DCMAKE_OSX_SYSROOT=iphoneos` 是真机 SDK（编译与链接，不签名）。`WEFT_BUILD_TESTS=ON` 时测试也编成模拟器程序，`xcrun simctl spawn booted build/ios-sim/bin/<test>.app/<test> [参数]` 在模拟器里运行（例如 `weft_logo_reference_metal_tests <输出目录>`）。

真机要签名的 App，用 Xcode 生成器。先在 Xcode → Settings → Accounts 登录 Apple ID（免费账号的 App 7 天后要重装），iPhone 连上 Mac、信任这台电脑并打开 设置 → 隐私与安全性 → 开发者模式。`WEFT_IOS_DEVELOPMENT_TEAM` 是团队 ID（Xcode 的账号页，或钥匙串里 Apple Development 证书的组织单位），`WEFT_IOS_BUNDLE_ID` 换成自己的（免费团队拿不到别的团队已注册的标识）：

```sh
cmake -G Xcode -B build/ios-xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
  -DWEFT_BUILD_TESTS=OFF -DWEFT_BUILD_PROBES=OFF -DWEFT_RENDER_VULKAN=OFF \
  -DWEFT_IOS_DEVELOPMENT_TEAM=<团队 ID> -DWEFT_IOS_BUNDLE_ID=<com.example.weft-demo>
open build/ios-xcode/Weft.xcodeproj                                  # scheme weft_ios_demo，目标选那台 iPhone，Run
```

`samples/ios-host`（`weft_ios_demo.app`）：与 macOS 示例共用引擎、Metal 与页面的部分（`samples/macos-host` 的 `engine.hpp`、`gpu.hpp`、`page.hpp`），UIKit 场景里的 CAMetalLayer 随 CADisplayLink 绘制（ProMotion 上最高 120 Hz），View 占屏幕的安全区，手机横屏。一个触点是指针：移到触点、按下、抬起，然后离开（点过不留悬停）；被系统取消的触点只离开，不点击。页面在那里没有可交互元素时，触点伤害那里的单位。右上的 Next page 依次换成 logo 的四个版本；左上一行是页面、帧率与每帧 CPU、GPU 时间，stdout 每秒同样打印（Xcode 的控制台或 `simctl launch --console-pty`）。页面随包复制到 `examples/`（真机读不到源码目录）。系统字体与 macOS 一样经 CoreText 列出（`weft::platform_macos` 在 iOS 上也构建）。


## 固定依赖与离线构建

固定 QuickJS-NG 0.17.0、Yoga 3.2.1、FreeType 2.13.3、HarfBuzz 11.2.1 与 Ahem 测试字体，源码位于各 `third_party` 目录。D3 7.9.0 原版 UMD 仅作为兼容性输入。来源、版本、commit、archive SHA256、逐文件 SHA256 与许可见 `third_party/manifest.json` 和依赖 metadata。只有 Yoga 的 CalculateLayout.cpp 有已保存的小补丁，其他上游源码未修改；构建不使用 FetchContent，也不追踪可变分支。

只构建 Yoga 的 yogacore，避免引入其上游测试的下载过程。FreeType 关闭 ZLIB/BZIP2/PNG/HARFBUZZ/BROTLI；HarfBuzz 关闭 subset、utils 与 CoreText，其上游 CMake 因 freetype target 存在自动开启 FreeType 接口。我们的测量使用 hb_ot_font_set_funcs。MSVC 的三个新依赖 target 设 /utf-8，保持源码原字节。HarfBuzz 上游偏好 Meson，当前固定版本的 CMake 实际构建通过；不据此保证未来版本配置相同。

```powershell
python tools/verify_dependencies.py
```

校验会拒绝缺失、增添或内容变化的文件。D3 的 bundle 同时保留所含 37 个模块的许可和来源核对记录。更新依赖时需要重新验证来源和行为后审阅 lock 的变化；校验失败时不能直接刷新 hash。

2026-10-16 新增未经修改的 lodepng（commit e584a84，`LODEPNG_VERSION_STRING` 20261006）全部 26 文件：scene 页面 `url()` 背景图的 PNG 解码，只编 `lodepng.cpp` 为私有静态库 `weft_lodepng`，关掉编码器、文件读写、辅助块与 C++ 接口，zlib 许可（来源/开关见 [记录](../third_party/lodepng/README.md)）。此前固定文件共 5,106 项，新增未经修改的 libunibreak 6.1 全部 61 文件（私有静态、zlib 风格及 Unicode 条款，来源/开关见 [记录](../third_party/libunibreak/README.md)）；含原始 React 19.3.0/react-dom 19.3.0/scheduler 0.28.0、Zustand 5.0.15 及 MIT notices，另固定 Noto CJK Sans2.004 字体与 SIL OFL 1.1。FreeType 选择 FTL 授权，原文和 notices 保留；HarfBuzz 源码中的独立字体许可也一并保存，探针 Roboto Regular 另在 third_party/probe-font-notices 补齐 Apache-2.0 原文、归属与 hash。Ahem 的公共领域/CC0 fallback 是字体自身许可，不由 FreeType 许可替代。Yoga 补丁的原始/修改后 hash 与删除条件在其 metadata 中。

新增 `flow_layout_contract`、browser diff/fixture integrity、原始 Unicode 数据测试，以及 `--flow-warp/hardware`、`--cheetah-flow-warp/hardware` 的真实滚动/圆角像素探针。输入、离线参考、强制 GC 和本机浏览器步骤见 [flow 复现](flow-layout-probe.md)。正常 CTest 无需联网或重建 React bundle。

另新增 `flow_input_contract`、`flow_input_browser_diff`、`flow_input_fixture_integrity` 和 `d3d11_warp_cheetah_flow_window`。原样 React 长列表接 HWND 细分/取消 wheel、paint-order/inline/rounded hit 和 scroll-only repaint，独立值接口保持 renderer C ABI v1。可见窗口为 `weft_d3d11_probe.exe --cheetah-flow-window-hardware artifacts/local/cheetah-flow-window-interactive`，隐藏自测追加 `-test-`；fixture 和强制 GC 命令见 [flow 输入窗口](flow-input-window-probe.md)。

## Flex 与真实字体

`font_fallback_contract` 验证固定 Roboto/Noto CJK 的缺字回退与 cluster/连字；`--cheetah-alert-hardware` / `--cheetah-alert-warp` 验证原样 AlertBar/Zustand/UE 协议、中文灰度与真实 GPU，详见 [复现与范围](cjk-alert-probe.md)。普通构建使用固定 IIFE；仅重建样例需要 `node tools/build_cheetah_alert.mjs`。

```powershell
.\build\windows-msvc\bin\Debug\weft_flex_layout_probe.exe third_party/ahem/Ahem.ttf artifacts/local/flex-native-debug
python tools/compare_flex_reports.py tests/fixtures/flex_browser_reference.json artifacts/local/flex-native-debug/css-adapter.json --output artifacts/local/flex-native-debug/adapter-comparison.json
```

程序的两个参数为字体文件与输出目录，产生 raw-yoga.json/css-adapter.json。两份报告都使用已打补丁的 Yoga，raw 仅关闭一部分 CSS 适配。CTest 自动生成各配置报告，和随源码保存的实际浏览器参考比较，正常测试不依赖在线浏览器。输入是显式样式值树；不是 DOM/CSS 页面，也没有 native 像素。完整 profile、补丁、独立浏览器捕获命令与字体限制见 [探针文档](layout-text-probes.md)。

## 真实 D3D11 提交与颜色

Windows 的 `weft_d3d11_probe` 链接系统 D3D11、DXGI、D3DCompiler 与诊断 PNG 用的 WIC，不增加 vendored 依赖。它仍是独立程序，未成为正式 renderer 模块。默认 CTest 用 WARP 执行实际 command list/query，离线验证 browser 参考；硬件另用显式参数验证，不自动降级。

```powershell
.\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --hardware artifacts/local/gpu-debug/hardware
python tools/compare_gpu_reports.py tests/fixtures/gpu_color_browser_reference.json artifacts/local/gpu-debug/hardware/report.json --output artifacts/local/gpu-debug/hardware/browser-comparison.json
```

输出 report.json、direct.png、rtt.png，编译模式来自对应目录。固定 SM5 shader（2026-10-11 起是共享的 `src/render/shaders/scene.hlsl` 线性变体）在构建时由 Windows SDK 的 fxc 编译并内嵌（`cmake/WeftShaders.cmake`），运行时不读源码，也不需要 D3DCompiler。本机硬件与 WARP 都实际启用调试层，没有 warning/error。十个颜色检查与独立 browser PNG 的数值边界、透明 alpha、资源退休和等待职责见 [GPU 探针](gpu-probe.md)。

## 可扩展 GPU 后端 API

`render/render_backend`（属 `weft::render`）不链接图形 API、窗口、字体或脚本库。它实现 v1 C ABI facade，由 host 注册 adapter function table、传具体 native pass、实际提交与完成。D3D11 reference、React window 与 Vulkan adapter（2026-10-12，`render_vulkan_backend_contract` 以宿主身份驱动它）已接入；原生 Metal adapter 尚未实现。运行 `weft_d3d11_probe.exe --backend-hardware artifacts/local/backend-debug/api-hardware` 可验证宿主 context/target、保留已有内容、颜色/SDF 对照、float alpha、取消/失败与真实 query 退休；软件入口为 --backend-warp。接口与限制见 [后端 API](render-backend-api.md)。

## SDF 字体

字体适配器增加独立的 rasterize_sdf，默认使用固定 FreeType 的 bsdf，保留原有灰度接口。CTest 的 sdf_font_contract 验证距离编码、padding、塑形度量与错误输入，d3d11_warp_sdf 执行真实 GPU 缩放/描边/readback 和资源退休。硬件显式执行：

```powershell
.\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --sdf-hardware artifacts/local/sdf-debug/hardware
```

输出 report.json、direct.png、rtt.png、distance-atlas.png。自动 WARP 产物位于 build/windows-msvc/artifacts/sdf-Debug/warp 与 sdf-Release/warp。固定 atlas 不是生产缓存，中文、页面、移动与 MSDF 尚未验证；质量/布局边界和完整复现见 [SDF 字体](sdf-font-probe.md)。

## React / Vite 页面

普通 CMake 构建直接使用已保存的 Vite IIFE，不需要 Node 或网络。实际 React 操作自己的 native DOM，再经受限 inline CSS/Flex/字体输出三帧 GPU 像素：

```powershell
.\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --react-hardware artifacts/local/react-debug/hardware
python tools/compare_react_reports.py tests/fixtures/react_browser_reference.json artifacts/local/react-debug/hardware/report.json --output artifacts/local/react-debug/hardware/browser-comparison.json
```

WARP 使用 --react-warp。25 个元素 × 三帧的 300 几何字段与独立 Chromium 参考对照，state/keyed identity、文字像素、计数/重排后的图像改变、direct/RTT 和实际资源退休同时验证。仅支持显式 Flex、受限行内样式与单行 Roboto；没有 HTML parser 或样式表层叠。这个离屏 runner 直接派发事件，输入窗口使用下面独立入口。Node/Vite 重建、原版 runtime 校验、浏览器预览和 golden 捕获见 [完整步骤](react-vite-probe.md)。

## 原生 React 窗口

运行 `weft_d3d11_probe.exe --react-window-hardware artifacts/local/window-interactive` 可打开主鼠标窗口；WARP 使用 --react-window-warp。自动测试使用 --react-window-test-hardware/warp 创建自己的隐藏 HWND，实际运行消息/命中/React/swapchain。完整步骤与边界见 [窗口说明](native-window-probe.md)。

## 原版 D3 与原生 DOM

同一份 `tests/fixtures/d3_probe.js` 用于浏览器与原生探针。它覆盖 HTML/SVG namespace、NS attributes、keyed join、wrapper identity、排序与退出、axis、line、事件替换与传播。不会注入 JS DOM 实现，也不会修改 D3。

```powershell
.\build\windows-msvc\bin\Debug\weft_script_dom_probe.exe third_party/d3/d3.js tests/fixtures/d3_probe.js artifacts/local/d3-native-debug
```

前两个参数为 D3 和 fixture 路径，均必需。第三个参数指定生成目录；产物为 `d3-native-report.json` 与 `d3-native-dom.json`。路径相对于当前工作目录，也可使用绝对路径。

浏览器参考页面为 `tests/fixtures/d3_chart_reference.html`。浏览器比较步骤和已保存证据见 [前端库验收](frontend-libraries.md)。浏览器生成的 SVG 图形只用于参考，原生报告明确标记 `native_rendering_verified=false`。

保存浏览器报告后，可独立比较全部 JSON 字段；差异会使命令退出失败：

```powershell
python tools/compare_d3_reports.py artifacts/local/d3-reference/report.json artifacts/local/d3-native-debug/d3-native-report.json --output artifacts/local/d3-native-debug/browser-comparison.json
```

## 强制 GC

下面的构建启用 QuickJS 上游的“每次分配前完整 GC”，用于放大遗漏的 root/mark 边界：

```powershell
cmake -S . -B build/windows-gc-stress -G "Visual Studio 17 2022" -A x64 -DWEFT_BUILD_TESTS=ON -DWEFT_BUILD_PROBES=ON -DQJS_ENABLE_GC_STRESS=ON
cmake --build build/windows-gc-stress --config Debug --parallel 4
ctest --test-dir build/windows-gc-stress -C Debug --output-on-failure
```

脚本探针设有 2 秒执行 deadline、10,000 个 microtask、32,768 个原生节点、256 层树深度和 256 MiB VM 分配上限（13.4 由 64 MiB 放宽：CRPG 的浮动 UI 有 200 个单位时 VM 峰值约 79 MiB）。VM 分配上限统计请求大小与自有 header，不是进程 RSS，也不包含全部原生 DOM 内存。OOM、超时和节点超限需要 reset VM；这些是 M0 故障验证参数，不是产品性能预算。

默认 VM 栈上限为 512 KiB，本机默认 1 MiB 宿主线程栈通过 D3 和深递归恢复。React 使用 1 MiB VM 上限，page/input/GPU 三个实验 exe 通过 /STACK 保留 8 MiB 宿主线程栈。接入宿主时必须为 native bridge 留余量；尚未提供可移植的线程栈容量检测。

ScriptConfig 可配置受限执行预算，默认仍为 2 秒。只有 QJS_ENABLE_GC_STRESS=ON 的 React page/input/GPU workload 使用 30 秒执行预算、180 秒 CTest timeout，以完成每次分配前完整 GC 的压力场景；现有预算超限反例继续用默认值。该配置不用于性能验收。

## 公共样式与原始主题

`--cheetah-style-hardware` / `--cheetah-style-warp` 使用原样 AlertBar bundle、原始 theme block/expanded rule 与明确尺寸适配。浏览器准备工具 `python tools/build_cheetah_style.py` 只写自己的 example，普通 native 构建直接用既有固定资源。实际范围、预览方法和 artifact 路径见 [公共样式合同](style-cascade-probe.md)。本阶段最终 Debug/Release 37/37，新增范围和八次 React GC 7/7；不是移动或完整 CSS 验收。

## 测试含义

| 测试 | 验证内容 |
| --- | --- |
| style_fixture_integrity | 固定原始主题/展开规则、组件 bundle、资产副本和 browser provenance；需要 Python |
| style_cascade_contract / style_browser_diff | token var、selected-fallback cycles/IACVT、CSS-wide、cascade/selector、边界与 192 个 browser 颜色分量 |
| dom_contract | 不依赖脚本引擎的原生 DOM 树（插入次序与移动、拒绝的原子性、深度上限、变更日志、快照、查询、释放与 epoch）；按节点 slot 与代际索引的旁表；canvas width/height 的 HTML 非负整数解析与 2048 上限 |
| selector_contract | DOM 查询与样式共用的选择器引擎：名称大小写、标识符开头、属性运算符、class 空白、需要回溯的后代匹配、伪类、specificity、列表、两种文法与两类诊断 |
| d3d11_warp_cheetah_style / cheetah_style_browser_diff | 原始主题/展开 CSS 的八帧实际 GPU、资源退休；2,672 颜色分量和 1,336 几何字段 |
| dependency_integrity | 固定源码与许可文件未改变；需要 Python |
| flex_fixture_integrity | C++/browser 输入来自同一 JSON/hash，生成文件没有过期；需要 Python |
| react_fixture_integrity | 固定 JSX/toolchain/runtime 输入与 IIFE 输出 hash、版本和 MIT notices；不需要 Node，需 Python |
| core_contract / c_api_smoke | 实验 C ABI、C 调用、owner 线程、代际句柄、失败原子性和 host 时间 |
| resource_lifetime | CPU/Prepared/Encoded/Submitted pin、取消、实际完成、失败、共享队列、epoch 与 shutdown |
| render_backend_contract / render_backend_c_abi | 可注册 API/native schema、instance/epoch/generation、能力、BGRA、snapshot/部分失败、无分配提交、容量、drain/abandon；C11 实际 ABI |
| d3d11_warp_backend_api | 真实宿主 context/target、颜色/coverage/SDF 与 reference 像素一致、float alpha、目标内容保留、取消/失败和 query retirement |
| d3d11_warp_render_cache | 固定 seed 的 48 帧缓存/全量像素对照，源图/字号/裁剪/透明顺序、完整依赖 LRU、丢包、容量拒绝、失败重试和 query drain |
| d3d11_warp_two_frame | 两个真实 Submitted use、独立 constant buffer/readback、同队列上传复用、48 帧差分、slot/host lease、提交失败及关闭 drain |
| script_dom_lifecycle | 原生/JS 统一强引用图、detached node、事件路径、循环回收、随机树操作、线程与 View epoch、执行资源上限 |
| d3_native_dom | 原版 D3 生成和更新原生 HTML/SVG DOM，并输出 29 项行为报告 |
| text_font_probe | FreeType metrics/coverage，HarfBuzz glyph/advance/cluster，字号变化、缺字和 owner |
| sdf_font_contract | FreeType 距离编码、padding、generator/spread 隔离、度量不变、空格与错误输入 |
| font_catalog_contract | 字体族列表解析、CSS Fonts 4 字重/斜体匹配、generic 与回退族；不依赖平台 |
| scene_text_metrics / scene_text_browser_diff | 本机字体上的 scene 文字：852 个 span 的字体选择、宽度（1/64 px 内）、高度与基线对照 Chrome 155；TTC 与可变字体上的 SDF；需要 Python |
| scene_text_image | 文字节点定位、颜色、对齐与彩色字形报告；SDF field 按 scene 布局在 1×/2×/4× 重建覆盖率，平移 1 px 必须失败 |
| flex_adapter_contract | measure 被跳过后最终文本仍排版、宽度/字体变化、basis 差异、盒约束与明确诊断 |
| box_layout_contract | 文档与 scene 共用的 Yoga 适配器：block 子项不 flex、flex item 自动最小尺寸、文字溢出可用高度、grid 隐藏子项、行内单行、svg 属性与比例、transform 包含块、隐藏子树、视口与输入诊断 |
| hit_test_contract | 文档与 scene 共用的命中路径：半开矩形、闭合边界、圆角、填充规则、描边端点与虚线、绘制顺序、跳过的目标、裁剪链与平面 |
| scene_transform_contract | scene 唯一的 transform 模型：合成顺序、百分比、CSS 与 SVG 角度、旋转中心、分隔符、诊断、透视平面与 transform-origin |
| flow_input_contract / browser_diff / fixture_integrity | 3,150 点命中、128 随机算术 mutation、inline/圆角/滚动、完整 wheel、owner/GC 与 source/font hash |
| d3d11_warp_cheetah_flow_window | 原样 React 的 HWND mouse/wheel、signed/receipt viewport、resize/cancel/close、GPU query 和 readback |
| d3d11_warp_cheetah_flow_window_4k | 3840×2160 中英字形、两级 letterbox 命中、idle 合成/FPS 时钟、HUD 替换与实际 completion serial |
| d3d11_warp_cheetah_flow_window_motion | 连续 DOM 位置更新、30 Hz 参照、浮点/跳帧时间、pause/speed、minimize/resize、原始 React 共存、像素与退休 |
| mouse_input_contract | 布局命中后的真实 React MouseEvent、GC/取消/disabled/hidden、共同祖先与 detached/epoch |
| d3d11_warp_window | 实际 HWND/swapchain/Present、mouse/resize/letterbox/取消/关闭、post-submit failure 与 query retirement |
| page_react_contract | 八轮真实 React mount/GC/useState/unmount/reset、原生 identity、CSSOM 反映、snapshot 所有权、诊断、微任务错误与 timer batch |
| d3d11_warp_react / react_browser_diff | native React 三帧真实 GPU readback、1,142 断言、522 texture query 退休与实际 browser 状态/300 几何字段；差分需 Python |
| flex_layout_report / flex_browser_diff | 33 场景报告与独立浏览器的 452 字段比较；差分需要 Python |
| d3d11_warp_color_lifetime | 实际 D3D11 软件执行、121 项颜色/alpha、状态、取消/提交/query/退休断言 |
| d3d11_warp_sdf | 实际 R8 distance atlas、三种倍率、导数 AA、蓝色描边、独立直边 coverage oracle、direct/RTT 与退休 |
| d3d11_browser_color_contract | 固定 browser PNG/DOM/hash 与 native 颜色合同比较；记录有意的 blend-space 差异，需要 Python |

resource_lifetime 测试模拟生命周期；独立 d3d11_warp_color_lifetime 实际执行 GPU API、shader、query 与 readback。`weft_view_record_tick` 仍仅记录 host 时间，没有运行脚本或渲染。全部测试通过也不能宣称完整页面、SVG、移动平台或性能预算通过。完整退出条件见 [实现计划](implementation-plan.md)。

自动布局追加 `layout/auto_page`（第 8 步并入 `layout/document`）、`weft_auto_page_tests`（现为 `weft_auto_layout_tests`）、fixture integrity 与两类 browser diff。`--cheetah-auto-hardware` / `--cheetah-auto-warp` 复用固定原版 bundle，内部使用自动尺寸。浏览器准备、离线测试和 sealed evidence 见 [自动布局合同](automatic-layout-probe.md)。

可见 flow 窗口的 `--cheetah-flow-window-hardware/warp` 默认使用 4K 离屏目标、最大化窗口和 FPS HUD；新 hidden 4K 场景为 `--cheetah-flow-window-test-4k-hardware/warp`。旧 hidden 640×420 入口不变；实际 client、logical/raster 区别与 FPS 定义见 [flow 窗口](flow-input-window-probe.md#4k-与-fps2026-10-08)。

4K 样例现启用固定资产的有界缓存。性能测量单独运行，不注册机器相关 CTest 时间阈值；采样时避免同时构建/运行其他测试。命令实际输出 `profile.json`、逐帧 `samples.csv` 和生命周期 `report.json`，包含 8 帧 warmup、120 帧固定 wheel、CPU 各阶段/P50/P95/P99/max、UI command-list GPU timestamp、冷首帧与进程内存。完整条件、历史基线和限制见 [滚动优化](flow-input-window-probe.md#4k-滚动优化2026-10-09)。

~~~powershell
cmake --build --preset windows-release --parallel 10
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-perf-hardware artifacts/local/flow-scroll-perf/optimized
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cache-hardware artifacts/local/flow-scroll-perf/render-cache-hardware
ctest --test-dir build/windows-msvc -C Release -R 'd3d11_warp_(render_cache|cheetah_flow_window_4k)' --output-on-failure
~~~

4K 窗口现默认最多两帧在途，两个独立 event query/constant buffer、immutable HUD lease 与真实完成退休；同步参考保留。`--pipeline-hardware/warp` 是独立生命周期/像素探针。`--cheetah-flow-window-perf-sync-hardware` 与既有 `--cheetah-flow-window-perf-hardware` 用同一 exe 对比同步/两帧；v2 profile 分别标明 owner tick 截止到 completion/submission，并另记录含最后 GPU drain 的 `measurement_wall_ms` 和全完成吞吐，不能拿两种 tick 指标直接当显示延迟。完整条件见 [两帧窗口](flow-input-window-probe.md#两帧流水线2026-10-09)。

~~~powershell
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --pipeline-hardware artifacts/local/two-frame/hardware
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-perf-sync-hardware artifacts/local/two-frame/sequential
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-perf-hardware artifacts/local/two-frame/pipelined
ctest --test-dir build/windows-gc-stress -C Debug -R 'flow_input|flow_window|mouse_input|script_dom_lifecycle|page_react|render_cache|font_fallback|two_frame' --output-on-failure
~~~


可见 4K flow 入口默认带每帧/30 Hz 连续运动展示，pause 和 speed 均经原生 DOM 按钮。新 `--cheetah-flow-window-test-4k-motion-hardware/warp` 与 CTest `d3d11_warp_cheetah_flow_window_motion` 验证几何/像素/时钟/输入/生命周期，静态 perf 与 golden 入口不变。它使用局部宿主 JS 时钟，没有添加全局 rAF 或 CSS 动画。运行和合成时钟采样定义见 [连续运动](flow-input-window-probe.md#连续运动展示2026-10-09)。

gallery/rAF 阶段新增 `weft_animation_frame_tests`、`weft_gallery_probe` 与 `gallery_*` 完整性/诊断审计。正常构建只用固定 bundle；`node tools/build_gallery.mjs` 可选离线重建十个分组和完整页面，不读原游戏或 install。gallery native observation 不加载完整 CSS/输入/GPU，结果不能当作渲染通过；大 bundle 的 72 例诊断不注册到 GC instrumentation。命令、依赖来源/工具链平台限制与浏览器参照见 [gallery/rAF](gallery-foundation.md)。

## 原样 Gallery Canvas

Windows 可运行 `build/windows-msvc/bin/Release/weft_d3d11_probe.exe --gallery-canvas-window-hardware artifacts/local/gallery-canvas/interactive`，Tab 切换、移动/点击交互、Esc 退出。离屏自动验证为 `--gallery-canvas-warp` / `--gallery-canvas-hardware`，隐藏 4K/HWND 为 `--gallery-canvas-window-test-warp` / `--gallery-canvas-window-test-hardware`。真实浏览器参照是 `node tools/browser_canvas_reference.mjs artifacts/local/gallery-canvas/reference`；正常 CTest 使用固定 PNG，不启动 Chrome、不联网。条件/误差与实际命令见 [Canvas 阶段](gallery-canvas.md)。

## 外部纹理 lease 探针

新增 `d3d11_warp_external_texture`，以及 `weft_d3d11_probe.exe --external-texture-hardware <output-directory>` / `--external-texture-warp`。原 renderer C ABI v1 结构不变，扩展单独安装。三个 linear-premultiplied RGBA 格式、两帧 native producer 合成、caller 提前释放/真实 query 退休和失败反例的复现与限制见 [外部纹理合同](external-texture-lease.md)。该模式输出离屏 PNG/report，不创建可见窗口。


## 4K Gallery 检查目录

`weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-css-d3/interactive` 创建带 FPS/CPU 分位数、分组跳转和滚动的 72 例目录。26 CSS + 8 D3 + 两例原样 Canvas 实时绘制，其余 36 例显示执行/像素缺口；目录不是完整原始页面。`--gallery-window-test-hardware/warp` 运行隐藏 160 帧的时钟/目录/几何/输入/缓存差分/退休测试，WARP 已加入 CTest。条件、实际限制与产物见 [Gallery 检查窗口](gallery-preview.md)。


## 固定 CSS/D3 gallery scene 验证

```powershell
cmake --build --preset windows-release --parallel 8
ctest --preset windows-release --output-on-failure
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-css-d3/interactive
& .\build\windows-msvc\bin\Release\weft_scene_gallery_tests.exe artifacts/local/gallery-css-d3/release/native
python tools/compare_scene_geometry.py artifacts/local/gallery-css-d3/release/native tests/fixtures/scene-gallery-reference
python tools/verify_scene_fixture.py
```

新增 `scene_contract`、`scene_gallery_original`、`scene_fixture_integrity`、`scene_browser_geometry` 四项。后两项需要 Python，正常构建/CTest 不联网、不启动浏览器，读取固定 102 PNG/几何 oracle。Windows 的 scene raster target 使用 SDK 自带 d2d1/dwrite/windowscodecs/ole32；只注册在 WIN32，不推断移动可用。scene 测试的字体策略在 `tests/support/scene_fonts.hpp`：本机已安装字体加 Chrome 155 在本机实际使用的 generic 映射。Scene owner core 依赖已固定的 Yoga，不向 C ABI 暴露 D2D/STL。

可选离线 utility 重建：`python tools/compile_gallery_scene_styles.py`，从已固定原 `gallery.css`/TestGallery 源生成 123 utility 和 29 原 keyframe block，校验和保存 manifest。可选真实浏览器 oracle 重建：`node tools/browser_scene_gallery_reference.mjs artifacts/local/gallery-css-d3/reference`，需本机 Chrome，使用完整原 CSS、未改 bundle、受控 clock/random，阻止外部请求；不属于普通构建，不自动覆盖固定 fixture。来源/采样/字体条件与误差阈值见 [scene gallery](scene-gallery.md)。
