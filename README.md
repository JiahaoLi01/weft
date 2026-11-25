<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/weft-logo-dark.gif">
    <img src="docs/images/weft-logo-light.gif" width="380" alt="Weft">
  </picture>
</p>

<h3 align="center">面向游戏引擎的嵌入式原生 Web 标准 UI 运行时</h3>

<p align="center">
  <strong>C++20 核心</strong> · <strong>纯 C ABI</strong> · <strong>D3D11 / D3D12 / Vulkan / Metal</strong> · <strong>Windows / macOS / iOS / Android</strong>
</p>

<p align="center">
  <sub>上方动态 Logo 由 Weft 运行时自身渲染（<a href="examples/logo">examples/logo</a>，纯 SVG 描边与 CSS 关键帧，与 Chrome 逐帧校验）。</sub>
</p>

---

**Weft** 是一套专为实时游戏引擎设计的轻量级嵌入式 UI 运行时。它让团队使用标准的 HTML、CSS、DOM 与 JavaScript 生态构建游戏界面，同时将**线程调度、资源加载、GPU 设备与命令列表控制权完全交还给宿主引擎**。

相比传统的内置浏览器进程（CEF / WebView）或封闭的二进制资产工作流，Weft 以纯 **C ABI** 嵌入引擎进程内部：不启动额外子进程，不创建独立 GPU 设备，不经过 CPU 像素回读与拷贝；其纯文本、声明式的界面描述也天然契合**现代前端工具链与 AI 辅助界面开发工作流**。

## 核心优势

- **AI-First 声明式界面工作流**：界面逻辑与样式以标准 Web 子集编写，具备清晰的文本 Diff、组件化复用与极高的 AI 生成/迭代精度；配合内建的 Chrome DevTools 协议与热重载工具，改写界面无需重新编译或打包游戏。
- **宿主掌控的零拷贝 GPU 渲染**：渲染器直接向引擎自身的 GPU 设备与命令列表录制绘制指令（支持 D3D11、D3D12、Vulkan、Metal），无缝接入 Unreal Engine RDG/RHI、Unity 或自研渲染管线。
- **为游戏帧预算设计的增量管线**：样式层叠、Yoga 布局、字形塑形与显示列表均为增量更新，仅重算受变化影响的子树；在包含数千节点的复杂 HUD 场景下，单帧局部更新的 UI 与录制开销保持在亚毫秒级。
- **原版前端生态兼容**：内置 QuickJS-ng 运行时，可直接运行未修改的 React、D3（含原生 SVG 渲染）、Zustand、KaTeX 等现代前端库。
- **面向游戏场景的引擎桥接**：提供双向事件、Promise 异步调用、同步函数与视图模型；支持 **3D 世界锚点（`data-anchor`）** 由渲染线程直驱位置，彻底绕过脚本与布局重排；支持零拷贝双向 GPU 纹理互通（场景纹理导入 UI / UI 渲染导出为 3D 世界材质）。
- **完整的游戏级交互与国际化排版**：覆盖键鼠、多点触控（点击/滑动/惯性回弹）、系统输入法（IMM/TSF/macOS/iOS IME）、手柄与空间导航（Spatial Navigation）、移动端安全区（`env(safe-area-inset-*)`）；集成 HarfBuzz、FreeType 与 SheenBidi，完整支持 CJK 及阿拉伯语/希伯来语双向混排（BiDi / RTL）。
- **浏览器对照验证与双构型交付**：核心排版、渲染、命中测试与动画行为均建立与 Chrome 逐帧对照的自动化契约测试；提供 ABI 完全一致的 Dev / Release 双构型，发布版彻底剥离开发者工具代码与网络符号。

## 平台与图形后端支持

| 平台 | 图形后端 | 集成形态与状态 |
| --- | --- | --- |
| **Windows x64** | D3D11 · D3D12 · Vulkan | 完整支持（静态库 / DLL），已在实际 UE5 与 Unity 项目中集成验证 |
| **macOS arm64** | Metal | 完整支持（静态库 / `.dylib`），含 2D HUD 与 3D 游戏内 UI 原生示例 |
| **iOS arm64** | Metal | 完整支持（静态库），含触控、安全区与 CoreText 系统字体适配 |
| **Android arm64** | Vulkan | 支持 NDK (`arm64-v8a`) 交叉编译与系统字体发现 |
| **自研引擎** | 内置后端 / 自定义后端 | 通过 [`include/weft`](include/weft) 纯 C ABI 接入，参见 [`samples/engine-host`](samples/engine-host) |

