# 通用交互优化与可切换 Gallery profiler

2026-10-10，C++20 / Windows M0 探针。此次优化针对共享输入、cascade、Scene、几何与绘制路径；原始 Gallery 组件、第三方库、bundle 和 browser oracle 不修改。当前仍为 66/72 可绘制，完整 HTML/CSS、移动后端和真实游戏预算继续待验收。

## 预览与操作

```powershell
& 'F:\Projects\CaseElephant\build\windows-msvc\bin\Release\ce_d3d11_probe.exe' --gallery-window-hardware 'F:\Projects\CaseElephant\artifacts\local\incremental-ui\interactive'
```

默认 **Profiler OFF**，保留基本 Present FPS / Owner Hz，UI 按原调度全速运行。点击侧栏 `Profiler: OFF (F8)` 或按 F8 开启详细面板；再次操作关闭。开启时显示 Owner、Render CPU、GPU elapsed、Frame age、输入排队曲线，及 JS、style、build、layout、geometry、snapshot、hit、dispatch、shell、raster、prepare、encode、Present、pipeline wait 的 Latest/P50/P95/P99/Max 和最近最重的三个案例。每条曲线最多 600 个样本，显示最近五秒；高帧率下保留时长可能不足五秒。HUD 最多 5Hz 更新，不增加 UI 帧率上限。

详细面板和 timer/query 本身有开销；关闭后停用阶段计时、GPU timestamp 命令与 trace 记录，调度、FPS、生命周期和真实 GPU completion 所需时钟/query 保留。该开关属于检查宿主，不改变应用事件、动画或者同步 geometry read 的语义。`--gallery-window-profile-hardware <目录>` 可从启动就启用详细模式。

Esc 正常退出后，曾开启过 profiler 的 run 写 `owner-trace.json` 和 `render-trace.json`。每线程在内存保留最近 65,536 条 owned 事件，退出时一次写盘；长运行超出上限覆盖最旧事件并记录 dropped 数，避免在鼠标热路径逐条写文件。全程关闭的新目录不生成 trace。关闭 profiler 后已有采样仍会在退出时保存。

```powershell
python tools/analyze_gallery_trace.py artifacts/local/incremental-ui/interactive
```

分析器生成 `profile-summary.json` 和可导入 trace viewer 的 `combined-trace.json`，校验共同 steady-clock、当前 run 身份和 settled input accounting，区分完整运行与开关/覆盖后的采样窗口。复用旧目录但本次未采样时，旧 trace 会被 run_id 拒绝，不会被当作本次结果。

`GPU elapsed` 是 producer/UI timestamp 区间，包含队列空隙，不等于纯 GPU busy。Render CPU 是该线程 tick 的墙钟区间，固定 benchmark 会含等待 owner，不能当作采样 profiler 的纯 CPU busy。Owner 与 render 并行，嵌套阶段有重叠，表内数字不能直接相加。Input queue 从 WindowProc 收到消息到 owner 开始执行，不包括此前 OS 队列等待；Frame age 为使用的 owner 发布时刻与当前帧时刻之差。曲线与 trace 均没有物理鼠标到屏幕的端到端延迟测量。

## 共享修复

普通 `WM_LBUTTONUP` 的主动 `ReleaseCapture` 曾同步触发 `WM_CAPTURECHANGED`，被错误翻译成 Cancel。现在区分正常释放与真实捕获/焦点丢失；只对有活动输入状态的会话取消。原生 hover/active 改为比较旧/新 weak generational 路径，不再扫描全部 live slot，detached 节点与传播路径仍按原生命周期规则保活。

共享 style memo 使用受支持 selector 的精确依赖：祖先的标签、namespace、id/class、显式属性和静态匹配所需 pseudo 状态；自身 inline/presentation 声明仍参加原层叠。父值通常只取实际继承字段，显式 inherit/var 保守保留完整父值依赖，typed custom properties 单独作键。稳定 rule winner 与 computed 两个 LRU 各最多 2,048 项/2MiB 有效载荷；容器分配开销不计入该载荷上限。命中照常收取 token/output quota，编译器世代隔离。

`CompiledStyleProbe::update` 在 owner 中保留 unchanged own maps；可选 changed identities 报告字段变化或输出重建。同父重排不触发 own 重建，跨父挂载会重建。Scene 分开保存 canonical raw cascade 与 shorthand 展开派生值，避免每次动画更新因混用两种表示把全树重写。仅变化/重建节点重新展开；继承、CSS-wide、custom properties、无效 computed value 和计划失效仍由全量参考差分验证。

