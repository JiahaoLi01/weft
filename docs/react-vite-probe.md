# React / Vite 原生页面探针

2026-10-07，固定 React 19.3.0、react-dom 19.3.0、scheduler 0.28.0 和 Vite 8.3.3，真实 JSX 应用已通过自己的 QuickJS、C++ DOM、行内样式、Yoga、HarfBuzz/FreeType 和 D3D11 输出像素。Windows 硬件与 WARP 的初始、计数更新、keyed 列表重排共三帧通过。没有修改 React runtime 源码，没有将浏览器截图输入 native renderer。

这是明确受限的离屏页面实验，M0 仍未全部退出。随后已增加 [原生 HWND/swapchain 主鼠标窗口](native-window-probe.md)；下文三帧离屏 runner 仍使用直接事件派发，窗口证据单独记录。一般 HTML/CSS、移动和任意 React 应用兼容仍未交付。

后续 page adapter 已共享 [公共样式求值层](style-cascade-probe.md)，新增 stylesheet 入口与 rgba；下文固定三帧 workload 保留原来的 inline/Flex 范围和历史报告。

## 同一份应用的两个构建

[main.jsx](../examples/react-vite/src/main.jsx) 使用实际 `createRoot`、`useState`、JSX 组件、事件与 key，包含 25 个元素。640 × 420 HUD 有计数按钮、三项彩色条和重排按钮。文本来自固定 Roboto Regular；浏览器也加载相同字体字节。

- Browser：Vite 正常 HTML/模块输出，供独立浏览器参考和开发预览。
- Native：同一入口由 Vite library mode 输出 production ES2020 IIFE，自己 VM 直接执行 JS；宿主创建 mount 节点，不经过 HTML parser。

