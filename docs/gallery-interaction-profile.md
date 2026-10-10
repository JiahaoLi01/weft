# Gallery 交互和动画 profiler（2026-10-10）

针对持续移动时 K-Line 停顿、D3 zoom 拖拽、Anime/Framer 播放和 Spring 无操作的卡顿，增加 owner CPU 与真实 HWND 输入两条采样路径。Present 可以反复显示旧包；只有 Present FPS 不能证明 JS 动画或输入响应及时。当前有明确改善，K-Line 和多案例 Anime 仍未达到游戏 UI 的 2ms 预算。

## 实际改动

- 同步 geometry/computed-style 读取共用一次最新 scene solve；VM 可选按 mutation revision 和宿主 layout epoch 复用拥有值快照/几何/样式。同一状态的重复读不反复求整个页面。宿主必须为时间、viewport、字体/资源等外部布局输入变化更新 epoch；默认未安装该 host 的调用仍全量。回调不得重入 DOM/JS。
- 公共 cascade 的 owner memo 对完整 selector 上下文、父 computed 值和类型化变量作精确键，256 项/2MiB payload；命中仍收取原 token-work/output quota，避免缓存绕过保护。编译器代际和 owner 线程绑定，reset 清空。同步样式读缓存另外限128项/2MiB字符串 payload、geometry最多2048项，不持有 Node/JSValue。
- Scene build 对完整相同 DOM/computed 子树、相同父样式且无 keyframe/transition 状态作保守复用；相同索引优先验证代际身份，插入/重排走 identity 查找。返回包仍完整拥有，不依赖被丢弃包。
- Unicode/emoji 字体图像的源键删除已经由 compositor 应用的 authored transform/opacity；computed 字体、颜色、文本和尺寸仍参与键。Spring Trail 原先每帧重建五张静态 emoji 图像，修正后源只建一次并复用。ASCII mask 和彩色 Unicode 路径继续分开。
- Gallery 显式合并同卡片/按钮状态、连续待处理 Move；正在执行的事件、Down/Up/Cancel/Wheel 和 reset 不合并，常规 `input()` 继续可靠。已排队的最新位置可替换旧移动，计数包含 accepted/processed/coalesced/peak pending。

## CPU 前后测量

Ryzen 9950X3D / RTX5090、Windows11 Pro 26200、MSVC19.42 Release；默认 priority/affinity，一轮安静串行，无并发编译或测试。25例：D3 zoom1、K-Line1、Framer6、Spring3、Anime14。各240个固定60Hz逻辑帧，16 warmup/224样本，初始化排除，不含GPU、sleep或帧率限制。每个案例使用实际定义尺寸和原包，单例VM同一 owner；目录和其他65个VM不在该CPU采样中。输入与step都计入 warm tick。

K-Line/Scatter 连续120个移动机会；Scatter还含down/up和两次wheel；Framer/Anime使用固定原输入计划；Spring不注入操作。原字体/图片/bundle与以前一致（固定SHA的fixture和utility adapter，Windows字体报告在Chrome参考目录）。单轮尾部有系统调度噪声，不能外推所有用户设备。

单位ms：

| 原例 | P50 前→后 | P99 前→后 | 最慢 前→后 |
| --- | ---: | ---: | ---: |
| d3_interactive-03 | 4.424 → 2.799 | 13.244 → 3.825 | 18.880 → 6.543 |
| kline-00 | 48.777 → 13.940 | 57.773 → 26.622 | 80.678 → 33.880 |
| framer-00 | 0.239 → 0.069 | 0.340 → 0.218 | 0.349 → 0.234 |
| spring-00 | 0.057 → 0.059 | 0.135 → 0.131 | 0.141 → 0.216 |
| spring-01 | 1.041 → 0.896 | 1.430 → 1.298 | 1.539 → 1.356 |
| anime-00 | 4.314 → 1.755 | 4.892 → 2.249 | 5.655 → 2.410 |
| anime-05 | 2.382 → 2.049 | 3.196 → 3.251 | 16.769 → 6.117 |
| anime-08 | 1.459 → 0.137 | 7.021 → 1.170 | 12.578 → 5.817 |
| anime-13 | 1.282 → 0.504 | 11.037 → 3.120 | 13.675 → 4.500 |

