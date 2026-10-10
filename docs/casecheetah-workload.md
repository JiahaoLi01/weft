# CaseCheetah 真实界面样本

2026-10-07：后续研发以 `F:\Projects\CaseCheetah\UI` 的实际组件为参考，继续使用 D3D11。图形 API function table 保留，Vulkan/原生 Metal 是后续平台后端。

后续 [公共样式阶段](style-cascade-probe.md) 已接入原始 operations.css theme/expanded rule，并保持本页 NineSlice 的 fixture/golden 和历史产物。完整游戏格式化仍未交付。

## 来源与隔离

游戏工程只读，不执行其中的安装、构建或脚本，不改它的源码、dist、node_modules 或 Ultralight 插件。第一次读取 `AGENTS.md` 后，保存了 57 个文件、6,043,160 字节的固定样本，包括全部 UI/src、package/lock/config/index 和三张共享面板 PNG。每个文件在发布样本前重读核对；来源绝对路径、UTC 时间和 SHA-256 在 [manifest](../tests/fixtures/casecheetah/manifest.json)。该目录不是 Git checkout，没有虚构 commit。用户后来修改的内容不会自动进入样本。

`tools/capture_cheetah_fixture.py` 默认拒绝覆盖已有样本。以后捕获新目录，比较两份清单，再显式更新 workload。普通构建、CTest 和校验只访问 Weft 的固定文件，不要求游戏工程仍在原位置。样本属于用户游戏源码/素材，未将其列为开源第三方依赖或赋予再分发许可。

原工程当前使用 React/react-dom 19.2.8、Zustand 5.0.15、D3 7.9.0、Vite 6.4.3，有 125 张 PNG。全界面大量使用中文、宋体/微软雅黑系统字体、CSS variables、Grid/Flex、伪元素、动画、渐变、阴影、滚动和 UE 数据桥；这些特征的库存不等于支持。

## 本轮实际链路

未改动的 [NineSlice.tsx](../tests/fixtures/casecheetah/UI/src/components/NineSlice.tsx) → Weft 已固定的 React 19.3.0/Vite 8.3.3 → QuickJS/原生 DOM → C++20 positioned formatting context → 不可变图片/灰度 Glyph 快照 → v1 backend API → D3D11 → 实际 HWND/swapchain Present/readback。

采用游戏 `shell.css` 第 5–9 行的五条规则，逐字提取并校验。源组件用被裁剪的整张图片实现九宫格，没有改写成原生“面板”指令或预先烘焙的页面截图。三个原始 PNG 为 `ink-panel-v2`、`ink-dock-left-v1`、`ink-dock-right-v1`，各 1024×1536。浮动卡片绘制九块；贴边面板有零切片，左右各两块，总共 13 个真实 image draw。

[fixture shell](../examples/cheetah-nine-slice/src/main.tsx) 是为验收写的 640×420 独立场景：三种共享底图、英文 Roboto 标签、一个缩放按钮。保留原组件 TSX，尺寸和标签来自新测试外壳；没有宣称运行原游戏完整 App、Zustand 或 Unreal bridge。原游戏依赖版本与此隔离运行时不同，均明确记录。

## 新增的格式上下文

[positioned_page_probe.hpp](../src/layout/positioned_page.hpp) 与已有 Flex profile 分开；未来文档格式上下文调度器负责组合。它支持有界 author rule、type/id/class compound、后代/子代选择器、specificity/source order、inline 优先、继承的字体/颜色/pointer-events，以及 definite width/height、absolute/relative、单值 inset、px/%、`calc(percent +/- px)`、矩形 overflow hidden 和受限隔离层叠。

