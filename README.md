<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/weft-logo-dark.gif">
    <img src="docs/images/weft-logo-light.gif" width="380" alt="weft">
  </picture>
</p>

<p align="center">用 HTML、CSS 和 JavaScript 写游戏界面，由游戏引擎掌控线程、资源和 GPU 的 UI 运行时。</p>

<p align="center"><sub>上面的 logo 是 weft 自己画的：一个只用 SVG 描边与关键帧的页面（<a href="examples/logo">examples/logo</a>），与 Chrome 逐帧对照。</sub></p>

---

weft 让游戏 UI 像网页一样写：DOM、CSS、原版 React 和 D3，用浏览器的开发方式与工具链；运行时却是为嵌入游戏设计的。它是一个 C++20 库，通过 C ABI 接进引擎：页面在宿主指定的线程上运行，画面用宿主自己的 GPU 设备和命令列表绘制，不另开设备，不经 CPU 拷贝。

> **状态：实验阶段。** Windows 上可用（D3D11、D3D12、Vulkan），已接入 Unreal Engine 5 与 Unity 6。HTML 与 CSS 只支持声明过的子集，不支持的写法会给出诊断而不是悄悄画错。Android 能交叉编译，还没在设备上运行；iOS（Metal）尚未开始。详见 [兼容范围](docs/compatibility.md) 与 [当前状态](docs/status.md)。

## 能做什么

- **原版前端库**：QuickJS 上运行未修改的 React、D3（真正画出 SVG 图表，包括坐标轴、文字、过渡与交互）、Zustand 以及常见的动画库。页面没有 HTML 解析，DOM 由脚本创建，React 和 D3 本来就是这样工作的。
- **游戏 UI 的 CSS**：层叠、类/ID/属性选择器、`:hover` 与 `:active`、CSS 变量；Flex 与绝对定位（Yoga）；圆角、边框、渐变、阴影、2D 变换、不透明度组、裁剪；关键帧与过渡；`backdrop-filter` 毛玻璃；PNG 背景图；UI 粒子与着色器效果。
- **中文与排版**：HarfBuzz 与 FreeType 塑形，字体回退，Unicode 换行；使用系统字体或宿主提供的字体文件。
- **与引擎通信**：双向事件、带 Promise 的引擎调用、同步函数、视图模型。每帧跟着场景走的元素（血条、名牌、伤害数字）用**锚点**定位：引擎写入屏幕位置，渲染线程直接移动它们，不经 DOM、不重排、不跑脚本。页面还可以显示引擎每帧在 GPU 上画的纹理（`background-image: -weft-texture(name)`）。
- **为游戏的帧预算设计**：样式、布局和绘制都是增量的，只处理变化的部分。约 1,600 个节点的 HUD 每帧改一个元素时，UI 线程约 0.1 ms，渲染 CPU 约 0.05 ms。
- **可信的画面**：各项能力都有与 Chrome 对照的测试，覆盖像素、几何、命中与动画时刻；增量结果与全量重算逐值比较。

## 接进引擎

| 宿主 | 位置 |
| --- | --- |
| Unreal Engine 5 | [weft-unreal](https://github.com/JiahaoLi01/weft-unreal)：D3D12 RHI，Slate 控件，页面与引擎纹理 |
| Unity 6 | [weft-unity](https://github.com/JiahaoLi01/weft-unity)：D3D11、D3D12、Vulkan，UPM 包与示例 |
| 自己的引擎 | 公开头文件 [`include/weft`](include/weft)；只用公开头文件的模拟引擎样例在 [`samples/engine-host`](samples/engine-host) |

宿主这一侧大致是这样（省略错误处理）：

```c
weft_runtime* runtime;
weft_runtime_create(NULL, &runtime);                       // 系统字体，默认的 View 数
weft_view_handle view;
const weft_view_desc desc = {sizeof desc, 0, 1920, 1080, 1, 0};
weft_view_create(runtime, &desc, &view);
weft_view_load(runtime, view, &page);                      // 脚本、样式表、关键帧

// 每帧，在 UI 线程上：
weft_view_set_model(runtime, view, "player", 6, WEFT_VALUE_JSON, "{\"hp\":73}", 9);
weft_view_advance(runtime, view, seconds);
weft_view_drain_messages(runtime, view, on_message, NULL);  // weft.engine.send / call

// 渲染线程上：取最新的帧，用 D3D11、D3D12 或 Vulkan 后端录进宿主的命令列表。
weft_view_frame* frame;
weft_view_acquire_frame(runtime, view, &frame);
```

页面这一侧：

```js
const player = weft.engine.model('player');
player.subscribe(v => { bar.style.width = v.hp + '%'; });
attack.addEventListener('click', () => weft.engine.send('attack'));
weft.engine.call('inventory.load', {page: 1}).then(items => render(items));
```

设计与完整的 API 说明见 [与引擎通信](docs/engine-communication.md) 和 [渲染后端 API](docs/render-backend-api.md)。

## 构建

需要 Windows、Visual Studio 2022 与 CMake 3.25 以上；装了 Vulkan SDK 1.4.363.0 时连 Vulkan 后端一起编。第三方依赖已随仓库固定版本与校验值，构建不联网。

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release --parallel
ctest --preset windows-release
```

Android（arm64，NDK）的交叉编译和更多选项见 [构建](docs/building.md)。

## 目录

| 位置 | 内容 |
| --- | --- |
| `include/weft` | 公开的 C ABI：运行时与 View（`weft.h`），渲染器与各后端 |
| `src` | 运行时：DOM、脚本、样式、布局、文字、scene、渲染后端 |
| `samples` | 宿主样例 |
| `examples` | 示例页面，包括 [logo](examples/logo) |
| `tests` | 契约测试与浏览器对照（`tests/fixtures` 是 Chrome 的参考数据） |
| `docs` | 设计与证据 |

## 文档

- [架构与运行合同](docs/architecture.md)：线程、数据、渲染、文字、资源。
- [兼容范围](docs/compatibility.md)：支持什么、不支持什么、怎么诊断。
- [与引擎通信](docs/engine-communication.md)：锚点、事件、调用、视图模型、对外 View。
- [渲染后端 API](docs/render-backend-api.md)：后端接口、宿主的提交与完成、设备世代。
- [构建](docs/building.md)：依赖、预设、测试。
- [立项与阶段摘要](docs/charter.md)、[关键取舍](docs/decisions.md)、[实现计划](docs/implementation-plan.md)、[当前状态与工作记录](docs/status.md)。