完整25例及逐帧style/build/layout/geometry/read次数见 `artifacts/local/interaction-profile/before/report.json`、`accepted-cpu/report.json`、`accepted-cpu/frames.jsonl`。Spring Number等小例没有显著CPU收益；Anime粒子05的P99仍约3.25ms，不能只列有利结果。baseline.exe在优化前保留，依赖与输入未改；该baseline没有完整独立源快照，不能将它称为独立可重建历史构建。

## 真实4K窗口路径

最终 `final-trace/` 五组各240次真实submission/completion、2帧在途、16/224 warm采样，不截图、不readback；输入经真实HWND队列。K-Line/Scatter每机会发4个Move，但Windows sample host先合并连续Move，实际accepted分别120/123，**不是480个JS事件的压力保证**。Framer/Anime各两次down/up操作，含Move退出共6输入；Spring无输入。所有组owner accepted=processed+coalesced，trace无丢失，D3D debug warning/error=0。

64 scene VM+2 Canvas VM和438 shell nodes已创建，隐藏新例rAF暂停但timer仍pump，旧8例D3按既有策略继续更新。新30例frame24共1061 DOM element（K-Line444、D3 Interactive380等），不含root/text；每组实际可见数、字体/图片口径见session和固定参考。完整页布局使用1920×1080逻辑shell，绘制3840×2160、scene scale2。多个分组共享同一屏幕，例如Framer viewport也露出Spring，因此耗时按实际可见集计。

| 场景 | 全完成吞吐FPS | Owner P50/P99/max ms | CPU tick P50/P99/max ms | GPU query P50/P99/max ms | 峰值WS/退出private MiB | 含72次观察的启动ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| kline | 53.45 | 19.88 / 26.94 / 31.93 | 26.14 / 34.31 / 39.95 | 0.29 / 0.35 / 0.98 | 406.1 / 1144.9 | 4788.9 |
| scatter | 134.14 | 4.40 / 13.37 / 19.03 | 7.25 / 19.72 / 70.84 | 0.26 / 0.46 / 0.48 | 393.2 / 1464.8 | 5032.7 |
| framer | 157.72 | 2.78 / 10.74 / 14.43 | 5.10 / 13.55 / 81.76 | 0.13 / 2.27 / 2.59 | 406.5 / 1086.4 | 4706.3 |
| spring | 108.07 | 4.96 / 6.63 / 12.55 | 9.16 / 11.71 / 16.31 | 2.34 / 3.23 / 3.61 | 400.0 / 1071.1 | 4744.7 |
| anime | 66.13 | 9.85 / 15.89 / 23.53 | 13.47 / 21.01 / 123.63 | 1.70 / 2.71 / 26.51 | 404.9 / 1069.9 | 4872.8 |

GPU query包围producer+UI，包含CPU等待/queue空隙，排除HUD/Present，并非纯GPU busy。private包含已创建的全部VM/软件参考设施，imported linear字节和GPU缓存字节不等于总VRAM。目录观察及首次字体/source建立使冷启动明显更慢；trace保留所有240帧，不把前16帧从文件删除。`benchmark.json`是warm吞吐，session最后一秒FPS不用于前后比较。

Spring Trail 的中途对照：静态Unicode源修正前paint P50/P99/max=11.592/12.982/13.267ms，修正后0.094/0.141/0.148ms；整个Spring viewport warm吞吐51.61→120.71。最终加上完整双线程trace的一轮为上表，不能用中途121当最终保证。Framer viewport同受Spring Trail拖慢，最终完整trace约158FPS；Anime页仍约66FPS且owner P99约16ms；K-Line更新窗口仍约53FPS、owner P99约27ms。不能宣称这些已经满足2ms或所有主观卡顿消失。

## 手操记录

```powershell
$exe = 'F:\Projects\CaseElephant\build\windows-msvc\bin\Release\weft_d3d11_probe.exe'
& $exe --gallery-window-profile-hardware 'F:\Projects\CaseElephant\artifacts\local\interaction-profile\manual'
# 操作D3拖拽、K-Line移入、播放/重放动画；Esc退出后写完trace。
python tools/analyze_gallery_trace.py artifacts/local/interaction-profile/manual
```