`SceneProbe::update` 接受 owner 专属 mutable 输出，复用未变节点和几何。几何投影包含字体、文本、盒子、transform、SVG geometry 与身份；纯 paint 变化复用 geometry，影响命中/布局的变化立即重算。发布时仍复制为独立完整 immutable snapshot，render 不读 owner 输出、DOM Node 或 JSValue，不依赖可能丢弃的前一未提交包。异常后丢弃部分输出并 reset。

Windows 矢量适配器增加 factory-scoped SVG shape（1,024 项/4MiB key bytes）、stroke（128 项/2MiB key bytes）缓存，同次 geometry solve 的 bounds 按 owned Scene 节点局部复用；factory 改变清空缓存，不跨帧保存 Node 指针。缓存保持绘制、透明、裁剪与层叠顺序。

检查 shell 曾把每次选中卡片的诊断文字写回 `#detail`，导致约 440 节点目录全量布局。现将宿主诊断放进 native overlay；原例自身 DOM/React 状态继续正常更新。shell 忽略本 stylesheet 没用到的 pseudo 状态，其余 geometry key 保留。所有不可见卡片统一暂停 rAF，timer 继续泵送，这是 Gallery host 的显式可见性策略；移除了旧 host 对八个 D3 卡片的例外，不改变中间件定时器 API。

## 验证与测量证据

最终数据及验证记录见 `artifacts/local/incremental-ui/run-metadata.json`。本轮旧基线由上一阶段封存的 1,352 份源文件在独立 runtime source root 重建，SHA 对照原 seal；不让旧 binary 读取新 bootstrap。早期探索和 phase1 测量保留在各自子目录，不能与最终结果混为一轮。

实际 Release / Debug 全量 CTest **各 87/87，67.18 / 246.20 秒**，日志为 `complete-{release,debug}-ctest.log`。两配置另跑 RTX5090 的 `--gallery-window-test-hardware`：各 160 次真实 submission/completion、288 卡片几何字段、17 缓存/full 包对照、1,680 像素采样（最大差 1）、最多两帧在途，debug layer warning/error=0。窗口脚本通过真实 HWND 消息开启 sidebar profiler、hover/click、F8 关闭；断言正常点击不触发 shell 全量布局或误发 capture Cancel，细分 timer/曲线开启后有数据、关闭后 owner detail 停用。截图为 `complete-window-{release,debug}/profiler-detail-4k.png`。

```powershell
cmake --build --preset windows-release --parallel 4
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-release --parallel 4 --output-on-failure --output-log artifacts/local/incremental-ui/complete-release-ctest.log
ctest --preset windows-debug --parallel 4 --output-on-failure --output-log artifacts/local/incremental-ui/complete-debug-ctest.log
& build/windows-msvc/bin/Release/ce_d3d11_probe.exe --gallery-window-test-hardware artifacts/local/incremental-ui/complete-window-release
& build/windows-msvc/bin/Debug/ce_d3d11_probe.exe --gallery-window-test-hardware artifacts/local/incremental-ui/complete-window-debug
```

新增/扩展反例：512 次 cached/in-place/full cascade mutation，覆盖共享相同样式、祖先/class/custom inheritance、generation/reparent/sibling reorder 和 changed identities 精度；命中 quota 拒绝与 reset 恢复。128 次 hover/active/祖先 `[style]`/显式 inherit/SVG presentation 对照，160 次动态 Scene 和 64 次 layout/transition owner 输出对 full 值、数值 paint、像素；geometry paint-only 复用与 transform 失效。新增 `gallery_trace_contract` 验证关闭无文件 I/O、65,540 次事件的 latest ring 覆盖、scope 开关和退出保留。原 Chrome 图像/几何门槛与生命周期/退休测试未放宽。

MSVC ASan Release（`/EHsc /fsanitize=address /Zi`，匹配 14.42 的 ASan runtime DLL）实际通过 style/Scene 两项 **2/2，26.43 秒**，及 160 帧 4K 原生窗口/开关/输入/退休检查。不是完整 87 项 ASan 或强制 GC 重跑。日志为 `accepted-asan-ctest.log`、`accepted-window-asan.log`；初次测试目录 target 未启用、缺少 ASan DLL，以及早期 overlay 容量失败保留在历史日志，不计通过。overlay 现在有 4,096 draws / 512 assets / 16MiB 资产载荷边界，资源仍按实际 completion 退休。