绝对定位使用最近的 positioned ancestor，static 中间容器不会偷偷成为 containing block；同层负 z-index 留在 isolated 父背景上方、正文下方。非 auto z-index 限于直接 isolated positioned parent，其他层叠明确报错。边框、padding/margin 限零；普通流限具有明确高度的 block。非 absolute 的 inline/replaced 标签需要显式 block，混合 inline 和斜体未实现。后续已加入固定字体集合的中英回退与有限 span/b/ul/li，见 [中文组件](cjk-alert-probe.md)。[CSS positioned containing blocks](https://www.w3.org/TR/css-position-3/#def-cb)、[CSS visual formatting](https://www.w3.org/TR/CSS22/visuren.html)

图片 IO 在 Windows probe host：WIC 解码固定允许的三个本地 PNG 为 straight RGBA。DOM 无文件系统权限；布局只输出源字符串和值。图片矩形裁剪通过缩小 quad 和对应 UV 完成；文字在 glyph quad 阶段裁剪。该方式目前只适用于无变换、无圆角的轴对齐矩形，不把它宣称为一般 clip/mask。

输入与 GPU 分离不变：快照不持有 Node/JSValue，先释放 prepared/CPU 包，再由宿主 ExecuteCommandList/实际 query 通知退休。验收在卸载/reset VM 后使用保留下来的快照。此新格式上下文尚无通用 hit-test，交互窗口的已知位置单按钮映射只是 fixture host；报告 `mouse_geometry_hit_verified=false`。输入中更一般的 hit-test/capture 仍使用此前 Flex 合同。

CSS 文件由 host 显式传入，不是 HTML/link/style 文档加载；没有一般 CSS grammar、Tailwind 规则、伪类/伪元素、CSS variables、Grid、滚动、动画、滤镜或 opacity group。JS 侧同步 computed style/geometry 尚未接入。未知语法/属性或已使用的超出范围值有 `WEFT_POSITIONED_PROFILE` 诊断，不将它们默默替换成 Flex。

## 复现

正常 CMake/CTest 不需要 Node/Vite，不联网：

~~~powershell
cmake --preset windows-msvc
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
build/windows-msvc/bin/Debug/weft_d3d11_probe.exe --cheetah-hardware artifacts/local/cheetah-debug/hardware
build/windows-msvc/bin/Debug/weft_d3d11_probe.exe --cheetah-window-hardware artifacts/local/cheetah-interactive
~~~

`--cheetah-warp` 使用软件 D3D11。窗口使用自身缩放按钮；一般键盘、IME、中文和移动平台仍未验收。窗口 letterbox/resize 复用既有 presenter。

重建 fixture 时，在既有 `examples/react-vite` 运行 pinned `npm ci`，然后运行 `node tools/build_cheetah_fixture.mjs`。工具只写新样本的 public/dist/native/profile.css，引用 Weft 的 pinned runtime 与 vendored Roboto。`python tools/verify_cheetah_fixture.py` 离线验证全部捕获文件、原样规则、构建输入/输出哈希及 runtime notices。浏览器目录为 `examples/cheetah-nine-slice/dist/browser`，可用 `python -m http.server 4174 --bind 127.0.0.1 --directory examples/cheetah-nine-slice/dist/browser`。

## 证据与边界

原生与独立浏览器的三帧共 44 个 DOM element × 4 个字段 × 3 = 528 项几何对照，最大差异 0.123840 CSS px，声明阈值 0.2。实际浏览器 DPR 1.005，slice 小盒子的量化误差经约 1100% 的图片高度放大；native 保留理想浮点 CSS 长度。固定参考 [cheetah_browser_reference.json](../tests/fixtures/cheetah_browser_reference.json) 来自实际 DOM/getBoundingClientRect，按钮经浏览器键盘激活采集 false/true/false。不是从 native 报告生成的 golden。

三帧实际 GPU readback：small → wide → small，全图恢复完全一致。每个 driver 共 6612 个独立 CPU bilinear linear-premultiplied 样本，最大差异 1 色阶；每轮 211 个图片/字形纹理通过实际 completion query 后退休。硬件/WARP 的 D3D11 debug layer warning/error 为零。Debug/Release 完整回归以及强制 GC 的新 workload 检查记录在 `artifacts/local/cheetah-*`。

浏览器逐像素相等未验收；linear-sRGB 图片/文字边缘与 Chrome encoded-sRGB 合成有已知差别。参考适配器每帧全量生成/上传，三张图片解码 18 MiB，而正确性优先的 RGBA32F GPU 纹理约 72 MiB，均未达到产品内存/性能预算。接下来需要持久图片缓存、合适的生产纹理格式和 atlas，不能将这一探针当低成本生产 renderer。

## 下一步

后续中文阶段已固定 Noto CJK/Roboto 回退，并在作者预览 CSS 下接入原样 AlertBar/Zustand/store/双向桥，详见 [新阶段证据](cjk-alert-probe.md)。下一步组合 stylesheet 与 Flex/block/inline 文本，支持组件的实际 CSS、滚动和圆角，再复刻左右档案/视图条、Select/Tooltip、真实 D3 SVG。用固定数据驱动 UI，不运行游戏业务命令。完整 App 的未支持 CSS、字体、SVG 和外部 `inspector.imgsrc` 必须分别验收。后端优先继续 D3D11，接口保留原生 Vulkan/Metal 扩展。

后续自动布局阶段已提供独立新 profile/workload，将固定 AlertBar 内部位置与列表高度改为内容计算；本篇保留旧阶段证据，不覆盖新范围。见 [自动布局实际合同](automatic-layout-probe.md)。
