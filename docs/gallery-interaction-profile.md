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

## 关闭垂直同步（2026-10-11）

第 7 步之后画廊在 240 Hz 显示器上锁在 240 FPS，需要关掉垂直同步才能看到实际帧率。

- **开关**：
  - 命令行任意位置加 `--no-vsync`，对本进程打开的所有窗口生效；
  - 画廊窗口和 Canvas 窗口里按 F7 切换，HUD 第二行与标题栏显示 VSync 状态；
  - `tools\gallery.bat` 菜单按 V 切换，作用于 1、2、3、5、7 项；也可以写 `tools\gallery.bat 1 novsync`；输出目录名带 `-novsync`。
- **实现**（`WindowPresenter`）：
  - 关闭时 `Present1` 用 sync interval 0；
  - 系统支持撕裂时（`DXGI_FEATURE_PRESENT_ALLOW_TEARING`），交换链带 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`，present 带 `DXGI_PRESENT_ALLOW_TEARING`；
  - 创建失败时去掉该标志重建。此时不等待垂直同步，但窗口由合成器显示，每次刷新只显示一帧。
- **记录**：`live.json` 增加 `vsync` 字段，`tools/summarize_gallery_live.py` 在场景名后标注 `(no vsync)`。

RTX 5090、可见 4K 240 Hz 窗口，`--gallery-live-benchmark-hardware`，各 2 次：

| 场景 | Present FPS | 被拖卡片更新/秒 | present 间隔 P50 / P99 ms |
| --- | --- | --- | --- |
| anime 拖拽 | 240 → 490–529 | 158–176 → 158–178 | 4.1 / 6.1–6.4 → 1.7–1.8 / 3.3–3.8 |
| 散点图拖拽 | 240 → 388 | 234 → 331–334 | 4.1–4.2 / 5.9 → 2.7 / 3.6–3.7 |

- anime 卡片更新受 owner 限制，帧率提高后不变。
- Canvas 页约 180–200 FPS，受渲染线程每帧约 5 ms 限制，开关前后相同。
- live 基准每个渲染帧注入一次拖拽移动，关闭垂直同步后输入频率随帧率上升，owner 负担更重。因此 owner 指标和输入延迟不能与开启时直接对比。
- 原始数据在 `artifacts/local/vsync-check*`。

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

## 重构之后继续压 owner CPU（2026-10-11，第五轮）

起点是重构第 9 步结束时的 `cd83659`：K 线 owner P50 2.927 ms。每一步先用 owner 采样剖析定位，改完再做交替对照。

测量方法：
- `tools/compare_owner_profile.py --high-priority --rounds 5`，两个版本每轮交换先后，都以 Windows 高优先级运行；
- 环境：RTX 5090 / Ryzen 9 9950X3D / Windows 11 / MSVC Release，固定 60 Hz 时钟，每例 240 帧、16 帧预热；
- 只计 owner CPU，不含 GPU、内存与启动；
- 表中是 5 轮各自中位数再取中位数，每行取自该步与上一步的那次对照。同一个版本在不同对照里会差几个百分点，各步正文同时给出同一次对照里的基线。原始数据在 `artifacts/local/owner-cpu/`。

| 步骤 | 改动 | K 线 P50 / P99 ms | 散点图 P50 / P99 ms | anime P50 ms |
| --- | --- | --- | --- | --- |
| 起点 | 第 9 步结束 | 2.927 / 4.878 | 1.558 / 2.260 | 0.108 |
| 1 | 几何与命中的只读访问 | 2.588 / 3.405 | 1.483 / 2.478 | 0.106 |
| 2 | 属性表按视图查找，样式键不产生临时串 | 2.353 / 3.351 | 1.528 / 2.213 | 0.105 |
| 3 | 样式输入与上一帧只跟着变化走 | 1.914 / 3.301 | 1.396 / 1.995 | 0.101 |
| 4 | SVG 形状按节点保留，几何遍历不再逐节点建表 | 1.747 / 2.327 | 1.402 / 2.024 | 0.103 |

**1. 几何与命中的只读访问**（剖析：K 线 `SceneGeometry::Impl::shape` 占 14.5%）：
- `shape()` 在一个复用的缓冲区里一遍遍历属性、拼出缓存键。原来对 15 个属性名各查两次 map，还用 `std::to_string` 产生临时字符串。
- 几何与命中遍历里的样式读取改用不复制的 `scene_values::view`。
- `svg_affine` 对没有变换的元素直接返回单位矩阵，不再复制、解析。
- z-index 在排序前对每个子节点只求值一次；只有一个子节点时不读，保持原来的报错时机。
- 结果：
  - K 线每轮都更快，采样数从 795 降到 728，`shape` 从 115 个样本降到 43；
  - 散点图 P50 每轮都更快。它的 P99 中位数较高，但逐轮看两边范围重叠（基线 2.16–2.86 ms，候选 2.07–2.77 ms），P95 与最大值持平或更低，属于 224 个样本下 P99 的噪声；
  - 产物逐字节不变。

**2. 属性表按视图查找，样式键不产生临时串**（剖析：第 1 步后 K 线 `CompiledStyle::update` 占 21.6%，其中缓存键 `memo_key` 10.7%）：
- `DomSnapshot` 与 `SceneNode` 的属性表改用透明比较的 `AttributeMap`（`std::map<std::string, std::string, std::less<>>`），用 `string_view` 或字面量查找时不再构造 `std::string`。
- 样式缓存键与选择器路径键的字节不变：
  - 长度前缀用 `std::to_chars` 直接写入；
  - 每个展示属性只查一次；
  - `inherit`、`var(` 改为不分配内存的大小写无关查找，不再整串转小写。
- 结果：
  - K 线 5 轮 P50 都更快：同一次对照里 2.542 / 3.926 → 2.353 / 3.351 ms（P50 −7%）；采样数 728 → 610，`CompiledStyle::update` 157 → 104 个样本，`memo_key` 78 → 48；
  - 散点图 P50 持平（1.522 → 1.528 ms，逐轮范围重叠），anime 持平；
  - 产物逐字节不变。

**3. 样式输入与上一帧只跟着变化走**（剖析：第 2 步后 K 线每帧整份重建样式输入键约占 10%，整份复制上一帧的场景节点 8.2%、计算样式 1.8%）：
- 样式输入键改为 `StyleInput`，由 `CompiledStyle::refresh_input` 按 DOM 子树版本号原地维护：
  - 版本号没变的子树不再读取；
  - 每个节点记下子树的节点数与层数，跳过的子树照样计入节点与深度上限；超限时输入保持原样。
- 求解器记录本次构建整棵取自上一帧的子树（静态复用）。上一帧的场景节点与计算样式只比较、复制其余部分，整棵取来的子树只同步布局写入的盒子。
- 静态复用改为要求计算样式全字段相等（`ComputedStyle` 新增默认 `==`，包括自定义属性），保证取来的子树与上一帧完全相同。Debug 构建在每次同步后自检上一帧与本帧相等，不等时抛出 `WEFT_SCENE_CORRUPTION`。
- 结果：
  - K 线 5 轮 P50 都更快：同一次对照里 2.367 / 3.553 → 1.914 / 3.301 ms（P50 −19%），最大值 5.34 → 4.37 ms；采样数 610 → 约 495，`SceneSolver::update` 362 → 约 250 个样本；
  - 散点图 P50 1.470 → 1.396 ms（−5%，5 轮中 4 轮更快，1 轮持平）；anime 持平；
  - 产物逐字节不变。

**4. SVG 形状按节点保留，几何遍历不再逐节点建表**（剖析：第 3 步后 K 线 `SceneGeometry::geometry` 占 18%、`hit_map` 12%；其中 `shape()` 为每个元素拼键、`to_chars`、对整串 `d` 数据求哈希，两处合计约 11%）：
- 每个节点保留它的形状和构建时用的标签、填充规则与几何属性值。属性没变就直接复用，不再拼键、求哈希；变了才走原来按内容共享的形状缓存。几何属性名先按首字母筛掉绘制、样式类属性。
- 解析过的 SVG `transform` 按文本保留。
- `geometry()` 的包围盒改为按树序写进一个数组：每个节点的变换只解析一次，不再为每个节点建哈希表项；元素几何原地构造，不再整份复制。
- 两种缓存都有条目与字节上限，超出时清空。
- 结果：
  - K 线 5 轮 P50 与 P99 都更低：同一次对照里 1.969 / 2.865 → 1.747 / 2.327 ms（P50 −11%，P99 −19%），最大值 4.57 → 4.12 ms；`geometry` 约 90 → 55 个样本，`shape` 约 57 → 29；
  - 散点图 5 轮 P50 都更快（1.434 → 1.402 ms）；anime 持平；
  - 产物逐字节不变。

**未做：层叠增量化**（第 4 步后约占 K 线 owner 的四分之一，因转入重构第 10 步暂停）。设计要点：
- 层叠直接读 `StyleInput`（它包含层叠读取的全部内容：元素结构、选择器与展示属性、可见的 hover/active），与读 `DomSnapshot` 结果相同。
- `refresh_input` 给每个输入节点打两个进程内唯一的版本戳：自身内容变化时、子树任何变化时各换一个。
- 计算样式节点记下自己从哪两个版本算出，以及子树消耗的节点数、token 工作量与输出字节（簿记，不参与 `==`）。
- 父节点上下文未变（父节点本身的输入未变，且它的上下文未变）且子树版本未变时，整棵子树沿用，消耗按记录累加；累加会越界时退回逐节点计算，保持原来的报错位置。
- 需要的测试：与整份层叠的随机差分、`changed` 列表的完备性、跳过子树仍计入工作量上限（两棵各占约六成上限的子树）。


## 悬停扫动与 scene 图像缓存（2026-10-13）

用户手测：鼠标在 D3 交互的 sunburst（`d3_interactive-00`）和 K 线（`kline-00`）上快速来回扫动时，画廊掉到 60–80 FPS（240 Hz 显示器）。

**复现**：live 基准新增两个场景，`tools\gallery.bat 3` 一并运行。
- 命令：`weft_d3d11_probe.exe --gallery-live-benchmark-hardware <目录> sunburst-sweep|kline-sweep [--no-vsync] [--no-validation]`。
- 输入：窗口打开 1 s 后的 5 s 内，每个渲染帧排队一次无按键的 `WM_MOUSEMOVE`，沿李萨如曲线扫过图表（横向 2 Hz、纵向 3 Hz，约 2000 卡片 px/s），只在卡片的可见部分内；结束时移出卡片。
- 场景名加 `-sampled` 后缀时，同时对渲染线程的 scene 绘制采样，写 `render-samples.txt`（函数名需要 windows-profile 构建的 PDB）。

**原因**（渲染线程采样，K 线）：
- scene 预处理器有文字与图层、阴影、路径三种图像缓存，上限都是 128 条、16 MiB，超出就整个清空。
- K 线一帧有 546 张路径图像：SVG 文字按字形轮廓画成路径，再加 120 根影线、网格与刻度。sunburst 外圈的弧在 4K 下一张覆盖率图就有数 MB，整帧 22.3 MiB。
- 结果是每帧清空数次，复用为 0：
  - 每帧在 CPU 上重新光栅化全部路径（`ScenePreparer::path` 与 FreeType 约 27%）；
  - D3D11 为每张新图像新建纹理（`SceneRenderer::texture` 27%）；
  - 每张图像一次绘制，每次绘制更新一次常量缓冲（约 18%）。
- 另一个因素：D3D11 渲染器在装了 SDK Layers 的机器上总是开启调试层，画廊窗口也不例外。采样中约 10% 在调试层里，绘制密集的帧多花约一半时间。此前文档中的画廊性能数字都是调试层开启时测的。

**修复**：
- 缓存改为按最近使用淘汰（`src/scene/scene_image_cache.hpp`）：只淘汰之前帧用过的图像，当前帧用到的一律保留，所以一帧的图像多于上限时，下一帧仍能全部复用。
- 上限改为每种缓存 1024 条、32 MiB，按实测最大的单帧工作集确定（K 线 546 张，sunburst 22.3 MiB）。探针的 `render-work.json` 新增 `peak_pass_images`、`peak_pass_image_bytes`。单张图像上限仍是 16 MiB，回退判定不变。
- D3D11 scene pass 不再每次绘制更新常量缓冲：最多 256 次绘制的视图常量一次写入，按偏移绑定（D3D11.1 constant buffer offsetting）；设备不支持时退回逐次更新。图像只在切换渲染目标前解绑。
- 探针外壳只在用参考光栅（D2D）重绘时计算损伤区域；原生 pass 本来就重画整帧。
- `--no-validation` 现在也关闭画廊窗口的 D3D11 调试层。`tools\gallery.bat` 的交互入口（1、2、3、7）默认关闭验证，与游戏实际运行一致，菜单按 D 打开；自测（4、8）和 CTest 始终开启。

**测量**：
- 条件：2026-10-13，AMD Ryzen 9 9950X3D + RTX 5090，Windows 11；Release；4K 窗口（3840×2160，壳层 1920×1080 逻辑像素，scene 2 倍）；垂直同步关闭；画廊固定的字体与图片；每次测量 5 s；连续运行，未控温；GPU 时间未单独测量。
- “修复前”是同一基准在修复前代码上的一次运行（那时只能开调试层），另一次运行与它相差不到 5%；“修复后”是 3 次运行的中位数。
- 帧时间是渲染线程每帧的 present 间隔；内存是进程峰值工作集。

| 场景 | 调试层 | 版本 | FPS | 图表更新/s | present P50 / P95 / P99 / 最大（ms） | 渲染 CPU P95 / P99（ms） | 路径重建 / 复用 | 峰值工作集 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sunburst | 开 | 修复前 | 320 | 30 | 1.60 / 17.05 / 19.48 / 25.38 | 16.99 / 19.46 | 7,038 / 0 | 705 MiB |
| sunburst | 开 | 修复后 | 574 | 115 | 1.61 / 2.51 / 3.14 / 4.45 | 2.43 / 2.98 | 56 / 26,394 | 666 MiB |
| sunburst | 关 | 修复后 | 763 | 144 | 1.20 / 1.89 / 2.47 / 4.21 | 1.82 / 2.29 | 56 / 33,064 | 669 MiB |
| K 线 | 开 | 修复前 | 124 | 39 | 1.75 / 23.85 / 25.66 / 28.81 | 23.78 / 25.48 | 106,202 / 0 | 697 MiB |
| K 线 | 开 | 修复后 | 256 | 103 | 1.82 / 8.17 / 9.56 / 11.44 | 8.15 / 9.37 | 1,967 / 275,094 | 716 MiB |
| K 线 | 关 | 修复后 | 363 | 147 | 1.41 / 5.75 / 6.87 / 8.71 | 5.70 / 6.81 | 2,632 / 390,862 | 716 MiB |

- 样本数（修复后）：sunburst 约 2,870 / 3,810 帧（调试层开 / 关），K 线约 1,280 / 1,815 帧。
- 图表更新/s 是被扫动的卡片每秒换新内容的次数；修复前渲染线程画不过来，更新被合并。
- 内存：最初试过 4096 条、64 MiB 的上限，K 线峰值工作集涨到 817 MiB，退出时私有内存多约 265 MiB（缓存留着永远不会再用的 tooltip 文字和十字线图像，每张还对应一张 GPU 纹理），所以收紧到现在的上限。

**仍然存在**：
- K 线场景更新帧约 5.7 ms（调试层关）：
  - 每帧重新解析 SVG 属性约占 24%（`scene_display_list` 中的 `scene_svg_path` 与数值、长度解析）；
  - tooltip 每次 `innerHTML` 生成新节点，按节点身份缓存的文字图像每次都重建；
  - 仍是每张路径图像一次绘制（约 546 次）。
  - 可做：小图像放进图集后合批绘制；文字图像按内容缓存；路径覆盖率改为每像素 1 字节（现在是 4 字节 BGRA）。
- 散点图平移（`scatter-drag`）每帧约 9 ms：平移改变所有圆的设备变换，每帧都要重新光栅化。10 月 10 日它还走 D2D 回退，渲染 CPU 3.4 ms；SVG 改走原生之后变慢。可把 `<circle>`、`<rect>` 等基本形状画成带圆角的盒子，不经光栅化。
- Vulkan scene 渲染器共用同一个预处理器，缓存的改进同样适用；它每次绘制分配一个描述符集的开销没有改。

**验证**：
- 新增 `scene_image_cache_contract`（淘汰顺序、当前帧保留、条目与字节上限、替换与删除的记账）；`scene_native_contract` 新增反例：一帧的图像多于条目上限时，下一帧全部复用，再换一组图像时旧的按最近使用淘汰到上限以内。
- 原生 scene 测试（WARP 与 RTX 5090，34 个 scene）在最后一帧关闭常量偏移再画一遍，与按偏移绑定的结果逐字节相同。
- render-stages 用原来的上限（128 条、16 MiB）运行，保留真实在途帧中的淘汰覆盖：图像离开缓存时，仍在使用它的帧照常完成。
- `ctest`：Release 122/122，Debug 122/122（329.0 s）；Android 交叉编译零警告。
- 产物图像与 10.6d 基线逐字节相同。报告里的变化：render-stages 的 `native_image_evictions` 1 → 35（计数从“清空次数”改为“淘汰的图像数”）；scene-native 的 `frames` 多 34（上面的常量路径对照）；Vulkan scene 的 `uploads` 379 → 319（复用变多，上传变少）。