最后补充 owner 采样边界：每次操作开始时固定是否启用阶段时钟，主线程在操作中途开启 profiler 不能令零 time_point 被当作开始时刻。Scope 同时接受 owner 固定状态，分析器拒绝零起始时钟；新增反例确保 recorder 已启用但操作开始时禁用的 scope 不记录。两普通配置最终追加 `gallery_trace_contract` / `d3d11_warp_gallery_preview`，三个配置重跑 hardware 窗口，日志和截图在 `toggle-{release,debug}-ctest.log` / `toggle-window-{release,debug,asan}/`；下方性能使用这次边界修复后的 Release binary，之前三组保留为探索。

本次正常关闭并保留用户手操 profiler 至 `manual-received-latest/`：9,510 processed inputs、采样窗口 77 input / 1,650 publication，trace 无覆盖丢失。此前版本 shell input 最大 25.09ms、shell layout 最大 31.13ms；仅为那一采样窗口的证据，不是所有 hover/click 的独立栈归因。开启按钮自身改变 label 会有一次合法 shell layout，新版反例要求普通案例点击不触发它。

## 最终性能对照

Ryzen 9 9950X3D / RTX5090 / Windows11 26200 / driver32.0.16.1062 / MSVC19.42 Release，D3D11 debug layer 开启。默认 affinity/priority，不并发编译或测试；每例/窗口240个固定60Hz机会，16预热后224样本；4K目标/1920×1080逻辑shell/scene scale2，原固定bundle和相同系统字体/图片。64 scene VM+2 Canvas、439 shell nodes已创建；CPU隔离无GPU，窗口无截图/readback，重型曲线HUD关闭，但两版阶段计时/trace开启。计时数据不是默认关闭profiler时的全速性能保证。

脚本 `artifacts/local/incremental-ui/run-comparison-accepted.ps1` 执行25个CPU案例与K-Line/Anime/Scatter的三组交替旧/新对；Framer/Spring各补一对。完整结果 `comparison-accepted/summary.json`、每帧 `frames.jsonl` 与每run的benchmark/session/trace均保留。P50/P95/P99列取run中位数，max列取所有run最差帧；单对不宣称重复稳定收益。所有22个窗口run实际240次完成、两帧峰值、零debug warning/error；输入排空等式与无丢失的完整trace覆盖通过。

CPU隔离（输入+JS+Scene+geometry，无GPU）代表性结果：

| 原例 | P50旧→新 ms | P99旧→新 ms | 最差帧旧→新 ms |
| --- | ---: | ---: | ---: |
| D3 Interactive Scatter | 4.009 → 3.362 | 7.621 → 5.527 | 12.894 → 8.276 |
| K-Line | 22.607 → 15.608 | 39.188 → 22.481 | 49.440 → 26.230 |
| Anime Stagger Grid | 2.974 → 2.218 | 3.922 → 3.035 | 6.280 → 4.275 |
| Anime Particle Explosion | 2.758 → 2.418 | 6.751 → 5.083 | 13.138 → 11.590 |
| Anime Wave Text | 1.646 → 0.790 | 3.785 → 1.237 | 4.498 → 1.556 |
| Spring Trail | 1.341 → 0.976 | 1.953 → 1.596 | 5.494 → 2.149 |

全部25例结果都在JSON中。七例P50未改善：Framer04 .0088→.0119、Spring00 .0923→.0997、Anime01 .3974→.4405、Anime04 .0802→.0826、Anime08 .2074→.2085、Anime10 .0667→.0962、Anime11 .0404→.0428ms；不以总体收益掩盖这些结果。K-Line每机会snapshot节点工作量峰值1007（可能多次同步读，不是DOM元素数），Scatter359、Anime Grid27/Particle67/Wave31、Spring12；与旧版相同。

完整窗口结果按实际可见集计，包含邻组露出的卡片：

