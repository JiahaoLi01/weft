# Flow 命中与 Windows 滚轮

> 2026-10-11 重构第 8 步：命中 oracle 的 #outer/#inner 片段中心改用采集时的坐标（不再由原生几何推导），命中结果不变（3,130/3,150 精确）；格式化器改名为 `layout_document`。见 [重构记录](refactoring-plan.md#第-8-步记录2026-10-11)。

2026-10-08 的 M0 小阶段，接续 [inline/滚动/圆角](flow-layout-probe.md)。C++20 实验值接口和 D3D11 样例已接通实际 HWND 消息、最新 flow 几何与原样固定 React AlertBar。尚非完整浏览器事件、稳定 SDK 或游戏集成。

## 实际合同

`Page.hits` 保存每个逻辑元素一次的代际 identity、DOM parent index、pointer-events 和 disabled button 祖先状态；`hit_regions` 按真实绘制顺序保存矩形、视口/overflow clip 和拥有值的圆角链。inline 每个分片独立注册，祖先到子元素按绘制顺序进入列表。命中反向遍历 regions，不用 inline union rect，也不以反向 DOM 代替层叠。透明背景仍可命中；pointer-events:none 的自身跳过，显式 auto 后代仍可命中和沿事件路径传播。

圆角按同一归一化椭圆判断；overflow visible 后代不会继承自身圆角，hidden/auto/scroll 后代继承裁剪链。视口外和已滚出的内容不能命中。逻辑 parent 与 paint region 分开后，click 的共同祖先和 disabled 检查不依赖绘制顺序。旧 Flex-only router 保留原范围。

`MouseRouter(script, owner_layout_callback)` 接收每次返回新 `Page` 的 owner 回调；输入前检查 owner，鼠标释放回调后再读取几何。`wheel(WheelEvent)` 先 fresh hit，再派发到代际身份。WheelEvent 保留像素 delta、client/screen 坐标、modifiers 和宿主时间，deltaMode=0。旧 `dispatch_wheel(identity, x, y)` 仍默认零坐标/modifiers/time；新 overload 使用完整值。preventDefault、回调移除 target、旧 epoch、错误线程沿原 DOM/scroll 合同处理。

Win32 WindowProc 只入队拥有值，不访问 DOM、JS、布局或 GPU。WM_MOUSEWHEEL/HWHEEL 的 LPARAM 是有符号屏幕坐标；转换为 client 后记录当时的 letterbox viewport。之后 resize 不改变已收到消息的映射。wheel 不与 mousemove 合并；原有队列上限/取消合同保持。owner 消费每条消息后运行 React 检查点，下条输入能看到新状态。

样例读取 OS wheel lines/chars；一行 24 逻辑 px、一字符 8 px、page 分别 420/640 px。raw delta / 120 保留细分，纵向正 delta 向上、横向正 delta 向右；零设置不给默认移动。计数最大取 1,000，超过 runtime 的 100,000 px delta 配额会明确拒绝。hidden 自测仅改自己的 policy 为 3/3，未改系统设置。Ctrl/Shift 传入事件，本阶段没有浏览器 Ctrl-zoom 或 Shift 改轴策略。

owner 的重绘 key 同时含 DOM serialization 和所有元素的 scroll offsets，保证单纯滚动会重绘。4K 样例现比较含 identity/epoch、属性、文本、顺序和 scroll offsets 的拥有值快照，完全相同才复用几何；scroll-only 复用固定 CSS 的 computed style，但重算几何。每个包仍含完整绘制/依赖，固定字体、圆角和 immutable GPU 资产有界复用；静态窗口重复合成已完成的离屏结果。没有生产 revision 失效调度，4K 宿主现最多两帧在途，按实际 query poll 退休，队列满/截图/resize/退出时 drain。renderer C ABI v1 保持不变；render thread 不访问 Node/JSValue。

