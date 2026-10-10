# 原样 Gallery Canvas 阶段

2026-10-09。两例原始 `Canvas2DParticles` / `Canvas2DFireworks` 已运行在自己的 QuickJS、原生 DOM/context、positioned 布局和 D3D11 中；原始函数与第三方包未修改。只读固定的 CRPG gallery 仍为 72 例，此次仅增加这两例的执行、限定视觉与交互验收，其余 70 例未增加像素验收。该 Canvas 阶段未实现 WebGL2/ANGLE、公共外部纹理、宿主 viewport 和移动；后续 external lease 首版已验证，见 [新合同](external-texture-lease.md)。

## 运行

在 `F:\Projects\CaseElephant` 中：

```powershell
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-canvas-window-hardware artifacts/local/gallery-canvas/interactive
```

默认最大化，4K 合成目标、最多两帧在途，右上角与标题显示实际 Present FPS。移动鼠标产生金色粒子，Tab 切换烟花，点击产生烟花，Esc 退出。原案例选择的 **800×600 backing 保持不变**；4K 是合成目标，放大不会增加原 Canvas 的细节。Tab/Esc 属于样例宿主快捷键，不代表 DOM keyboard/focus/IME 支持。最小化停止发出绘制机会，恢复不补发历史帧。没有提供此 Canvas 软件路径的性能基准或 FPS 保证。

离屏自动验证为 `--gallery-canvas-warp` / `--gallery-canvas-hardware`，隐藏窗口验证为 `--gallery-canvas-window-test-warp` / `--gallery-canvas-window-test-hardware`，最后一个参数均是输出目录。正常 CTest 读取固定 Chrome PNG，不启动浏览器、不联网、不读取原游戏目录。

## 实际实现与边界