普通4K窗口仍用 `--gallery-window-hardware`。`tools\gallery.bat 2` 一步完成 profiler 窗口和关闭后的分析，`tools\gallery.bat 3` 运行 live 基准并用 `tools/summarize_gallery_live.py` 汇总。profiler窗口额外写 `owner-trace.json`、`render-trace.json`，二者用同一个steady-clock微秒时钟、17位精度。分析脚本离线验证时间、publication/input覆盖和排空计数，合并到 `combined-trace.json` 并输出 `profile-summary.json`。Trace为Chrome/Perfetto trace-event JSON，每条含tid、ts、dur和拥有值参数；owner显示input queue age/各案例style/build/layout/geometry，render显示每卡绘制和整帧（包括owner等待）。HUD显示Present/Owner Hz、owner active P50/P99、frame age P99、inputQ。

每个线程文件上限65536事件，超限只记dropped数量；停止采集后退出以写入合法JSON结尾。输出包含profiling/文件写入开销；普通窗口不开trace。queue_age从WindowProc接收消息计到owner开始执行，不含此前OS队列等待。自动五组是确定性逐帧wait，age为0，不证明真实异步手操时延；manual trace用于后者。没有自动上传任何数据。

重现命令：

```powershell
& $exe --gallery-owner-profile artifacts/local/interaction-profile/new-cpu all
& $exe --gallery-input-benchmark-hardware artifacts/local/interaction-profile/new-kline kline
# 其余过滤：scatter / framer / spring / anime
```

## 正确性与后续

新增512个cascade全量/memo mutation差分及缓存命中仍触发token-work上限的反例、128次同步读mutation/epoch更新、detached/GC/reset、1000个Move夹Down/Up的队列反例、Unicode source位移/透明度复用而字体/颜色/内容必须失效。既有scene的随机mutation/动画/布局full-reference差分和Chrome图像门槛保留。Release/Debug正常配置全量各86/86通过（235.63/524.98s）。最终强制GC五项合同5/5通过（43.02s），覆盖script生命周期、owner队列、scene随机差分、共享平台同步读和style memo；大型30原例的强制GC重放在115/210 captures时主动停止，不纳入通过数，停止后的JSON尾部不完整是部分产物，浏览器审计因此失败；日志保留。硬件、GC最终计数以status和seal实际日志为准。

下一步瓶颈是K-Line hover时整棵SVG/样式值复制、全量style input traversal和geometry；Anime同时可见的多个JS动画求值/scene rebuild；同步字体图像首次建立与部分SVG/圆角D2D fallback也仍有尖峰。实体输入到显示时延、长时间运行、实际游戏接入、公平Ultralight同负载对照、2WebGL/4Lottie和所有移动验证仍未完成。

## 拖拽卡顿与 Canvas 帧率（2026-10-10，第二轮）

用户反馈：带输入时（anime.js 拖拽、D3 Interactive 散点图）拖拽很卡；Canvas 页只有 60–70 FPS，其他页约 220。

### 测量方法

- `--gallery-live-benchmark-hardware <目录> <场景>`：与交互窗口相同的节奏（墙钟时间、不等待地取 owner 最新帧、可见窗口），每个渲染帧投递一次鼠标移动，与真实鼠标被合并后的 WM_MOUSEMOVE 一样。
  - 场景：`anime-drag`（anime-08 的 `.drag-item`）、`scatter-drag`（散点图按下拖动）、`anime`（无输入）、`webgl`（两例 Canvas）。
  - 热身 16 帧后的第 1–6 秒为测量区间，写 `live.json`：present 间隔、owner 发布间隔与 active 时间、被拖卡片（或 Canvas）的内容更新间隔、输入接收到 owner 发布和到渲染线程取到的时间。
  - 只统计测量区间内接收的输入，可见窗口出现时真实光标产生的早期输入不计入。