| 窗口 / 可见数 | warm完成吞吐FPS旧→新 | Owner P50旧→新 ms | Owner P99旧→新 ms | Owner最差旧→新 ms |
| --- | ---: | ---: | ---: | ---: |
| K-Line / 1 | 31.29 → 43.93 | 31.81 → 17.85 | 49.46 → 31.93 | 65.58 → 50.33 |
| Anime / 12 | 44.74 → 59.46 | 14.41 → 10.45 | 23.27 → 14.72 | 35.96 → 23.11 |
| Scatter / 2 | 73.12 → 88.55 | 8.25 → 5.93 | 20.00 → 10.86 | 32.34 → 20.36 |
| Framer / 9（单对） | 81.79 → 84.92 | 5.17 → 2.93 | 15.84 → 5.72 | 18.52 → 12.71 |
| Spring / 5（单对） | 73.21 → 77.06 | 6.04 → 3.44 | 8.17 → 5.62 | 14.30 → 12.66 |

前三组新版FPS范围分别42.89–51.74、58.91–60.11、82.17–89.98。旧/新native accepted input分别K-Line120/120、Scatter123/122、Anime6/4、Framer6/4、Spring0/0；差异是移除误Cancel，Down/Up边界未丢。Move仍由host按原策略合并，不能当作每机会4个全部JS Move的保证。

新版窗口资源/尾部（P50/P99/max）：

| 窗口 | CPU tick ms | GPU elapsed ms | peak WS / close private MiB | 最大目录冷启动 ms |
| --- | ---: | ---: | ---: | ---: |
| K-Line | 26.63 / 40.99 / 60.01 | .301 / .361 / 1.026 | 414.7 / 1156.4 | 5660.1 |
| Anime | 16.39 / 22.72 / 32.10 | .185 / 15.491 / 17.805 | 424.4 / 1064.2 | 5619.9 |
| Scatter | 11.46 / 26.27 / 31.47 | .259 / .312 / .971 | 402.5 / 1360.2 | 5639.9 |
| Framer | 7.66 / 30.55 / 33.55 | .126 / 14.050 / 15.622 | 414.7 / 1074.7 | 5479.9 |
| Spring | 9.96 / 30.50 / 33.10 | .231 / 17.434 / 19.135 | 416.1 / 1087.6 | 5658.4 |

没有宣称所有阶段或尾部都改善：Scatter CPU tick P95由21.15升到23.63ms，Framer CPU tick P99由28.34升到30.55ms，Spring最差CPU tick由32.47升到33.10ms；GPU query也有不一致的尾部。完整旧/新P95、内存与启动账目见JSON。目录冷启动含72个隔离短观察与字体/source首次建立，private包含全部VM/参考设施，不是单个游戏View或总VRAM。

阶段差异明确包含：旧trace逐事件写盘、新trace内存ring；旧基本HUD较多文字、新HUD两行加native footer；旧八个隐藏D3仍rAF、新host统一隐藏暂停；原组件/库和当前可见案例不改。因此对照衡量整体通用路径与inspection host交付，不把全部增益归因某一个cache。默认调度存在明显波动，先前探索更高FPS不作为最终保证，也没有从多阶段一起变慢推断系统原因。

新版K-Line warm且有输入机会的CPU阶段中位数：total17.68ms、style3.55、Scene build6.39、layout3.12、geometry.96、hit.77、dispatch1.37ms（嵌套有重叠）。**剩余主要成本是完整Scene构建/样式遍历与布局，GPU纯忙时不是已证明的主因**。多案例Anime Owner P99仍14.72ms；K-Line完整Owner P99仍31.93ms，远未达2ms游戏UI预算。固定窗口逐机会等待publication、age为0，不证明异步物理输入延迟或可见长时FPS。

## 实际限制与下一步

完整 scene 构建、map/string 比较、同步几何求解、复杂卡片 raster/合成仍有成本。此次并未实现完整节点级 dependency graph、自动图层提升、零拷贝 snapshot 发布、JIT 或并行 DOM。后续应优先进一步减少 Scene build 和几何失效范围，保留 full reference 与代际/生命周期差分，再单独优化多卡 raster。K 线和大量同时动画仍需按 Owner/Render CPU 毫秒预算继续优化，不能从独立 Gallery FPS 推断可嵌入游戏时的开销。

未验证：ASan 全范围、最新强制 GC 全范围、实体鼠标端到端延迟、长时真实游戏与公平 Ultralight 同负载对比、Android/iOS/Metal/Vulkan。部分 ASan 与历史 GC 证据不冒充完整平台/整库验证。
