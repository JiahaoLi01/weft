# 构建、平台支持与测试指南

Weft 基于 **C++20** 与 **CMake 3.25+** 构建。所有第三方源码依赖均已随仓库固定版本并记录校验值（`third_party/manifest.json`），**完整构建过程离线完成，不访问外部网络**。

## 1. 平台与工具链要求

| 目标平台 | 编译器与工具链 | 可用渲染后端 | 备注 |
| --- | --- | --- | --- |
| **Windows x64** | Visual Studio 2022 + Windows SDK + CMake 3.25+ | D3D11 · D3D12 · Vulkan | 安装 Vulkan SDK 1.4.363.0 后自动启用 Vulkan 后端与 SPIR-V/MSL 着色器生成 |
| **macOS arm64** | Xcode / Command Line Tools（或 Homebrew LLVM）+ CMake + Ninja | Metal | 安装 macOS 版 Vulkan SDK 1.4.363.0 后自动启用原生 MSL 编译与 Metal 示例 |
| **iOS arm64** | Xcode iOS SDK（最低部署目标 iOS 26.0）+ CMake + Ninja | Metal | 支持模拟器与真机签名打包，含 CoreText 字体与安全区适配 |
| **Android arm64** | Android NDK（`arm64-v8a`，API 29+）+ Ninja + Vulkan SDK DXC | Vulkan | 运行时动态打开 `libvulkan.so`，不硬链接系统 Vulkan 库 |

> 可选依赖 **Python 3.10+** 用于执行依赖校验、参考夹具完整性检查与像素/几何自动化回归测试。

## 2. 常用 CMake 选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `WEFT_DEVTOOLS` | `OFF` | 是否编入 Chrome DevTools 与热重载模块（`OFF` 为发布版，`ON` 为开发版） |
| `WEFT_BUILD_SHARED` | macOS `ON` / 其他 `OFF` | 构建单一共享库 `weft_shared`（`weft.dll` / `libweft.dylib`），仅导出公开 C ABI 符号 |
| `WEFT_BUILD_TESTS` | `ON` | 构建自动化契约测试与浏览器对照测试套件 |
| `WEFT_BUILD_SAMPLES` | `ON` | 构建各平台引擎宿主集成示例与自定义效果示例 |
| `WEFT_BUILD_PROBES` | `ON` | 构建微基准与性能剖析工具 |
| `WEFT_RENDER_VULKAN` | 自动检测 | 显式控制是否构建 Vulkan 后端（在纯 Metal 平台可设为 `OFF`） |

## 3. 分平台构建命令

### Windows x64

```powershell
# 配置、编译 Release 并运行测试套件
cmake --preset windows-msvc
cmake --build --preset windows-release --parallel
ctest --preset windows-release

# 可选：构建带 PDB 的性能分析版本
cmake --preset windows-profile
cmake --build --preset windows-profile --parallel
```

如需构建供引擎插件加载的独立 DLL（同时构建发布版与开发版以便按需替换）：

```powershell
# 发布版 DLL（无调试代码与网络符号）
cmake --preset windows-msvc -B build/win-shared-release -DWEFT_BUILD_SHARED=ON -DWEFT_DEVTOOLS=OFF
cmake --build build/win-shared-release --config Release --target weft_shared --parallel

# 开发版 DLL（含 Chrome DevTools 支持）
cmake --preset windows-msvc -B build/win-shared-dev -DWEFT_BUILD_SHARED=ON -DWEFT_DEVTOOLS=ON
cmake --build build/win-shared-dev --config Release --target weft_shared --parallel
```

### macOS arm64（Metal）

```sh
export VULKAN_SDK=~/VulkanSDK/1.4.363.0/macOS
cmake --preset portable-ninja -B build/macos-metal \
  -DCMAKE_BUILD_TYPE=Release \
  -DWEFT_RENDER_VULKAN=OFF
cmake --build build/macos-metal --parallel
ctest --test-dir build/macos-metal

# 运行 2D 引擎宿主示例或 3D 游戏内 UI 示例
open build/macos-metal/bin/weft_macos_demo.app
open build/macos-metal/bin/weft_macos_ingame.app
```

> 若系统 Command Line Tools 自带的旧版 `libc++` 缺少浮点 `std::from_chars`，可安装 Homebrew LLVM（`brew install cmake ninja llvm`）并通过 `-DCMAKE_C_COMPILER` / `-DCMAKE_CXX_COMPILER` 指定。

### iOS arm64（Metal）

```sh
export VULKAN_SDK=~/VulkanSDK/1.4.363.0/macOS

# iOS 模拟器示例构建
cmake -G Ninja -B build/ios-sim \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
  -DWEFT_BUILD_TESTS=OFF \
  -DWEFT_BUILD_PROBES=OFF \
  -DWEFT_RENDER_VULKAN=OFF
cmake --build build/ios-sim --target weft_ios_demo --parallel
```

真机构建将 `-DCMAKE_OSX_SYSROOT` 替换为 `iphoneos` 并通过 `-DWEFT_IOS_DEVELOPMENT_TEAM=<TEAM_ID>` 指定签名团队后使用 Xcode 生成器构建。

### Android arm64 交叉编译

设置环境变量 `ANDROID_NDK_HOME` 指向本地 Android NDK 根目录后执行：

```powershell
cmake --preset android-arm64
cmake --build --preset android-arm64 --parallel
```

构建产物输出至 `build/android-arm64`，包含全部可移植核心库、Vulkan 后端、Android 系统字体适配层（`weft::platform_android`）与 `arm64-v8a` 测试可执行文件。

## 4. 自动化测试与质量保障

Weft 的测试套件（`tests/`）覆盖以下验证维度：

- **C ABI 与生命周期契约测试**：句柄代次校验、跨线程拒绝、错误码与结构体尺寸向后兼容性、Dev / Release 双构型 ABI 互换验证。
- **增量等价性测试**：在随机 DOM 变异序列下，逐值比对增量更新管线与全量重算结果。
- **Chrome 浏览器对照测试**：以无头 Chrome 录制的参考夹具（`tests/fixtures/`）为基准，自动化校验像素输出、布局几何、命中测试、动画时刻、BiDi 排版、图片渲染、遮罩/裁剪/混合模式与 DevTools 协议响应。
- **离线依赖完整性校验**：校验 `third_party/` 源码哈希与 `third_party/manifest.json` 严格一致。

```powershell
# 运行全部自动化测试
ctest --preset windows-release --output-on-failure

# 按名称筛选单项测试（例如 Logo 像素对照或 DevTools 测试）
ctest --preset windows-release -R "logo_|devtools_" --output-on-failure
```