Vite 是构建工具。native 没有 ESM loader、dev client、HMR 或开发服务器依赖。标准 React 应用调用仍由原版 runtime 执行；当前只验证这一份固定 workload。[Vite library mode](https://vite.dev/guide/build)、[React createRoot](https://react.dev/reference/react-dom/client/createRoot)

## 实际数据链路

1. [平台 bootstrap](../probes/web_host_bootstrap.js) 补充受限的 CSSStyleDeclaration、宿主时间和任务接口；节点、属性、树操作和事件传播仍由 C++ native DOM 提供。
2. React 挂载后，owner 执行有限批次的任务与微任务；实际 native `Event` 冒泡触发 React 事件系统。计数变为 1，反转列表时 Ember 保持原生节点身份。
3. [snapshot_subtree](../include/caseelephant/script_probe.hpp) 复制自己的 native 节点树；[page adapter](../src/layout/page_probe.cpp) 读取行内声明、有限继承，生成已有 Flex adapter 的输入。
4. Yoga 输出唯一盒几何，HarfBuzz 生成逻辑 glyph/advance，FreeType 产生灰度 coverage；背景矩形与文字组成值类型 `ProbePage`。
5. [GPU runner](../probes/d3d11_probe.cpp) 准备 R8 glyph 纹理，实际宿主执行 command list；query 确认完成后退休资源，staging readback 保存 PNG。

三份页面和塑形结果在 React unmount、GC、VM reset 之后才送入 GPU。renderer 不读取 Node 指针或 JSValue；逻辑 glyph 不携带 atlas UV。这也验证了快照可以独立于 VM 存活。实验每帧最多 512 个不同 glyph、4 MiB coverage，用独立 glyph texture 验证链路；尚非正式 atlas/cache 或稳定 FramePacket SDK。

## 样式和调度边界

| 项目 | 本次实际范围 |
| --- | --- |
| 元素 | HTML namespace 的 div/button，显式 display:flex 或 none；缺少 display 或 block 报错 |
| CSS | 有界平坦行内声明、已有 Flex profile、opaque #rrggbb 背景与文字、有限文字属性继承 |
| 文本 | 固定 Roboto regular、左对齐、nowrap 单行、ASCII 空白折叠；混合 inline、缺字、文字越界报错 |
| 绘制 | DOM 顺序的背景/文字；非零边框、overflow clip、opacity、圆角、transform 和未支持声明报错 |
| CSSOM | style 属性与 native style attribute 相互反映；camelCase、cssText、set/get/removeProperty；无 JS computed style/stylesheet loading API；后续共享 C++ cascade 独立验收 |
| 任务 | owner 单调时间、set/clearTimeout、Promise 微任务；新 timer 留到下一批，队列和 drain 有界，异常送到宿主 |
| 未验证接口 | native MessageChannel/rAF、DOM geometry read、focus/selection、表单、窗口指针命中、触摸、IME、SVG paint |

`document.activeElement` 在这个无焦点 profile 返回 body；它不能证明焦点引擎实现。bootstrap 只供实验宿主，不代表完整 Web 平台。独立 Flex probe 原来的 33 场景继续通过；这次补充 nowrap column 文本在自动 cross width 下的 intrinsic height，换行文本仍需确定的 cross constraint，反例保留在测试中。

默认 VM 保留 64 MiB、2 秒/次执行、512 KiB 栈限制。React 探针使用 1 MiB VM 栈，Windows page/input/GPU 三个实验 exe 显式保留 8 MiB 宿主线程栈，给 Debug/native bridge 留余量。仅 QuickJS 强制 GC 构建的 React workload 使用 30 秒执行预算和 180 秒测试超时；其他故障测试仍保留 2 秒。最初强制 GC 触发了 2 秒 deadline，故障日志也保留，放宽仅用于每次分配前完整回收的 instrumentation。此预算不是产品帧率或性能验收。

## 已有独立预期与结果

浏览器由实际 Codex in-app Chromium 执行同一 JSX 的正常 Vite build；[reference.js](../examples/react-vite/src/reference.js) 只在 `?probe=1` 下依次捕获初始、计数和重排状态。先等待固定字体加载，测量真实 getBoundingClientRect，再保存 DOM 中的 JSON 与截图。没有根据 native 几何生成浏览器 golden。

[固定 browser 参考](../tests/fixtures/react_browser_reference.json) 保存输入 hash、原始捕获 hash、三份状态与几何。每帧 25 个 box，共 **300 项几何字段**和 React state 完全匹配检查通过；最大几何差异 **0.075882 CSS px**，允许 **0.1 CSS px**。采集浏览器实际 DPR 为 **1.005**，这次容差包含其分数量化；不能混用旧独立 Flex golden 的 0.025 容差。原生三帧 direct/RTT 全图最大差异硬件 1、WARP 0 色阶。

每个配置/驱动共 **1,142 项断言**，包括实际文字像素、计数区域之外不变、重排区域改变、状态恢复、真实 query 与 **522 个 glyph texture** 的退休；调试层 warning/error 为零。这个三帧 runner 通过自己的 DOM 直接派发验证计数和 keyed identity；随后可点击的 native 窗口使用另行记录的命中与消息路径。浏览器逐像素一致保持未验证：既有 linear-sRGB 合成合同仍适用，本次正文选择灰度 coverage，SDF 页面材质尚未集成。

`page_react_contract` 另做八轮真实 React 挂载、完整 GC、计数更新、unmount/reset，验证节点身份、snapshot 所有权、CSSOM 双向反映、unsupported CSS、微任务错误和 timer batch。所有配置和实际测试时间以 [status](status.md) 为准，不能从这些通过项推断性能预算或移动支持。

## 复现

普通 CMake/CTest 不需要 Node 或网络，直接使用已保存的 native bundle 和独立 browser golden：

```powershell
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
.\build\windows-msvc\bin\Debug\ce_d3d11_probe.exe --react-hardware artifacts/local/react-debug/hardware
python tools/compare_react_reports.py tests/fixtures/react_browser_reference.json artifacts/local/react-debug/hardware/report.json --output artifacts/local/react-debug/hardware/browser-comparison.json
```

WARP 使用 `--react-warp`；Release 更换对应 exe 路径。输出 `frame-0.png` 到 `frame-2.png`、三份 RTT PNG、native-dom.json 和 report.json。CTest 自动产物在 `build/windows-msvc/artifacts/react-Debug/warp` 与 `react-Release/warp`。

修改 JSX 后在 example 目录构建。当前实际工具链为 Node 24.19.0、npm 10.8.2；安装需网络或已有 npm cache，构建使用 package-lock 固定版本：

```powershell
Set-Location examples/react-vite
npm ci --ignore-scripts --no-audit --no-fund
npm run build
npm run build:native
node ../../tools/freeze_react_bundle.mjs
node node_modules/vite/bin/vite.js preview --host 127.0.0.1 --port 4173 --strictPort
```

普通 `http://127.0.0.1:4173/` 可在浏览器点击；`?probe=1` 产生独立参考。也可执行 npm run dev。上面 preview 指令在当前终端前台运行，退出用 Ctrl+C；本次展示服务器另由隐藏进程启动并绑定 loopback。

[freeze 工具](../tools/freeze_react_bundle.mjs) 核对安装的 React/react-dom/scheduler 全部原始字节与已审阅依赖 lock，再复制 Vite IIFE、原始 MIT notices 并记录输入/输出 hash。native bundle 当前为 **582,902 字节**，SHA256 `fac28baee3fe7076f281e699b215ffc93e85a8f9332b1ef79a49960afd5b6c63`；重复构建一致。修改应用后需用真实浏览器重新捕获参考，不可直接更新 golden hash 绕过对照。离线 `verify_react_fixture.py` 会拒绝过期 bundle 输入/输出，`compare_react_reports.py` 会拒绝过期 browser 输入。

新增 React runtime 的 87 个官方 npm 文件保存在 third_party/react-runtime；逐文件来源、tarball SHA256、integrity、许可和 gitHead 随 lock 保存。React 源码无兼容性补丁。Vite 是单独 package-lock 管理的构建依赖，不进入 C++ 运行时。

本轮产物：`artifacts/local/react-reference` 保存真实 browser report/DOM/PNG；`react-debug`、`react-release` 保存硬件和 WARP 原生结果、比较及测试日志；`react-gc-stress` 保存强制回收证据。Android/iOS、中文回退、一般 HTML/CSS、native 输入和 SDF 页面的状态仍为未验证。