## 架构概览

Weft 严格分离**逻辑更新**与**命令录制**两个阶段，不接管宿主主循环，也不创建隐式渲染线程：

```text
┌─────────────────────────── 宿主逻辑 / UI 线程 ───────────────────────────┐
│  weft_view_set_model / emit / pointer / key / touch / gamepad / anchors  │
│                                  │                                       │
│                                  ▼                                       │
│   QuickJS-ng & DOM ──▶ 增量样式层叠 ──▶ Yoga 布局 ──▶ HarfBuzz / BiDi    │
│                                  │                                       │
│                                  ▼                                       │
│                        不可变帧快照 (weft_view_frame)                     │
└──────────────────────────────────┼───────────────────────────────────────┘
                                   │  线程安全所有权移交
┌──────────────────────────────────▼───────────────────────────────────────┐
│                           宿主渲染 / RHI 线程                             │
│   weft_renderer_prepare_scene ──▶ weft_renderer_encode (写入宿主命令列表) │
│                                  │                                       │
│            D3D11 / D3D12 / Vulkan / Metal / 自定义后端                    │
│                                  │                                       │
│          GPU Fence 完成后回调 weft_renderer_notify_completed 回收资源     │
└──────────────────────────────────────────────────────────────────────────┘
```

完整线程契约、增量更新机制与 GPU 资源生命周期见 [系统架构与渲染管线](docs/architecture.md)。

## 快速接入

Weft 的公开接口全部收敛于 [`include/weft/`](include/weft) 下的纯 C 头文件，可直接对接 C / C++ 或其他支持 C FFI 的引擎运行时。

### 1. 宿主侧（C / C++）

```c
#include "weft/weft.h"
#include "weft/render_backend.h"

// 创建运行时与 View（可在宿主指定的逻辑/UI 线程执行）
weft_runtime* runtime = NULL;
weft_runtime_create(NULL, &runtime); // 默认启用系统字体与默认 View 上限

weft_view_handle view = {0};
const weft_view_desc view_desc = {sizeof(view_desc), 0, 1920.0f, 1080.0f, 1.0f, 0};
weft_view_create(runtime, &view_desc, &view);
weft_view_load(runtime, view, &page_desc);

// 每帧驱动（逻辑线程）：更新状态、推进动画并消费页面发出的消息
weft_view_set_model(runtime, view, "player", 6, WEFT_VALUE_JSON, "{\"hp\":73}", 9);
weft_view_advance(runtime, view, host_time_seconds);
weft_view_drain_messages(runtime, view, on_page_message, host_ctx);

// 渲染线程：获取不可变帧快照并录制到宿主 GPU 命令列表
weft_view_frame* frame = NULL;
weft_view_acquire_frame(runtime, view, &frame);
// -> weft_renderer_prepare_scene(...) -> weft_renderer_encode(...) -> weft_renderer_notify_submitted(...)
weft_view_frame_release(frame);
```

### 2. 页面侧（JavaScript）

```js
// 订阅引擎视图模型
const player = weft.engine.model('player');
player.subscribe(state => {
  hpBar.style.width = `${state.hp}%`;
});

// 向引擎发送事件或发起异步请求
attackBtn.addEventListener('click', () => weft.engine.send('combat.attack'));
const items = await weft.engine.call('inventory.load', { page: 1 });
renderInventory(items);
```

更多关于 3D 世界锚点、双向 GPU 纹理互通与输入路由的完整说明，请阅读 [引擎集成与宿主通信指南](docs/engine-communication.md) 与 [渲染后端集成指南](docs/render-backend-api.md)。

## 开发者工具与热重载

Weft 提供 ABI 完全一致的 **Release**（默认，`WEFT_DEVTOOLS=OFF`）与 **Dev**（`WEFT_DEVTOOLS=ON`）双构型。开发期间替换动态库或开启构建选项，即可直接通过 Chrome DevTools 审查运行中的游戏界面：

```sh
# 1. 启动游戏时监听调试端口（无需修改宿主代码）
WEFT_DEVTOOLS_LISTEN=9230 ./your_game

# 2. 控制台输出 devtools:// 地址，直接粘贴至 Chrome 审查 DOM / 样式 / Console

# 3. 监听本地页面目录，保存即热重载运行中的游戏界面
node tools/devtools_watch.mjs 127.0.0.1:9230 path/to/ui/page

# 4. 自动化检查不兼容 CSS 或与 Chrome 布局逐项比对（可接入 CI）
node tools/devtools_report.mjs 127.0.0.1:9230
node tools/devtools_compare.mjs 127.0.0.1:9230
```