- `--gallery-owner-profile <目录> drag`：单例 CPU，按下后每帧移动两秒。
- 确定性同步基准新增 `--gallery-input-benchmark-hardware <目录> anime-drag` 与 `--gallery-benchmark-hardware <目录> webgl`。
- render-trace 新增区段：shell page/packet、owner exchange、scene publish/retire、packet assets、submit（含 prepare/encode/present/wait 与 draws/assets 数）、window messages、pipeline poll、hud。

### 发现

1. **Canvas**：渲染线程的 prepare 每帧 8.7 ms。
   - 两例粒子 Canvas 总有半透明像素，渲染器每帧在 CPU 上逐像素 sRGB 解码、预乘并展开成 R32G32B32A32_FLOAT（每例 7.7 MB）再上传；加上 encode 2.3 ms，渲染线程约 14.7 ms/帧，即 64 FPS。
   - 宿主每帧为同一份 Canvas 像素新建 1.9 MB 资产，渲染器缓存因此失效。
   - owner 侧的 Canvas 快照每像素做 3 次整数除法反预乘。
2. **拖拽**：单例 owner 开销不大（anime-08 持续拖动 P50 0.45 ms，散点图 2.9 ms），问题在窗口节奏。
   - 显示器为 4K 240 Hz，Present 同步间隔 1、两个缓冲、DXGI 默认 3 帧队列。
   - 渲染线程每帧要为整个 shell 快照 DOM、深拷贝比较并复制 FlowPage，每次输入再来一遍；owner 发布新帧时还要重绘多张卡片。
   - 单帧 CPU 接近或超过 4.17 ms 刷新间隔时，帧被成批排队后又成批阻塞，present 间隔 P99 16–28 ms；阻塞期间 owner 收不到新请求而空闲。

### 改动

- **渲染器**（`weft::render_d3d11`）：
  - 半透明 sRGB 输入改为上传原始 RGBA8 到 R8G8B8A8_UINT 源纹理，由 `srgb_convert.hlsl` 在 GPU 上写入同一 float 纹理。
  - 转换使用与 CPU 相同的单精度 256 项解码表和 alpha 表，资源计费仍按 float 纹理，源纹理在资源就绪后释放。
  - `render_d3d11_contract` 用 65,536 个通道值×alpha 组合逐位比较，WARP 与 RTX 5090 全部相同。
- **Canvas 快照**：反预乘改为每个 alpha 一行的 64 KiB 查表，结果与整数除法逐字节相同。
- **画廊宿主**：
  - shell 的 subtree revision 随 DOM、hover/active 与滚动变化而变；revision 不变时 page() 与静态包直接复用，page() 返回引用。自测在 revision 未变时仍做完整快照比较，作为反例检查。
  - Canvas 像素未变时复用同一资产，渲染器命中缓存。
  - 窗口标题只在文字变化时设置。
- **帧节奏**：交互窗口与 live 基准使用 frame-latency waitable swapchain（3 个缓冲，最多 2 帧排队），每帧开始前等待交换链接收下一帧。
  - 只允许 1 帧时，单帧 CPU 超过一个刷新周期就整体降到 120 Hz；允许 2 帧多一个刷新周期（4.2 ms）的延迟，但保持 240 Hz。
  - 确定性测试和其他窗口仍是原来的两帧流水线节奏。

### 前后对比

条件：
- 环境：Ryzen 9 9950X3D、RTX 5090、Windows 11 26200、MSVC Release、4K 240 Hz 显示器。
- 负载：64 个 scene VM 与 2 个 Canvas VM 全部运行，开 trace。
- 之前为单轮隐藏窗口（改动前代码，同一测量工具）；之后为可见窗口三轮的中位数。同一版本隐藏与可见窗口的结果一致（例如 anime 拖拽 present P99 7.2 / 7.1 ms）。