`weft::canvas` 定义 owner raster factory、绘制值、尺寸/颜色/路径状态与完整不可变像素快照；不暴露 COM、D3D 或 Node/JSValue。Windows adapter 使用 Direct2D **SOFTWARE** 与 WIC PBGRA；这是首先可验证的全量路径，尚非 GPU 共享纹理路径。[Microsoft WIC render target](https://learn.microsoft.com/en-us/windows/win32/api/d2d1/nf-d2d1-id2d1factory-createwicbitmaprendertarget%28iwicbitmap_constd2d1_render_target_properties__id2d1rendertarget%29) 和 [格式/alpha 合同](https://learn.microsoft.com/en-us/windows/win32/direct2d/supported-pixel-formats-and-alpha-modes) 是 adapter 的依据。

已实现可选 `getContext('2d')`、context 身份与 `canvas` 回指、alpha/sRGB；width/height 默认 300×150，属性/attribute 同值写入也重置 bitmap、绘制状态与路径，旧 context 身份保持。CSS 尺寸独立于 backing，positioned profile 支持显式 block 的固有尺寸/比例与同步 geometry values。[HTML Canvas 标准](https://html.spec.whatwg.org/multipage/canvas.html) 是支持语义的依据。通用 positioned hit-test 与一般 inline replaced formatting 尚未开放。

绘制子集是 `fillStyle`、`globalAlpha`、`fillRect`、整数 `clearRect`、`beginPath`、一个完整圆形的 `arc`/`fill`，source-over 保存在 sRGB bitmap 中。颜色有 148 named colors、3/4/6/8 位 hex 与 legacy comma RGB/RGBA/HSL/HSLA；named color 常量来源于固定 d3-color 3.1.0 的 ISC 源，源 SHA 与许可路径记录在生成的 include 中。invalid color/globalAlpha 保持旧值；有效但未支持的颜色语法、非完整/多段路径、非整数 clear 明确诊断。负半径错误 name 为 IndexSizeError，但尚无完整 DOMException 类。

canvas 的 `onclick`/`onmousemove` 是原生 listener，保持替换次序、this 与 return-false 取消语义，未扩大其他元素的事件属性。原生 receiver 错误为 TypeError。Node/context 双向引用均由 VM tracing 标记；detached canvas 可由 JS context 保活，循环可回收。尺寸/容量拒绝发生在 mutation 前，raster backend 失败使 View fault，reset 回收 backing，已发布旧帧仍有效。

单 View 最多 8 contexts、单轴 2048、32 MiB 逻辑 backing（含 resize 重叠），每次 snapshot 前最多 65,536 commands；额外 CPU snapshot leases 不计入此 backing 预算，因此它不是 RSS 上限。renderer 只接收完整 sRGB straight RGBA8 像素资产，WIC encoded premultiplied 数据先解除预乘，再经原 renderer 的 sRGB decode/linear-premultiply。渲染线程不解析 Canvas 身份，纹理按实际 event-query completion 退休，包不依赖上一未提交帧。

stroke、transform、save/restore、渐变、图片/文字绘制、JS ImageData/getImageData、partial/multi path、offscreen/worker、context loss、现代空格颜色语法/currentColor/其他色彩空间均未实现。公共样式仅新增单个已支持纯色的 `background` 到 `background-color` 的展开，保留 important/var/层叠，图片/图层/位置组合拒绝。原 gallery 的完整 Tailwind 页面仍未验收，这两例采用明确 viewport adapter。

## 验证与误差

真实 Chrome 155.0.8059.39，DPR 1，800×600，外部网络阻止。固定 seed `0xce202610`，每例原 useEffect 直接执行一次 loop，随后 64 次 host rAF；第 2/34 帧移动或点击，第 32–47 帧 CSS 显示改为 400×300，再恢复。第 11/39/63 帧保存真实 Canvas bitmap 与页面截图。浏览器的 host rAF 队列用于确定帧数，Canvas 绘制由 Chrome 完成；它不作为浏览器 rAF 语义验证。固定 PNG/版本/输入 SHA 在 `tests/fixtures/canvas-gallery-reference/`，复现为：

```powershell
node tools/browser_canvas_reference.mjs artifacts/local/gallery-canvas/reference
python tools/verify_canvas_fixture.py
```

两例分别绘制 6,500 / 5,006 次 fill，产生移动/点击与自动爆炸；每例 65 个实际完成提交（含 VM reset 后重放旧包）、65 张退休纹理、峰值两帧。每个硬件/WARP run 共 20,460 个独立 GPU/CPU 合成抽样，最大 **1 色阶**；debug layer 零 warning/error。隐藏 4K HWND 验证包括真实队列点击、切换案例、HUD 像素与关闭 drain，仅自动消息，实体设备仍需人工验收。

Chrome 与 D2D **不逐像素一致**。六个 bitmap 的分量最大差异 51–76，亮像素（任意 RGB >40）的 RGBA 平均差异 2.16–5.08，亮像素交并比 0.9208–0.9843；粒子的暗背景红通道全图平均差异最高 7.9845。圆边 AA 与低透明度反复绘制的 8-bit 舍入有显著差异。这里只验证有界形状/颜色回归：最大分量 ≤85、全图均差分别 ≤[9,.4,.4,.03]、亮像素均差 ≤6、交并比 ≥.90、亮像素至少 500。阈值写在固定 manifest；空动画反例必须拒绝。这些容差不构成精确浏览器像素兼容或任何其他案例的验收。

Canvas 合同还覆盖 256 次随机清除/绘制/尺寸重置的逐次 snapshot 与分批 snapshot 差分、200 轮 callback/context/detached 循环、8 contexts/32 MiB/resize overlap/command quota、版本不回绕、真实半透明像素、旧快照、属性 handler 和 backend 故障。强制 GC 范围不包含原 72 例大 bundle。

环境为 Windows 11 26200、AMD Ryzen 9 9950X3D、RTX 5090 / Microsoft WARP、MSVC 19.42；软件 raster 与原案例不使用字体图片，字体仅用于 HUD。测试时间不是性能指标。构建、最终 CTest、二进制/源/产物 SHA、只读 drift 与未验证项记录在 `artifacts/local/gallery-canvas/run-metadata.json`；之前 gallery-foundation seal 保留。此阶段之后的 external texture lease 已完成首版 linear 输入，见 [新合同](external-texture-lease.md)；下一步固定 ANGLE 的 WebGL2 探针。

最终 Debug 67/67（74.12 秒）、Release 67/67（26.16 秒）、强制 GC 4/4（51.32 秒）。旧 HUD probe 增加明确 host-only 提交，检查完成确认前/后的 lease；Debug 最终 Canvas/4K 范围另 5/5（9.74 秒）。全量和最终范围日志分开保留，未用失败结果替代通过结果。


2026-10-09 后续修复：可见 host timer pump 改为 classic locale / max_digits10，与 rAF 共享可往返的小数时间；单调性检查没有放松。新增 [4K 完整目录窗口](gallery-preview.md)，两例 Canvas 同页运行，其余 70 例显示实际缺口。旧单例/Tab 命令仍可用；此文的历史 67/67 和 Canvas seal 保留。