规范依据：[CSS 圆角与事件裁剪](https://www.w3.org/TR/css-backgrounds-3/#corner-clipping)、[pointer-events](https://www.w3.org/TR/css-ui-4/#pointer-events-control)、[WM_MOUSEWHEEL](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mousewheel)、[WM_MOUSEHWHEEL](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mousehwheel)。这些规范不代表尚未实现的全标准能力。

## 独立验证

同一份 `tests/fixtures/flow-input/fixture.js` 在原生 DOM 和 Chromium 运行，三轮 scroll/font/display/pointer-events/remove/radius mutation。Chromium 用 elementFromPoint 独立测量 3,150 个点；native 报告只提供采样坐标，不提供浏览器期望 target。固定参考保存页面、CSS、测试源码和字体 hash。浏览器另以 640×420 wrapper 模拟宿主 surface clip，隐藏占位 scrollbar，与探针的显式样式条件一致。

3,130 点 target 完全一致；20 点有边界差异。实际 DPR 1.005 下，浏览器 scrollTop=59.701492 而 native=59.25，15 个点落在相邻 row 边界；另外 3 个圆角、2 个 inline 尾部边界存在量化差异。参考保留每点固定 5×5 邻点，差异必须在每轴 2 CSS px 内，且总数不能超过 20。没有把这些点删除或称为精确一致，也没有放宽以前几何/颜色 goldens。

native 另有 seed `0xCE802026` 的 64 次独立算术 oracle mutation（128 个 scroll/z-order 检查），并测 fresh programmatic scroll、disabled 子元素、clip corner、viewport overflow、cancel、fractional/canceled wheel、完整 metadata、listener detach/GC、stale identity/epoch/owner 与 reset root 账目。

隐藏 own-HWND 自测使用实际 PostMessage/SendMessage → WindowProc → owner queue → layout hit → React。验证原始 facility/company focus payload 与 store 回执、2/13 项、滚动后选中、内容缩短夹紧、scroll-only repaint、sub-notch/half-notch、两轴 page/zero policy、负屏幕坐标、消息收到后 resize 的旧 viewport、letterbox、缩小窗口、minimize cancel 和 close。20 次 Present、3 次 resize、18 条 mouse、12 条 wheel；2,389 个 texture 在实际 query 完成后退休，completed serial=40。13 个捕获中的 7 个 640×420 帧共 38,640 CPU 像素样本，硬件最大 2/WARP 最大 1 色阶；调试层零 warning/error。CPU oracle 与 GPU 使用声明的 linear-sRGB 合同，尚无 browser pixel parity。

最新全量和强制 GC 数字以 [status](status.md) 为准；证据封存在 `artifacts/local/flow-input-{debug,release,gc-stress}/run-metadata.json`，窗口图片/report 在 `artifacts/local/cheetah-flow-window-*`，浏览器完成截图在 `artifacts/local/flow-input-reference/frame-2.png`。以前 flow 阶段的 sealed evidence 保留。

## 4K 与 FPS（2026-10-08）

可见 flow 样例现在默认最大化，离屏目标固定为 **3840×2160**。逻辑布局仍是 640×420 CSS px，保持原样组件与命中行为。准备阶段统一缩放绘制几何、glyph advance/offset、矩形/椭圆裁剪，然后 FreeType 在对应设备字号重新生成中英灰度字形；不是放大旧 640×420 字形纹理。原始 PNG 仍使用其固定源分辨率。非 16:9 内容在 4K 目标内有 letterbox；窗口再按目标比例合成。鼠标/wheel 在消息收到时保存两级变换的合成逆映射，内外黑边不会进入页面。

右上角 HUD 与标题栏每约一秒更新 FPS。分子为成功且非 occluded 的实际 Present1 调用数，分母为 steady_clock 实际秒数，包含 owner 工作、重绘、GPU wait 与 vsync 等耗时。静态帧复用已完成的目标并持续 Present，最小化重置采样并标记 paused。此数值是窗口呈现频率，不是每秒重新布局或完整重建 UI 的次数，也不是性能验收报告。标题另列 RT 与实际 client 尺寸；最大化 client 扣除系统标题栏/任务栏，可能小于 3840×2160。

静态合成也在同一宿主队列上以实际 event query 推进 completion serial。HUD 使用独立、拥有值的 immutable coverage 资源，只在前一宿主 submission 确认完成后替换；窗口、HUD 和 swapchain 释放沿同一 drain 条件。renderer C ABI v1 不变。新增 device-space crop 反例保证浮点 `(x+w)-x` 不能扩张源 UV，旧像素回归仍使用原阈值。

该 55 项历史阶段新增 `d3d11_warp_cheetah_flow_window_4k`，自动检查原样 React 的 4K 点击/滚动、内黑边、1000×420 client 缩放、idle 只合成、HUD 替换、query 退休和关闭。Release RTX 5090 与 Debug/Release WARP 均通过；优化前每次 7 次页面重绘、5 次 host-only 合成、815 texture 退休、completed serial 19，4 个 3840×2160 捕获共 3,128 CPU sample 最大 1 色阶，调试层零 warning/error。单独时钟反例检查真实慢帧、暂停恢复、反向时间；viewport 反例检查两级黑边和 minimized/非法尺寸。55 项历史回归结果、命令与产物见 [status](status.md)。当时未重新跑强制 GC、移动或实体设备性能验收；旧 sealed evidence 保留，后续优化的当前数量见下节。

~~~powershell
cmake --build --preset windows-release --parallel 10
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-hardware artifacts/local/flow-4k-interactive
# 新 4K 隐藏场景，旧 --cheetah-flow-window-test-* 仍保持 640×420 golden。
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-test-4k-hardware artifacts/local/flow-4k-release/hardware
~~~

## 4K 滚动优化（2026-10-09）

此节记录加入固定资产缓存后的同步宿主历史结果。当前两帧路径见下节，旧计时和 sealed evidence 保留。

针对实体测试报告的滚动约 6 FPS，先给同一路径添加阶段计时再保存基线，随后做有界缓存。基线主要 CPU 耗时是 owner 输入中的重复几何/字体塑形、4K glyph/圆角像素生成和 RGBA 转换/纹理创建。现复用 owner HarfBuzz font 对象与完整 UTF-8/字号塑形结果；4K 设备字号 glyph 和完整源图/位置/UV/圆角链的 coverage 复用；adapter 的 immutable asset ID/version 复用已实际提交的纹理。字号、文字、源图、裁剪和 paint-order 变化仍重新生成必要值，所有 frame 依赖完整。缓存配额和真实完成退休见 [architecture](architecture.md) 与 [backend](render-backend-api.md)。

测量条件为 Windows x64、AMD Ryzen 9 9950X3D（16 核/32 线程）、RTX 5090、MSVC Release、调试层开启，逻辑 640×420、真实离屏 3840×2160。使用固定原版 React AlertBar bundle、Roboto-Regular / NotoSansCJKsc-Regular、三张 allowlist 的 1024×1536 PNG（实际页面用到固定面板图）；展开 13 项时 60 个 DOM 元素。隐藏 own-HWND 发送固定 wheel，8 帧 warmup 后测 120 帧，scrollTop 在 0/36 px 间交替。计时包含实际 owner/input、最新布局、完整包、prepare/encode、Present1 与 actual query completion；采样中不 readback。GPU 是 UI command-list 的 timestamp 区间，排除 host composition/Present。它验证固定路径，不能当作实体鼠标长时间滚动或生产 UI 的 FPS 保证。

| 热滚动计时（ms） | 基线 P50 | 当前 P50 | 基线 P95 | 当前 P95 |
| --- | ---: | ---: | ---: | ---: |
| owner/input | 41.395 | 0.514 | 49.693 | 0.833 |
| layout | 20.442 | 5.801 | 24.391 | 6.333 |
| packet/glyph/mask | 47.868 | 0.072 | 63.463 | 0.099 |
| prepare/conversion/create | 37.481 | 0.056 | 44.495 | 0.075 |
| encode/release | 2.275 | 0.464 | 2.674 | 0.628 |
| submit/Present | 0.210 | 0.179 | 0.293 | 0.236 |
| completion wait | 5.921 | 8.201 | 14.418 | 9.477 |
| GPU UI | 1.255 | 0.069 | 1.279 | 0.081 |
| total | 155.257 | 15.384 | 201.367 | 15.996 |

total 的基线 P99/max 为 217.471/218.812 ms，当前为 16.558/16.963 ms；各阶段分位数不能相加代替 total 分位数。131 次页面提交的 texture 创建/上传从基线 16,563 次降到 128 次，GPU cache hit 16,435 次。最终冷首帧为 88.849 ms（基线 82.946 ms），没有证明冷启动改善；进程峰值 working set 170.188 MiB（基线 179.840 MiB），末尾 private committed usage 639.090 MiB（基线 693.574 MiB）。GPU cache 纹理有效载荷峰值 25.924 MiB、CPU glyph 0.316 MiB、CPU mask 1.608 MiB；这些不是全部 VRAM 或全部进程原生分配。前一轮优化采样出现 total max 92.576 ms、其中 query wait 84.993 ms，冷首帧 205.410 ms，原始文件保留在 `optimized/previous-run`；桌面调度/等待尖峰和首帧仍待优化。

全量 Debug/Release 56/56 与强制 GC 范围 10/10 通过；最后补容量失败回滚及空字形配额后，三种配置各重建并复跑 cache/4K 两项，全部通过。cache probe 使用 seed 0xCE902026 的 48 次位置、字号、source version、椭圆和透明顺序 mutation，与全量参考对照 200,704 像素，hardware/WARP 最大差异 0。另覆盖 dropped CPU packet、限制 8 项 cache 的整包依赖 LRU、9 依赖容量拒绝并 drain/reuse、Created 资源录制失败后重新上传、提交后宿主失败及实际 completion 前拒绝退休。164 个纹理最终全部退休，调试层零 warning/error。

当前 4K hidden 场景仍为 7 次页面重绘/5 次 idle 合成/12 次 Present、serial 19，texture 创建/上传/退休各 128 次，687 个 cache hit，3,128 CPU pixel sample 最大 1 色阶；旧 640×420 golden 继续原全量路径和 2,389 次纹理退休。产物和源/二进制/结果 hash 保存在 `artifacts/local/flow-scroll-perf/`；`baseline` 是优化前带相同计时器的保存版本，`optimized` 为最终版本，WARP/GC 报告和实际命令见 [status](status.md)。没有修改原游戏或刷新原版 bundle。随后已接两帧宿主，见下节；scroll-only 几何成本、实体滚动、移动与性能预算仍分别待验收。

~~~powershell
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-hardware artifacts/local/motion/interactive
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-perf-hardware artifacts/local/flow-scroll-perf/optimized
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cache-hardware artifacts/local/flow-scroll-perf/render-cache-hardware
~~~

## 两帧流水线（2026-10-09）

4K 可见、hidden 及 perf 窗口默认使用 `TwoFramePipeline`；旧 640×420 golden 与独立同步 reference 保留。DOM/JS/style/layout 仍在同一个 owner，消费消息后生成当前完整包，没有额外等待绘制包队列。两个提交 slot 各自拥有 constant buffer、event query、command list 和 immutable host/HUD lease；Submitted 的资源 pin 与 cache lease 独立。每 tick 非阻塞 poll 队头真实 query；只有两个 slot 都未完成时等待最旧者。截图/resize/退出先 drain，GPU 停滞仍可能阻塞，窗口保持 Present1(1) 的垂直同步。

共享离屏 RT 在同一 D3D11 队列按 UI 写入、host 合成读取、下一 UI 写入排序，CPU 不覆盖 GPU 像素。每个 slot 的 constant buffer 只在真实完成后复用。实际提交了上传的版本可被后续同队列 use 读取，只有 completion 才标 Ready；Created/Encoded 的未提交上传不能被后续包假定有效。HUD 替换由旧 slot 保留旧 SRV lease 到 query 完成，缩放释放 backbuffer 前必须 drain。提交后宿主抛异常仍保存 slot/query/UseToken，不取消已执行命令。C ABI v1 和原始 React/CSS/font/image fixture 未改；Vulkan/Metal、实际 device removal、通用 SDK 多线程调度仍未验证。

`d3d11_warp_two_frame` / `--pipeline-hardware` 使用 seed 0xCE102026，48 帧随机 RGBA/coverage/透明顺序/位置 mutation，成对提交后读回两份独立 staging，与全量同步路径比较 196,608 像素，hardware 与 Debug/Release/GC WARP 最大差异 0。另验证两个同时保留的真实 Submitted use/资源 pin、同队列已提交上传不重复上传、录制失败不占 slot、丢弃未发布完整包、第三次提交不超过两份记录、提交后失败、host lease 实际完成退休和带两个 pending slot 退出。55 个提交/55 完成、serial 110、57 个纹理全部退休，调试层零 warning/error。4K 原样 React 仍为 7 page/5 idle/12 Present、serial 19、128 texture 退休、3,128 CPU pixel sample 最大 1；该带截图的 fixture 常会 drain 到单帧，真正两份未完成记录的证据来自独立 two-frame probe 和 perf 场景。

同一最终 exe 分别运行同步控制与两帧入口；硬件/编译/3840×2160/固定字体图片/60 DOM 元素/13 项列表条件同上，8 帧 warmup 后 120 次 wheel，测量中不 readback，测试和构建在测量后运行。计时版本 v2 增加整个采样区间加尾部实际完成 drain 的 wall clock，用它比较全完成吞吐：

| 实测 | 同步控制 | 两帧 |
| --- | ---: | ---: |
| 120 帧全部完成（ms） | 1858.030 | 701.837 |
| 全完成吞吐（帧/秒） | 64.585 | 170.980 |
| 最后完成 drain（ms，已包含于总区间） | 0 | 14.190 |
| owner tick P50 / P95（ms） | 15.340 / 15.899 | 5.658 / 6.059 |
| owner tick P99 / max（ms） | 16.051 / 16.297 | 6.184 / 7.657 |
| GPU UI P50 / P95 / P99 / max（ms） | 0.068 / 0.072 / 0.084 / 0.090 | 0.067 / 0.073 / 0.077 / 0.083 |
| 冷首帧含完成（ms） | 88.538 | 75.616 |
| 峰值 working set（MiB） | 169.496 | 169.555 |
| 末尾 private usage（MiB） | 638.184 | 639.188 |

同步的 owner tick 截止到实际 completion，两帧截止到 submission；它们不是同一个端到端延迟指标，不能把后一列直接当作显示延迟。两帧在该轮采样中 capacity wait 为零、peak pending=2，CPU layout P50 4.890 ms 成为主要工作；GPU UI 不含 host composition/Present，GPU cache 有效载荷两者同为 25.924 MiB，未测总 VRAM。冷首帧各为单次观察，没有声明统计改善；隐藏脚本路径的 170.980 也不是实体窗口刷新率保证。可见窗口仍会受刷新率/桌面调度和积压影响。

Debug/Release 全量各 57/57（35.95/16.30 秒），强制 GC 范围 11/11（66.33 秒）通过；测试部分并行，时长不是性能指标。完整日志、WARP/hardware/readback/profiles 与源/二进制/hash 在 `artifacts/local/two-frame/`，旧缓存同步证据保留于 `baseline`，同一新版 exe 的同步控制在 `sequential`、两帧在 `pipelined`。实体长时间输入延迟、游戏宿主、多平台、多线程与生产失效调度未验收。下一步降低静态快照检查与 scroll-only 几何开销，扩大实际 UI/SVG/移动范围。

~~~powershell
cmake --build --preset windows-release --parallel 10
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --pipeline-hardware artifacts/local/two-frame/hardware
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-perf-sync-hardware artifacts/local/two-frame/sequential
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-perf-hardware artifacts/local/two-frame/pipelined
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-hardware artifacts/local/motion/interactive
~~~

## 运行

~~~powershell
cmake --build --preset windows-debug --parallel 10
ctest --preset windows-debug --output-on-failure
.\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --cheetah-flow-window-test-hardware artifacts/local/cheetah-flow-window-debug/hardware
ctest --test-dir build/windows-gc-stress -C Debug -R 'flow_input|flow_window|mouse_input|script_dom_lifecycle|page_react' --output-on-failure
~~~

Release 换对应 preset/config；强制 GC 配置见 [building](building.md)。WARP hidden window、native input、browser diff 和 source integrity 注册在 CTest，hardware 另跑。普通构建和 CTest 不联网，不重建原始 React bundle，不读写正在修改的游戏项目。

可见样例已准备，可从项目目录运行：

~~~powershell
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-hardware artifacts/local/motion/interactive
~~~

点击左下“切换 2 / 13 项”，点“仓”展开，在列表上滚轮，再点击一项；resize 后继续滚动/选择，右下按钮接收 fixture 回执。关闭窗口生成 report。当前只自动验证 hidden HWND 注入消息，实体鼠标/滚轮、触摸/惯性、键盘/焦点/IME、可见 scrollbar、多显示器 DPI、完整 PointerEvent/HTML task 时序、Android/iOS 与产品性能预算均未验收。下一步扩大真实档案 UI/样式和 D3 SVG 像素，同时继续准备移动工具链。


## 连续运动展示（2026-10-09）

可见 4K 入口现在默认启用 `examples/cheetah-flow/motion.js` / `motion.css`。金色 LIVE 滑块每个 owner tick 更新；绿色 30 Hz 滑块刻意每约 33.33 ms 采样一次并保持位置，仅作观感参照。Pause/Resume 冻结两条轨道，Speed 循环 1x → 2x → 0.5x。速度基于浮点毫秒经过时间，不依赖 tick 数；延迟 tick 直接计算当前往返位置，不排队补帧。最小化/暂停恢复先锚定时钟，避免长时间隐藏后跳跃。可见窗口仍显示实际 Present FPS，30 Hz 标签不改变全局渲染帧率。

独立演示节点由宿主挂到 gallery，自己的 JS/CSS 不改固定原始 React bundle 或正在开发的原游戏。输入之后在 UI owner 更新 inline `left`，全量 cascade/layout 生成拥有值快照，再通过现有缓存/两帧 D3D11 渲染。退出先移除本地 listener/node，再 unmount 原始 React。通用 rAF、CSS animation/transition、transform/opacity 合成与移动动画仍未实现。

新增 hidden `--cheetah-flow-window-test-4k-motion-hardware/warp` 验证 10/20/40/44/44.25 ms 的几何、30 Hz sample-and-hold、1,600 ms 跳帧/反向、无副作用拒绝非法时钟、原生 pause/resume 和 0.5x/1x/2x、minimize/restore、resize 后按钮命中、React 长列表更新共存与重复 dispose/停止后不再修改 DOM。4K 实际滑块中心像素另按已知金/绿 RGB 检查；五张 4K 和一张 resized PNG 进行整包 CPU oracle 或保存。

最终 hardware 与 Debug/Release/GC WARP 各 158 次页面绘制、3 retained compositions、161 实际提交/161 完成、serial 319、148 texture 全退休、peak pending=2；五张 4K 初始/运动/恢复/终态的 3,910 个 CPU sample 最大差异 1 色阶，调试层零 warning/error。截图和 report 在 `artifacts/local/motion/{hardware,debug,release,gc}/`。完整 Debug/Release 各 58 项、GC 生命周期/两帧/三个 flow 窗口共五项通过；具体命令/时长及最后收紧 React 状态断言后的局部复跑见 [status](status.md)。原有 640 与静态 4K goldens 继续通过。

硬件脚本测量先于构建/回归运行，Windows / Ryzen 9 9950X3D / RTX 5090 / MSVC Release，D3D11 调试层开启，3840×2160、固定 Roboto/Noto CJK/原始三张 PNG、展开的 13 行列表加动画共 70 DOM elements。8 warmup 后 120 次合成时钟 +4 ms tick，测量中无 readback；含实际最终 drain 共 922.839 ms，完成吞吐 130.034 tick/s，tail drain 9.692 ms 已包含。Owner 至提交 P50/P95/P99/max 7.495/8.197/8.325/8.422 ms，其中 layout 6.728/7.100/7.276/7.405 ms；GPU UI 0.073/0.077/0.081/0.081 ms，不含窗口合成/显示。冷首帧 248.246 ms 包含截图/readback/PNG，不应与先前无 readback 冷帧相比；峰值 working set 251.734 MiB、末尾 private usage 672.961 MiB 包含场景截图阶段，GPU cache 有效载荷 26.047 MiB，不是总 VRAM。这里测的是固定合成时钟的隐藏长列表吞吐，不能当实体 FPS/显示延迟；持续 DOM 位置变化会每帧重算样式/布局，负载高于静止画面的 host-only 合成。原始 profile/120 CSV 样本和封存二进制/hash 在 `artifacts/local/motion/`。

~~~powershell
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-hardware artifacts/local/motion/interactive
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --cheetah-flow-window-test-4k-motion-hardware artifacts/local/motion/hardware
ctest --test-dir build/windows-msvc -C Release -R '^d3d11_warp_cheetah_flow_window_motion$' --output-on-failure
~~~

实体按钮/滚轮观感可在上面的窗口直接检查；自动测试只验证 own-HWND 注入。DXGI 实体遮挡恢复、多显示器高刷新/帧延迟、长时间动画 soak、游戏宿主及 Android/iOS 动画性能尚未验收。下一步沿全量基线优化仅位置更新的失效/布局成本，再扩大通用调度与动画能力。