| 场景 | Present FPS | present 间隔 P99 / 最大 ms | 内容更新/秒 | 更新间隔 P99 / 最大 ms | 输入到渲染线程取到 P50 / P99 ms |
| --- | --- | --- | --- | --- | --- |
| anime 拖拽 | 220 → 239 | 16.0 / 23.3 → 7.8 / 9.4 | 99 → 93 | 25.3 / 29.3 → 20.8 / 25.0 | 13.7 / 25.7 → 12.9 / 24.9 |
| 散点图拖拽 | 192 → 238 | 17.2 / 28.5 → 11.0 / 12.2 | 139 → 133 | 20.2 / 31.9 → 16.6 / 20.8 | 11.6 / 33.0 → 12.4 / 16.9 |
| Canvas 页 | 64 → 229 | 21.1 / 24.5 → 11.7 / 12.7 | 57 → 92 | 34.8 / 38.7 → 16.7 / 24.8 | — |

渲染线程每帧 CPU（Canvas 页）P50 从 14.7 ms 降到 2.2 ms；shell page/packet/input 合计从约 1.3 ms 降到约 0.1 ms。

### 仍然存在

- **anime 页受 owner 限制**：14 个同时动画的卡片每帧约 10 ms（anime-00、anime-05 各约 2 ms），被拖卡片约 93 次/秒更新、延迟约 13 ms。可能的方向：
  - 输入命中的卡片先步进并提前发布部分帧；
  - 第 7 步类型化样式，减少每帧样式字符串解析和 scene 编译。
- **Canvas 页受 owner 限制**：owner 约 11 ms，其中粒子 Canvas 的 JS 约 6 ms（每帧约 270 个粒子，各有 beginPath/arc/fillStyle 模板字符串/fill），D2D 软件栅格与快照共约 2.3 ms。
- **渲染线程**：
  - encode 约 1.3 ms，为 750 个 draw，每个字形是独立纹理，需要字形图集或实例化才能合批；
  - 每张原生卡片重绘 0.2–0.4 ms，主要是逐帧把样式字符串编译成 paint。
- **硬件 4K 自测偶发失败**：`--gallery-window-test-hardware` 10 次中失败 1 次（改动前代码 4 次全过），单个采样点差 10 色阶（阈值更小）。该自测用墙钟时间，D3 动画每次停在不同位置；当采样点恰好落在动画边缘时，硬件 D2D 与软件 D2D 参考的抗锯齿会不同。本轮改动不涉及这条路径。WARP 版（CTest）不受影响。
- **未验证**：
  - 120/144/60 Hz 显示器、其他 GPU 与笔记本功耗状态；
  - 物理鼠标从设备到窗口的系统延迟（基准从 WindowProc 接收开始计时）；
  - 到屏幕的光子延迟。

### 复现

```powershell
$exe = '.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe'
& $exe --gallery-live-benchmark-hardware artifacts/local/perf-drag/live-anime-drag anime-drag
& $exe --gallery-live-benchmark-hardware artifacts/local/perf-drag/live-scatter-drag scatter-drag
& $exe --gallery-live-benchmark-hardware artifacts/local/perf-drag/live-webgl webgl
& $exe --gallery-owner-profile artifacts/local/perf-drag/cpu-drag drag
.\build\windows-msvc\bin\Release\weft_render_d3d11_tests.exe --hardware
```

live 基准会弹出约 8 秒的可见窗口。各轮产物与 trace 在 `artifacts/local/perf-drag/`。

## 输入优先的早帧（2026-10-10，第三轮）

上一轮之后，anime 页拖拽仍要等 owner 跑完整个 tick（14 个动画卡片约 10 ms）才能看到被拖卡片的变化。

### 改动

- **`OwnerExecutor`**（`src/core/owner_executor.hpp`）新增可选 `Hooks::progressive`。在它内部、只在 owner 线程上：
  - `Progress::publish` 发布本 tick 的提前完整帧，`latest()` 可取到，`wait()` 仍只返回最终帧；
  - `Progress::poll` 运行 tick 开始后新排队的输入，遇到 reset 即停，reset 留给 tick 之间处理；
  - 计数新增 `early_publications`、`polled_inputs`；
  - `owner_executor_contract` 增加反例：阻塞中的 tick 已发布早帧时 `wait()` 必须超时；tick 中排队的两个输入由 poll 运行，之后的 reset 不被 poll 越过，并使该 tick 的最终帧成为 stale。