发布版在编译期彻底排除 `src/devtools`，不引入网络协议栈或调试符号，详见 [开发者工具与双构型发布指南](docs/devtools.md)。

## 快速构建

所有第三方依赖均已固定版本与校验值并内置于仓库（`third_party/`），**构建过程无需联网**。

### Windows x64（Visual Studio 2022 + CMake 3.25+）

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release --parallel
ctest --preset windows-release
```

> 安装 Vulkan SDK（1.4.363.0）后将自动一并构建 Vulkan 后端及着色器交叉编译管线。macOS、iOS、Android 与共享库（`WEFT_BUILD_SHARED`）构建指南详见 [构建、平台与测试指南](docs/building.md)。

## 仓库结构

| 目录 | 说明 |
| --- | --- |
| `include/weft/` | 公开 C ABI 头文件：核心运行时（`weft.h`）、通用渲染后端及 D3D11/D3D12/Vulkan/Metal 适配、外部导入与导出纹理扩展 |
| `src/` | 运行时实现：DOM、脚本桥接、样式层叠、Yoga 布局、文字与 BiDi 排版、场景构建、输入系统、图形后端与可选开发者工具 |
| `samples/` | 引擎宿主集成示例：Windows 多后端宿主、macOS / iOS Metal 宿主、3D 游戏内 UI 示例与自定义着色器效果示例 |
| `examples/` | 界面示例集：动态 SVG Logo、React + Vite 应用、BiDi 混排、动画、图片、触控、KaTeX 与自定义视觉特效 |
| `tests/` | 自动化契约测试与 Chrome 逐像素/几何/命中对照测试套件 |
| `tools/` | DevTools 热重载监视器、CSS 兼容性报告工具、Chrome 布局比对脚本与参考夹具生成工具 |
| `third_party/` | 离线固定版本的第三方依赖与许可证清单 |
| `docs/` | 产品架构与集成参考文档 |

## 技术文档

| 文档 | 内容概览 |
| --- | --- |
| [系统架构与渲染管线](docs/architecture.md) | 模块分层、线程所有权、单帧执行顺序、增量更新、帧快照与 GPU 生命周期 |
| [引擎集成与宿主通信指南](docs/engine-communication.md) | C ABI 生命周期、事件/调用/视图模型、3D 世界锚点、外部纹理、键鼠/触控/IME/手柄与空间导航 |
| [渲染后端集成指南](docs/render-backend-api.md) | D3D11 / D3D12 / Vulkan / Metal 后端接入、命令列表录制、Fence 同步与自定义后端开发 |
| [3D 游戏内 UI（导出纹理）](docs/ingame-ui.md) | 零拷贝导出纹理环、线性预乘与自动 Mipmap、3D 表面射线输入与性能要点 |
| [Web 标准兼容参考](docs/compatibility.md) | 支持的 HTML/DOM、CSS 选择器与布局、视觉特效、动画、图片格式、BiDi/CJK 排版与前端库支持 |
| [自定义 UI 着色器效果](docs/effects.md) | HLSL 像素效果编写规范、背景读取（折射/热浪/毛玻璃）、离线编译与运行时注册 |
| [开发者工具与热重载](docs/devtools.md) | Dev / Release 双构型部署、Chrome DevTools 接入、热重载与 CI 自动化诊断 |
| [构建、平台与测试指南](docs/building.md) | Windows / macOS / iOS / Android 构建配置、CMake 选项、离线依赖与自动化测试 |

## 核心第三方依赖

Weft 内置并锁定了以下成熟的开源基础组件（完整版本与许可证信息见 `third_party/manifest.json`）：

- **脚本引擎**：[QuickJS-ng](https://github.com/quickjs-ng/quickjs)
- **布局引擎**：[Yoga](https://yogalayout.dev/)
- **排版与字形**：[HarfBuzz](https://harfbuzz.github.io/) · [FreeType](https://freetype.org/) · [SheenBidi](https://github.com/Tehreer/SheenBidi) · [libunibreak](https://github.com/adah1972/libunibreak)
- **图像解码**：[libjpeg-turbo](https://libjpeg-turbo.org/) · [libwebp](https://chromium.googlesource.com/webm/libwebp) · [lodepng](https://lodev.org/lodepng/)
