<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/weft-logo-dark.gif">
    <img src="docs/images/weft-logo-light.gif" width="380" alt="Weft">
  </picture>
</p>

<h3 align="center">写 Web，跑在引擎里。</h3>

<p align="center">
  <strong>C++20</strong> · <strong>纯 C ABI</strong> · <strong>D3D11 / D3D12 / Vulkan / Metal</strong> · <strong>Windows / macOS / iOS / Android</strong>
</p>

<p align="center">
  <sub>上方动态 Logo 由 Weft 自身渲染，纯 SVG 描边与 CSS 关键帧（<a href="examples/logo">examples/logo</a>）。</sub>
</p>

---

**Weft** 是为实时游戏引擎打造的嵌入式 Web UI 运行时。用 HTML、CSS 与 JavaScript 构建游戏界面：没有独立的浏览器进程，没有像素回读，也不接管你的主循环。每一笔绘制，都通过纯 C ABI 直接写进引擎自己的 GPU 命令列表。

<p align="center">
  <img src="docs/images/showcase/dark-fantasy-rpg.png" width="99%" alt="CRPG UI">
</p>
<p align="center">
  <img src="docs/images/showcase/pop-jrpg-battle.png" width="49%" alt="JRPG 3D In-Game UI">
  <img src="docs/images/showcase/4x-strategy.png" width="49%" alt="4X Strategy UI">
</p>
<p align="center">
  <img src="docs/images/showcase/tactical-fps.png" width="49%" alt="Tactical FPS HUD">
  <img src="docs/images/showcase/cozy-workshop.png" width="49%" alt="Cozy Workshop UI">
</p>

## 核心特性

### 直接画进你的 GPU 管线。

原生录制 D3D11、D3D12、Vulkan 与 Metal 命令列表，接入 Unreal Engine RDG/RHI、Unity 或自研渲染管线。毛玻璃、矢量裁切、渐变遮罩、混合模式与技能冷却扫描，全部零拷贝直出。

<p align="center">
  <img src="docs/images/showcase/feature-effects.png" width="100%" alt="GPU CSS Effects">
</p>

### 原版前端库，拿来就跑。

内置 QuickJS-ng，不改一行源码即可运行 React、D3、Zustand 与 KaTeX。改完界面保存即热重载，不用重新编译游戏。

<p align="center">
  <img src="docs/images/showcase/feature-katex.png" width="100%" alt="KaTeX & SVG">
</p>

### 注音排版，字字落位。

原生支持 CSS Ruby。日文剧情的逐字振假名，中文典籍的生僻字拼音，一行 `<ruby>` 精准对齐。

<p align="center">
  <img src="docs/images/showcase/feature-ruby.png" width="100%" alt="CSS Ruby">
</p>

### 从左到右，从右到左，一样自然。

内置 HarfBuzz、FreeType 与 SheenBidi。阿拉伯语与希伯来语的双向混排、RTL 布局镜像、泰文与天城文连写塑形，全部原生渲染。

<p align="center">
  <img src="docs/images/showcase/feature-bidi.png" width="100%" alt="Arabic BiDi & Complex Scripts">
</p>

---

- **千节点 HUD，亚毫秒更新。** 样式层叠、Yoga 布局、字形塑形与显示列表全程增量计算，只重算发生变化的那一小块。
- **3D 与 UI，零拷贝互通。** 渲染线程直驱 3D 世界锚点（`data-anchor`），绕过脚本与重排；GPU 纹理双向共享，3D 舞台进界面，界面进 3D 场景。
- **全平台输入，原生响应。** 键鼠、多点触控、惯性滚动、系统输入法（IMM / TSF / CoreText IME）、手柄空间导航与移动端安全区（`env(safe-area-inset-*)`）。
- **开发连 Chrome，发布干干净净。** 开发期直连 Chrome DevTools 审查 DOM 与热更新，核心行为与 Chrome 逐帧校验；切到 Release 构型，调试代码与网络符号在编译期彻底剥离。

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