- **画廊 owner**：
  - 收到输入的可见卡片先步进，并立即与其他卡片的最新快照一起作为早帧发布；
  - 本 tick 已经步进过的卡片再收到输入时只刷新 DOM，不再跑第二次动画帧，保证每个 tick 的 rAF 只运行一次；
  - tick 期间到达的输入只在某个可见卡片之后处理一次。每个卡片之后都处理会逐个运行鼠标移动、失去队列合并：散点图一次移动约 2–3 ms，tick 被拖长到 200–440 ms，其他卡片几乎停住。
- **范围**：只有交互窗口与 live 基准启用早帧；确定性测试仍是每 tick 一帧。早帧不计入 owner 用时、Owner Hz 与 profiler 采样，`thread-work.json`、`live.json` 单独计数。

### 结果

RTX 5090、可见 4K 240 Hz 窗口，三轮中位数，与上一轮结束时相比：

| 场景 | 输入到渲染线程取到 P50 / P99 ms | 被拖卡片更新/秒 | owner tick P50 ms | Present FPS |
| --- | --- | --- | --- | --- |
| anime 拖拽 | 12.9 / 24.9 → 4.3 / 12.9 | 93 → 92 | 10.2 → 9.9 | 239 → 240 |
| 散点图拖拽 | 12.4 / 16.9 → 8.6 / 13.3 | 133 → 155 | 6.9 → 12.1 | 238 → 239 |

- anime 卡片的更新率不变：anime.js Draggable 在动画帧里写 transform，只刷新 DOM 时没有变化，被拖卡片仍是每个 tick 更新一次，只是排到 tick 最前。
- 散点图的 owner tick 变长，因为每个 tick 多处理一批输入（d3-zoom 在事件里同步改 transform，刷新后立即可见），同页其他卡片的更新率相应降低。

## 类型化样式之后（2026-10-11，第四轮）

重构第 7 步（[记录](refactoring-plan.md#第-7-步记录2026-10-11)）降低了 owner 每帧开销，拖拽卡片的更新率随之提高。

### owner 采样剖析

`--gallery-owner-sample <目录> <类别>` 运行与 `--gallery-owner-profile` 相同的场景，另在 warm tick 期间每 250 µs 采样 owner 线程调用栈：
- `samples.txt`：项目帧（库内部记到调用它的项目函数）、独占、包含三张表，带调用者；
- `samples.folded`：火焰图输入格式，便于离线查询任意调用链。

需要带 PDB 的 Release 二进制：

~~~powershell
cmake --preset windows-profile
cmake --build --preset windows-profile --target weft_d3d11_probe
& .\build\windows-profile\bin\Release\weft_d3d11_probe.exe --gallery-owner-sample artifacts/local/step7/sample kline
~~~

第 7 步前 K 线 owner 时间约 32% 在 malloc/free。最大来源是 vector 扩容时整棵 `SceneNode` 子树的复制（约 19%），原因是 `std::map` 成员让移动构造不是 noexcept。

### 结果

RTX 5090、可见 4K 240 Hz 窗口。第一批两边交替各 3 次，第二批只重跑第 7 步 3 次；表中是第 7 步前 3 次、第 7 步 6 次的中位数：

| 场景 | 被拖卡片更新/秒 | owner 活动 P50 / P99 ms | 输入到渲染线程取到 P50 / P99 ms | Present FPS |
| --- | --- | --- | --- | --- |
| anime 拖拽 | 87 → 126 | 10.5 / 20.5 → 6.9 / 11.7 | 4.5 / 13.6 → 4.2 / 10.2 | 235 → 239 |
| 散点图拖拽 | 139 → 200 | 13.5 / 23.5 → 5.7 / 11.3 | 10.1 / 16.8 → 5.9 / 10.5 | 234 → 238 |

- 原始数据在 `artifacts/local/step7/live*`。
- 另外两边交替各 8 次（与 Debug 构建并发，绝对值偏高），方向一致。
- 剩余 owner 开销主要是 DOM 快照的复制与比较（含 gallery 会话 `sync_scene`）和 scene 上一帧的整份复制，留给第 8、9 步。
