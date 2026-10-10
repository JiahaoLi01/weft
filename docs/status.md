# CaseElephant 当前状态

更新日期：2026 年 10 月 10 日。当前是 **M0 部分交付**：C++20 核心和风险探针已落地，真实 React/Vite 页面已贯通 native DOM、inline CSS、Flex、字体与 D3D11，并接入 HWND/swapchain 主鼠标窗口。flow 的 inline/Unicode 换行、同步几何/滚动和圆角裁剪现已接上 paint-order/分片命中及 HWND wheel 消息；可见样例使用 4K/FPS、有界固定资产缓存和最多两帧在途，滚动吞吐采样已完成，并增加可见的连续 DOM 运动与 30 Hz 对照。一般 HTML/CSS 页面加载、通用 SVG、键盘/IME 与移动尚未实现，仍未达到 M0 全部退出条件。

## 本次已实现

通用增量与可切换 profiler（2026-10-10）：修正正常 ReleaseCapture 的误 Cancel，hover/active 改为 weak generational 旧/新路径；共享 style memo 使用精确受支持依赖、两类2048项/2MiB payload LRU。Scene 分离 canonical raw cascade/展开派生值，owner 专属 mutable update 复用不变样式与 geometry，跨线程仍发布完整独立 immutable 快照。Windows SVG shape/stroke/bounds 有界复用。检查 footer 改 native overlay，避免普通点击触发约440节点目录全量布局；原例自身组件/DOM/事件和固定库不改，没有按案例跳过工作。所有隐藏卡片统一暂停rAF、继续timer，属于检查host显式策略，66/72范围保持。

侧栏 `Profiler: OFF (F8)` / F8 可打开详细曲线与 Latest/P50/P95/P99/Max、sync reads和最重案例；默认关闭阶段 timer/GPU timestamp/trace，基本FPS与必要调度/completion保留。每条600样本、HUD≤5Hz、不额外限帧；trace每线程65,536条latest窗口，退出一次写盘，run_id防止旧文件混入。操作与全部口径见[本次合同和证据](incremental-ui-profiler.md)。

最终 Release/Debug 完整 **各87/87（67.18/246.20s）**；两配置硬件4K各160真实完成、288几何/17包对照/1680像素最大差1、两帧峰值、零debug warning/error。原生消息自动验证开启/hover/click/F8关闭，正常click不触发全目录layout或假capture Cancel。最新源 ASan Release style/Scene **2/2（26.43s）**与160帧硬件窗口通过，非ASan/GC完整重跑。差分、关闭无trace I/O、latest ring/切换、异常恢复证据与初次失败日志均保留。

最后固定owner每次操作的采样状态，避免中途开启profiler使用未启动时钟；分析器拒绝零起始时间。两普通配置追加trace/WARP window各2/2（17.59/53.37s），三个配置hardware再各160帧通过，最终记录 `toggle-*-ctest.log` / `toggle-window-{release,debug,asan}/`。完整87项是该纯采样边界补充之前的最终增量核心，边界补充以追加范围验证，不声称再全量重跑。

独立封存旧源1352份重建baseline；默认priority/affinity、相同4K/字体/bundle、固定240×60Hz、16/224 warm、trace计时开启但曲线HUD关闭，无并发编译/测试。最终25例CPU及K-Line/Anime/Scatter各三组交替、Framer/Spring单对；22个窗口各240真实完成与完整trace覆盖。K-Line隔离CPU P50/P99 22.607/39.188→15.608/22.481ms；完整窗口Owner P99 **49.46→31.93ms**，Anime12卡 **23.27→14.72**，Scatter **20.00→10.86**。warm完成吞吐分别31.29→43.93、44.74→59.46、73.12→88.55FPS，不能当实体窗口FPS或关闭profiler保证。七个轻例P50未改善，部分render/GPU尾部也不改善，全部结果与峰值WS/private/启动见 `comparison-accepted/summary.json` 和新文档。K-Line输入机会build6.39/style3.55/layout3.12ms仍偏重，远未达2ms预算；下一步缩小Scene构建/布局失效范围及降低多卡raster成本。

当前运行：`build/windows-msvc/bin/Release/ce_d3d11_probe.exe --gallery-window-hardware artifacts/local/incremental-ui/interactive`。开启Profiler后操作，Esc退出，再 `python tools/analyze_gallery_trace.py artifacts/local/incremental-ui/interactive`。最终源/二进制/正确性/交替性能/手操副本与新seal在 `artifacts/local/incremental-ui/`，旧seal校验保留。实体输入到显示、长期游戏、公平Ultralight、移动和剩余6例仍未验收。

以下为此前阶段的历史证据；单轮数据和旧缓存/profiler策略对应当时版本：

共享Gallery簇、Sunburst退休和交互/动画优化（2026-10-10）：原样新增D3 Interactive4、K-Line1、Framer6、React Spring3、Icons2、Anime14，累计64 scene+2 Canvas=66/72绘制。共享native geometry/computed-style/CTM、事件、ResizeObserver及受限SVG/Flex/Grid已接入；原包和原组件不改，完整HTML/CSS/整库API与移动仍未实现。原始Anime Throw在Chrome/native也静止，惯性只由同固定库的有效轴变换反例证明，不能把原例标成惯性通过。实际兼容范围见[共享技术簇](gallery-shared-cluster.md)。

Sunburst连续wheel退出已复现为512MiB import预算拒绝（result8），离屏旧卡片三版本未退休。现在只按实际完成serial非阻塞回收隐藏backing，同时归还native透明组scratch预算。Release/Debug WARP与hardware均验证旧策略468,048,384 bytes拒绝、新策略206次真实submission/completion、140次透明组重建且scratch归零；完整HWND连续100次wheel到Sunburst再返回、120帧通过Chrome/crop检查。硬件两配置30例/210张图/630次完成和160帧窗口也通过，debug层warning/error=0。

针对鼠标移动/动画卡顿，新增共用最新scene solve、mutation+layout epoch同步读缓存、256项/2MiB payload的owner style memo、保守静态子树复用及连续pending Move采样合并，Down/Up/Cancel/Wheel/reset边界保留。Unicode/emoji源键去除已composite的authored transform/opacity，字体/颜色/文本/尺寸仍失效；Spring Trail paint P50/P99/max从11.592/12.982/13.267ms降到中途对照0.094/0.141/0.148ms，原例不改。新增512个style memo差分/配额反例、同步读freshness/epoch/detach/GC/reset、1000个Move夹可靠边界、Unicode复用与失效反例，旧全量参考和视觉门槛保留。

同默认priority/affinity的单例owner CPU（25例，每例240固定60Hz、16 warm/224样本、无GPU、无并发编译/测试）P50/P99：K-Line48.777/57.773→13.940/26.622ms，D3 zoom4.424/13.244→2.799/3.825ms，Anime拖拽08 1.459/7.021→0.137/1.170ms，Framer00 .240/.340→.069/.218ms。完整4K+双线程trace最终单轮warm吞吐K-Line53.45、Scatter134.14、Framer157.72、无操作Spring108.07、Anime66.13FPS；分别240次真实完成。K-Line owner P99仍26.94ms、Anime15.89ms，不能用Present FPS或单例改善宣称达到2ms预算/消除全部主观卡顿。CPU/GPU尾部、约400MiB峰值WS/约1GiB private、4.7–5s目录启动和准确可见集见[交互测量](gallery-interaction-profile.md)。

最终正常配置Release/Debug全量各86/86（235.63/524.98s），强制GC五项合同5/5（43.02s）。此前旧Debug诊断对所有SVG节点读取造成每eval超时，以及旧窗口失败/中断日志保留；最终正常全量含窗口已通过，旧窗口崩溃没有独立完整栈归因。额外30原例强制GC重放在115/210 captures时主动停止，部分JSON审计失败，不纳入通过；长时间/大量原例强制GC仍未验完。原样新30例正常配置210组state/DOM、4,408个HTML几何字段通过；原34例102 Chrome图像门槛保持。

实际运行：

```powershell
cmake --build --preset windows-release --parallel 4
& .\build\windows-msvc\bin\Release\ce_d3d11_probe.exe --gallery-window-hardware artifacts/local/interaction-profile/interactive
# 手操采样，Esc退出后合并owner/render共同时间轴：
& .\build\windows-msvc\bin\Release\ce_d3d11_probe.exe --gallery-window-profile-hardware artifacts/local/interaction-profile/manual
python tools/analyze_gallery_trace.py artifacts/local/interaction-profile/manual
# 可重复的隔离CPU/真实HWND输入采样：
& .\build\windows-msvc\bin\Release\ce_d3d11_probe.exe --gallery-owner-profile artifacts/local/interaction-profile/new-cpu all
& .\build\windows-msvc\bin\Release\ce_d3d11_probe.exe --gallery-input-benchmark-hardware artifacts/local/interaction-profile/new-kline kline
```

最终产物在 `artifacts/local/interaction-profile/accepted-cpu/`、`final-trace/`、`hardware-release/`、`hardware-debug/`；完整日志为 `gallery-profile-{release,debug}-tests.log`、`gallery-profile-gc-contracts.log` 与hardware四项日志。`tools/record_gallery_interaction_evidence.py` 校验最终结果并保存来源、二进制、产物SHA及旧render-stages seal，输出本阶段 `run-metadata.json`；历史未跑完的gallery-cluster seal脚本不作为本阶段验收。优化前baseline仅有二进制/数据，无完整独立baseline源快照，限制明示。

下一步：K-Line的全量SVG/style值复制和geometry traversal、多例Anime的JS/scene求值及首帧字体源建立仍是热路径。手操异步真实input-to-display、长期运行、总VRAM、实际引擎/外部owner C ABI、公平Ultralight同负载对照、2WebGL/4Lottie与移动配置/编译/链接/模拟器/真机功能和性能仍未验证。自动输入基准等待每次publication，不能代替手操异步时延。

以下显式源图/独立owner及更早段落为历史阶段证据：

显式源图 / 原生 GPU pass / 独立 UI owner（2026-10-09）：完成后续方向 3、4、5 的内部 C++20 host adapters。owned API-neutral paint 在 D3D11 绘制 shapes/gradients/borders、缓存 glyph/source 与隔离透明组，SVG/projective/复杂 clip 完整回退 D2D；宿主明确选择复合源图，简单形状走 batched shader。D2D damage 相对实际已完成 backing 自己的 frame，未知 bounds/full crop/resize 全量；两帧实际 query/lease 退休保持。34 scene + 2 Canvas VM 在同一个 worker 创建/关闭，输入可靠、refresh 可合并、publication 完整不可变、epoch/reset/异常验证，丢弃大快照在队列锁外销毁。目录 shell 为主线程独立诊断 runtime；public C ABI threading 与实际游戏接入待做。

三交替对 4K、240/16 warm/224 samples：同 affinity 中段 158.60→197.80 FPS（+24.7%）、CPU P50/P99 5.996/8.264→4.668/6.828ms、raster 2.367→1.088；末段 203.41→228.65（+12.4%）。默认中段 122.68→147.23、末段 181.44→194.36。controlled owner 中段 1.594/2.325ms（P50/P99），默认 2.587/3.932，整页未达到2ms；末段 P99 5.970→12.847、约20ms最慢帧仍未收敛。三真实线程 hardware 队列 API P99 .013ms、27/27 输入，但不等于整套 UI 成本；renderer/owner/age/内存/启动/query口径与完整第一轮不利数据见 [记录](render-stages-performance.md)。

Release/Debug 全量各80/80、最后相关各7/7，安静 GC 10/10；两配置 hardware native scene/window，Release hardware damage/三线程 async 重跑。原34例102 Chrome图 unchanged mean/IoU阈值通过（2.32729/.798493），native2605/fallback1512/source6 build720reuse、4117实际退休；新增320帧160对照 mean .20115 / IoU1与缓存淘汰、crop、透明重叠/代际变化，窗口160帧/288几何/17包/1680像素最大差1，debug层零warning/error。GC大型原例/window未重复；实体输入、总VRAM、冷首帧尖峰、实际游戏/Ultralight公平对照、Metal/Vulkan/移动配置到真机均未验证。

当前：`build/windows-msvc/bin/Release/ce_d3d11_probe.exe --gallery-window-hardware artifacts/local/render-stages/interactive`。4K HUD分开显示Present FPS / OWNER更新Hz。产物、663源快照与seal在 `artifacts/local/render-stages/`，650旧源独立重建baseline，旧seal保持、原101文件drift0。下一步缩减完整scene/诊断字符串、转换/producer与字体atlas成本，追踪末段等待尾部，推进实际引擎C ABI接入与移动验证。

以下 owner-hotpath 与更早内容为历史阶段证据：

Owner 热路径两项优化（2026-10-09）：34 个 gallery scene VM 直接调用 task/rAF，最早到期时间避免无任务扫描；native DOM mutation/subtree revision 和 1,024 项弱 journal 覆盖树/属性/文本/scroll，不额外保活 detached，root 未变时复用 owned snapshot。scene 编译热变换/透明度/颜色，GPU 直接消费数值，256 项 / 2MiB accounted 的静态绘制参数程序复用形状、边框、origin/clip/z 下标，递归 eviction 持有自身程序。renderer C ABI、owner/值边界、透明组/裁剪、完整包和实际两帧退休保持，未增加一般 CSS/SVG/HTML 或线程能力。

Windows RTX5090 / 9950X3D / MSVC Release、4K、240 帧/16 预热/224 样本、3 交替对：默认中段 85.63→116.21 FPS、末段 116.65→148.18；两版相同 affinity 0xFFFF / High 子进程中段 140.74→163.04、末段 178.25→208.24，每对 after 均更快。控制中段 CPU P50/P99 6.724/9.151→5.738/8.078ms，未达 P50≤2/P99≤3ms；主要收益是 JS 1.071→0.582 与 snapshot .136→.025ms，scene .963→1.004ms，raster2.315→2.269ms，不能宣称数值路径大幅减轻整页。字符串和完整 scene 仍生成；默认波动、尾部/内存/启动、GPU elapsed 口径与全部限制见 [完整记录](owner-hotpath-performance.md)。

实际 Release/Debug 全量各76/76（27.65/62.67秒），最后 quota/module/owner 边界修正后相关8/8（27.42/57.59秒），hardware complex / 原 scene GPU / window 两配置重跑。新增320 scheduler旧模块差分、512随机revision/full快照、quota/deadline/reset/weak GC/scroll/viewport，complex316图像对照（128typed+4缓存容量）hardware mean/IoU .720556/.958484、4 program evictions/316真实退休。原102图/1476字段/4117真实退休，窗口160帧/288几何/17包/1680像素差最大1保持，调试层零warning/error。GC相关8/8 46.86秒，最后边界同范围8/8 44.22秒；未重复GC大型原gallery/window，非实体输入验收。

当前运行：`build/windows-msvc/bin/Release/ce_d3d11_probe.exe --gallery-window-hardware artifacts/local/owner-hotpath/interactive`。产物、源快照与新seal在 `artifacts/local/owner-hotpath/`，保留并校验上一阶段seal；基准旧版从642封存源在独立runtime source root重新构建，避免误读新bootstrap。原游戏/包/bundle只读不改。下一步消除剩余诊断字符串与完整scene更新、降低producer提交成本，再按游戏UI毫秒预算验收；移动、完整72实绘与公平Ultralight对照仍未验证。

以下为此前阶段的历史证据：

CSS 中段继续优化（2026-10-09）：Windows GPU border 的原 stroke/outer-mask/side-wedge 组合预编译为 filled path，按完整几何参数最多缓存 256 项；较大矩阵和软件参考保留旧 mask 路径，group opacity/overflow/绘制顺序保持整体语义。owner 在 cascade 失效时展开 shorthand，减少 target map 和布局复用时整个 previous frame 的重复复制；当前 returned scene 仍完整独立拥有。原函数/包/bundle、更新频率、两帧退休与 C ABI 不变，没有 D3 专用优化或自动图层提升。

4K / RTX5090 / Ryzen9950X3D / Release debug layer，240 帧/16 预热/224 样本，三组交替：相同 affinity 0xFFFF / High priority 子进程条件下，中段吞吐中位 **117.91 → 136.37 FPS**，末段 **153.07 → 176.64**，约 +16%/+15%；中段脉冲圆环 P50 **0.418 → 0.191 ms**。默认调度中段却为 **83.66 → 79.08**，末段 89.16 → 109.63，多个未改动阶段一起波动；不能据受控结果宣称默认整页稳定提速或确定波动原因，窗口没有强设 affinity/priority。所有样本、尾部/内存/启动、单次首屏/D3 sanity 和未测项见 [本轮完整记录](css-middle-performance.md)。

实际 Release/Debug build + CTest 各 **75/75，26.82/63.75 秒**；两配置另跑硬件复杂 CSS / 原 scene GPU / window。新增 64 帧 owner 布局参考/shorthand/transition/containing-block/viewport 与 full 值/几何/软件图对照；复杂 GPU 对照扩到184（增加96 fractional/异色/不等宽/旋转/重叠透明/越界反例），真实184提交退休，硬件 mean/IoU 0.595009/0.958484；原102图/1476字段、4117提交退休、160帧窗口/288几何/17包/1680像素与零调试 warning/error 保持。强制 GC owner/复杂 CSS/external/two-frame/cache **5/5，15.02秒**，本轮未重复 GC 大型原 gallery/geometry/window，命令和历史证据范围明确列账。

当前预览：`build/windows-msvc/bin/Release/ce_d3d11_probe.exe --gallery-window-hardware artifacts/local/css-middle-performance/interactive`。源/9个executable/图片/两类调度条件全24个主要run/日志/原游戏drift封存 `artifacts/local/css-middle-performance/run-metadata.json`，之前 seal 校验并保留。72目录/36实绘/36诊断与平台未验证项不变；下一步定位默认调度整页波动和降低 JS/scene/producer 提交成本。

以下为前一轮复杂 CSS 与更早阶段的历史证据，当前结论以上述中段阶段为准。

复杂 CSS 优化阶段（2026-10-09）：既有显式 planar 3D 源图改由 Windows GPU perspective 采样，角度变化复用源；四边同宽同色 solid 圆环一次描边，异色/不等宽边框原路径与 256 项 wedge cache 保留；owner 缓存动画 list/每属性 track 和有界端点解析，按完整 underlying style 失效。加入失败计划重复拒绝、继承/隐式端点/动画参数/文本/移除重插对照；layout signature 比较减少 map 分配，数值输出保持 12 位有效数字。没有自动图层提升，C ABI、原游戏/包/bundle、更新频率与视觉阈值不变。

最终 4K / RTX5090 / Ryzen9950X3D / MSVC Release+debug layer、固定 240 帧/16 预热/224 样本，旧版与新版交替测 3 组：**复杂 CSS 中段中位吞吐 67.38 → 87.56 FPS（after 范围 79.00–91.44），末段 80.60 → 106.61（94.04–111.22）**，约 +30%/+32%；中段 CPU P50 的 run 中位数 14.474 → 11.243 ms，末段 11.946 → 9.115。首轮翻转单卡 1.014 → 0.169 ms。首屏首次采样下降但交替样本不一致；D3 三组中位数 185.00 → 166.36，范围重叠，尚未排除退化，未宣称稳定提速。隐藏实际完成吞吐不等于可见长时 FPS，也未与 Ultralight 同负载比较；全部分项/尾部/内存/启动/首轮与重复数据见 [复杂 CSS 实际证据](complex-css-performance.md)。

普通 Release/Debug build 与 CTest presets 实际执行，各 **75/75，26.92/65.26 秒**；两配置另硬件跑 `--complex-css-test-hardware`、`--scene-gpu-test-hardware`、`--gallery-window-test-hardware`，产物 `artifacts/local/complex-css/final/{release,debug}/`，WARP 为 CTest。新增 160 帧 owner 动态 mutation 与关闭增量的 scene 值/软件图对照；GPU 64 mutation +24 angle-only、8 target switch、88 真提交/退休和 fast/fallback 计数，硬件 mean / IoU 0.592469 / 0.964642；24 次 angle-only source upload 1 次。原102图/1476字段/120随机mutation、4117 GPU提交退休/透明滤波/滚动露出、160帧窗口/288几何/17包/1680像素继续通过，阈值不放宽，调试层零 warning/error。

强制 GC scene/geometry/原 GPU/复杂 CSS/external/two-frame/cache **8/8，446.67 秒**；追加 cache proof 后只重建 ce_d3d11_probe / 新复杂 CSS 1/1、2.90 秒。最后仅测试变量更名消除 shadow warning，三配置重建 scene_contract 并复跑各 1/1（0.38/3.21/3.48 秒），没有改测量 executable；本轮不重复 GC gallery window，完整实际命令与未测项见上述文档。最终源/九个二进制/图片/全部性能样本/日志 SHA 和原游戏 drift 在 `artifacts/local/complex-css/run-metadata.json`，旧 seal 保留。

当前预览：`build/windows-msvc/bin/Release/ce_d3d11_probe.exe --gallery-window-hardware artifacts/local/complex-css/interactive`。完整 72 目录 / 36 实绘 / 36 诊断、4K/FPS 保持；仅验证 Windows 固定 CSS/planar 3D 范围，移动/Metal/Vulkan/device-loss/一般 CSS 3D/完整 gallery 与长时真实游戏 UI 未验证。下一步继续降低 scene/group isolation 成本，并定位轻场景吞吐的变动，不能保证所有复杂动画达到 90 FPS。

以下为首次 GPU 和此前阶段的历史证据，当前结果以上述复杂 CSS 阶段为准。

Gallery 性能阶段（2026-10-09）：**26 CSS + 8 D3 的可见 body 已接 Windows D2D GPU 绘制 → linear-premultiplied RGBA16F → 既有 external texture lease**，完整 WIC 软件参考保留。样式解析/样式与布局增量复用、文本布局/emoji/gradient/路径/圆角/3D 平面内容缓存、scratch layer 与三版本卡片池重用，消除每帧完整 CPU 图像转换/上传和活跃资产身份清空。仍单 UI owner、最多两帧在途，实际 completion 后重写/退休，完整 packet 不依赖前一包。公开 C ABI、原游戏/脚本/bundle 和原图阈值不变；72 目录、36 实绘/36 诊断、4K/FPS/输入继续保留。

最终独立 RTX5090 / Ryzen 9 9950X3D / Windows 11 26200 / MSVC Release、debug layer 开启、3840×2160、同字体/原例，240 帧含 16 预热后 224 样本，无截图回读，依次跑保存的旧 binary 与新版：**CSS 首屏 14.09 → 117.56 FPS；16 body 中段 5.22 → 55.14；D3 21.91 → 128.39**。中段 CPU P50/P95/P99/max 从 188.805/218.983/235.638/244.682 降至 17.347/21.203/23.626/31.003 ms。隐藏真实完成吞吐不能替代可见长时 FPS；未跑同负载 Ultralight，中段也未达用户报告的 90 FPS。GPU 范围差异、内存/启动、最慢帧、baseline 223/new 224 interval 公式和原始数据见 [性能条件与完整结果](gallery-performance.md)。

实际执行 `cmake --build --preset windows-release/windows-debug --parallel 8` 及对应 `ctest --preset windows-release/windows-debug --parallel 6 --output-on-failure --output-log artifacts/local/gallery-performance/final-{release,debug}-ctest.log`（每个 preset 分别运行）：**Release 74/74，32.39 秒；Debug 74/74，73.08 秒**。两配置另实际运行 `--scene-gpu-test-hardware artifacts/local/gallery-performance/final/{release,debug}/scene-gpu-hardware` 和 `--gallery-window-test-hardware artifacts/local/gallery-performance/final/{release,debug}/window-hardware`，WARP 从 CTest 执行。34 原例/102 张真实 Chrome 图仍过 mean<=3 / IoU>=0.75（硬件实际 2.39076 / 0.757239），新增半透明 filter/opacity oracle 最大差 1，裁剪/滚动露出验证、4117 提交退休、两帧 peak/零调试 warning。窗口 160 帧/288 几何/17 缓存full/1680 像素最大差 1；1476 浏览器几何与 120 随机 mutation、40 动画帧缓存/full、selector key/owner/reset/卸载反例保持通过。

强制 GC 实际重建 ce_scene_contract_tests / ce_scene_gallery_tests / ce_d3d11_probe，运行 scene/geometry/GPU original/window/external/two-frame/cache 共 8 项；首轮并行 7/8（269.16 秒），窗口加载触及原 5000ms JS 预算，单独复跑 1/1（342.68 秒）。8 项不同检查最终均有通过证据，预算/断言/超时未改，初次失败日志保留；完整实际命令在上述性能文档。源/最终三配置二进制/图片/基准/日志 SHA 与只读原游戏 drift 在 `artifacts/local/gallery-performance/run-metadata.json`，先前 seal 不覆盖。

预览：`build/windows-msvc/bin/Release/ce_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-performance/interactive`。本轮仅 Windows 固定范围；Android/iOS、Metal/Vulkan、device-loss、长时性能与完整 72 原例尚未验证。projective 投影、部分 gradient、color text 首次生成和 Canvas 仍有软件工作；下一步优先降低复杂 CSS 中段 scene/producer 成本，继续按真实阻断接 SVG 交互与其余案例。

以下 CSS/D3 首次渲染及更早记录为历史证据，软件性能缺口和旧未实现描述对应当时版本；本轮当前结果以上述性能阶段为准。

CSS/D3 scene 阶段（2026-10-09）：**26 CSS + 8 D3 原样案例已新增 native 渲染通过**，连同已有两个 Canvas 共 36/72。已接独立 SceneProbe/Windows D2D SOFTWARE+DirectWrite adapter，固定原 Tailwind utility 离线语义编译（123 utility / 29 原 keyframe），关键帧/transition、gradient/变换/边框/圆角/group opacity、基础 SVG path/轴/文字；native DOM descendant query、function-only interval 和 mouseover/out relatedTarget 扩展。旧 flow/auto/positioned profile 与公共 C ABI 不变，原游戏/函数/包/bundle 未修改。显式范围与诊断见 [scene gallery](scene-gallery.md)。

每个原例 121 帧，seed 0xce202610 / 60Hz / 0–2000ms；CSS 4–6 悬停/离开、CSS7 两次点击，400/1050/2000ms 三张图，共 **102 张 native PNG 对照真实 Chrome 155.0.8059.39、DPR2、完整原 CSS**。各 RGBA channel mean <=3 色阶、soft foreground IoU>=0.75；实际最差 2.33236 / 0.765045。34 个空底色反例必须全部失败。同步变换几何 **1476 字段**，阈值0.3 CSSpx，实际最大0.274975。SVG descendant 通过独立像素检查，尚不宣称 SVG geometry/hit。动画changed pixels、原React状态与卸载后保留bitmap/scene、live_nodes回到3均验证。另120轮随机mutation/full场景布局与像素差分、线程owner拒绝、隐式border终点和alternate-reverse反例通过。

实际构建：`cmake --build build/windows-msvc --config Release/Debug --parallel 8`（两配置分别执行）；最终 `ctest --test-dir build/windows-msvc -C Release/Debug --output-on-failure --output-log artifacts/local/gallery-css-d3/{release,debug}/full-ctest.log` 各 **73/73，69.62/342.14秒**。新增 scene_contract / scene_gallery_original / scene_fixture_integrity / scene_browser_geometry，250项fixture SHA校验通过。Debug/Release另执行 `ce_d3d11_probe.exe --gallery-window-test-hardware artifacts/local/gallery-css-d3/{debug,release}/hardware`，WARP由CTest执行；窗口保持160帧/72标签/288卡片几何/17完整包差分/1680像素采样，新增CSS悬停/点击和中段/末段/D3截图。实际两帧完成退休、最小化恢复和FPS更新通过，调试层零warning/error。各时间含不同验证并行，不是性能指标。

强制GC实际执行 `cmake --build build/windows-gc-stress --config Debug --parallel 8 --target ce_scene_contract_tests ce_scene_gallery_tests ce_d3d11_probe`，`ctest --test-dir build/windows-gc-stress -C Debug -R '^scene_contract$|^scene_gallery_original$|^scene_browser_geometry$|^d3d11_warp_gallery_preview$' --output-on-failure --output-log artifacts/local/gallery-css-d3/gc/final-ctest.log`，最终 **4/4，715.87秒**。完整34例/121帧/102图/1476字段与160帧WARP窗口通过，未重复72模块的隔离短观察，该项明确not_run_gc_instrumentation。初次普通240秒超时（scene在force前、随后窗口），依赖geometry缺文件而失败，日志保留 `gc/initial-timeout-ctest.log`；仅GC把两个大型测试超时设为600秒，不改JS预算/采样/断言。最后纠正“38 pending”文案为36（72减36），重建三配置，普通两配置重跑硬件窗口；GC重跑共享renderer/原Canvas窗口范围，记录 `final-wording-scope.log`，该文字改动不改变34例功能。

运行：`build/windows-msvc/bin/Release/ce_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-css-d3/interactive`，默认CSS，侧栏D3，4K/FPS/LIVE36，全部72目录可滚动。原其余36例仍显示缺口。34VM保留，8D3离屏继续timer/rAF，可见body才raster/upload；原两Canvas继续离屏暂停。Windows body由OS DirectWrite（本机Microsoft YaHei UI/Arial/Consolas/Emoji）塑形，跨机器字体、移动和矢量GPU路径未验证。

Release RTX5090隐藏正确性窗口末段同时显示16个CSS body：111个暖帧CPU P50/P95/P99/max为203.586/224.414/228.085/249.822ms，GPU离屏UI为4.201/5.314/5.413/5.620ms，成功Present约4.75FPS。带readback并与其他验证并行，**不能用作Ultralight性能对照**；但已明确本全量software raster/布局/RGBA32F上传路径仍有性能缺口，不套用旧flow的200FPS。gallery缓存预算512MiB（旧默认128MiB不变），该run peak缓存536685801 bytes，进程peak working set1413697536 bytes/private at close2195726336 bytes。源/二进制/产物与条件seal在 `artifacts/local/gallery-css-d3/run-metadata.json`，旧seal保留；初期字体/时钟探索输出不算最终通过。完整CSS/Grid、另4个D3 Interactive、WebGL、36待接例、实体设备与移动仍未验收。下一步分项profile并扩展GPU paint/纹理格式，按实际阻断接SVG交互。

以下为此前阶段的历史证据，旧通过数和未实现描述仅对应当时profile：

4K Gallery 检查窗口阶段（2026-10-09）：修复可见粒子/烟花的 timer/rAF 精度不一致；`std::to_string` 向上舍入导致时间倒退，现统一 classic locale / max_digits10，同一 raw double 传给两者，保留单调性验证。新增 72 例/10 组的原生 DOM/CSS 检查 shell，保留原始标签、分组顺序、列数和内容高度；侧栏、原生 wheel、案例摘要、Home/End/PageUp/PageDown、4K/FPS/CPU 分位数和两例原始 Canvas 的同页输入已接通。其他 70 例展示 JS/像素缺口，未新增原始像素通过。普通 Debug/Release 启用现有 Canvas 的隔离 12 次观察为 40 例执行未报错/32 例阻断，与之前无 Canvas 的 38/34 条件不同。

实际命令：`cmake --build --preset windows-debug/windows-release --parallel 10 --target ce_d3d11_probe ce_animation_frame_tests`（各 preset 分别运行），对应 `ctest --preset windows-debug/windows-release --output-on-failure` 全量分别 **69/69，120.17/37.95 秒**；最后仅 GC instrumentation 的观察分支调整后，普通两配置重建并复跑 animation_frame、gallery_preview、gallery_canvas_window 范围，各 3/3，记录在 `final-scope.log`。Debug/Release 均另执行 `--gallery-window-test-hardware artifacts/local/gallery-preview/{debug,release}/hardware`；WARP 为 CTest。每 run 160 帧、288 卡片几何字段、17 次缓存/全量包逐绘制/资产/UV/顺序对照、10 组首屏截图、1,680 个实际 GPU/CPU 像素采样最大差 1（容差 3）；粒子 move/烟花 click/目录 wheel、最小化恢复、FPS 已更新、峰值两帧在途和全部真实 query 退休通过，调试层零 warning/error。

强制 GC 实际重建 `build/windows-gc-stress` 的 ce_animation_frame_tests / ce_d3d11_probe，并运行 `ctest --test-dir build/windows-gc-stress -C Debug -R '^animation_frame_contract$|^d3d11_warp_gallery_canvas_window$|^d3d11_warp_gallery_preview$' --output-on-failure`，最终 **3/3，92.42 秒**。GC 下只验证 72 卡片 shell、两例 live Canvas VM 的完整 160 帧/输入/退休，不重复 72 个大型模块的隔离加载；报告明确标 not_run_gc_instrumentation。第一次把全量模块审计也放入强制 GC，在进入窗口前耗时超过 200 秒后主动停止，失败日志保留 `gc/initial-stopped-module-audit.log`，不是通过证据。

运行入口：`build/windows-msvc/bin/Release/ce_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-preview/interactive`。产物/完整错误栈/截图/统计和最终 seal 位于 `artifacts/local/gallery-preview/`；旧 Canvas 与 external-texture seal 保留。可见模式无每帧 readback，隐藏测试带截图复制；FPS 的负载是 shell + 可见 Canvas，尚不能与 Ultralight 原完整 gallery 的 40 FPS 比较。800×600 backing 未变，4K 文字按设备字号生成。原游戏/第三方源码/bundle 未改；SVG/WebGL/原始完整 CSS、其余 70 例可见内容、实体输入、移动和公平性能对照仍待验证，下一步按目录中的真实阻断扩展。详见 [运行和实际限制](gallery-preview.md)。


外部纹理 lease 阶段（2026-10-09）：新增图形 API 无关的 optional C ABI extension，保留原 renderer/frame/backend v1 布局。代际 caller lease、prepare/encode/use 独立 pin、有界 registry/byte budget、caller release 失效、真实 completion 退休与 host-confirmed abandon 已实现；D3D11 reference 导入有效 SRV、验证实际 device/format/dimensions，拒绝 native resource 重复发布和 input/RTV alias。支持同 device/queue 的 linear-premultiplied RGBA8/16F/32F、top/bottom origin、UV/opacity 和有序合成，见 [实际外部纹理合同](external-texture-lease.md)。

最终 **Debug 68/68，74.25 秒；Release 68/68，27.12 秒**。实际执行 `cmake --build --preset windows-debug --parallel 10` / `ctest --preset windows-debug --output-on-failure` 和对应 Release preset。Debug/Release 均显式运行 `ce_d3d11_probe.exe --external-texture-hardware artifacts/local/external-texture/{debug,release}/hardware`，WARP 从 CTest 执行。两配置/两驱动各 48 个 GPU producer 纹理/提交、峰值两个在途、实际 query 退休 48 个 external lease 和 24 个缓存 CPU coverage texture，源像素没有 CPU 读回/重传；每 run 1,572,864 个 CPU bilinear/透明合成采样，WARP sRGB 最大差 3 色阶/linear 0.00263887，RTX 5090 为 2/0.00331104，最终调试层 warning/error 为零。阈值为 sRGB 3 色阶和 linear 1/256；有限 subtexel 精度使初始 2/0.003 的假设过严，已记录边界，不宣称逐像素一致。producer sampler 缺失和 query 重复覆盖的探针 warning 已修正。

合同测试新增 512 轮固定 seed 生命周期压力，覆盖 import/prepare failure、caller 提前释放与两个 use、取消/部分命令错误、容量/owner/epoch/重入、无分配提交/完成和 abandon；C11 新扩展头/入口通过。源/二进制/产物 SHA、最终日志、四张预览 PNG/每 run report 和只读原游戏 drift 在 `artifacts/local/external-texture/run-metadata.json` 及 `{debug,release}/`。本轮没有改 DOM/JS，未重跑 GC instrumentation；普通全量既有 DOM/Canvas/React 通过。测试时长不是性能指标，机器/画幅/资产与未测指标见上述合同。

当前仅验证 reference 的同 device/queue、单 mip/sampling、LDR linear premultiplied；encoded sRGB/straight alpha/HDR、shared/keyed mutex/多队列、driver device-removal、通用 viewport SDK/真实 4K 动态 producer、移动均未验收。WebGL2/ANGLE 尚未接入，gallery 新增通过数仍只有上一阶段的两个 Canvas2D 案例。下一步固定 ANGLE 依赖并实现最小 WebGL2/颜色转换与 canvas/context 生命周期，然后接原 Three/gallery。以下 Canvas/rAF 记录为各自历史阶段证据。


Canvas2D 阶段（2026-10-09）：两个原样 gallery 案例（粒子/烟花）已在自己的 QuickJS、native context、positioned 几何和 D3D11 中运行，各 64 个 rAF、两次移动/点击、400×300 CSS 缩放并恢复，分别 6,500/5,006 次 fill；原游戏与原始函数/包未改。图形 API 无关的 owner raster factory 接 Windows D2D SOFTWARE/WIC，输出完整 immutable sRGB straight RGBA8，Node/context tracing、detached 保活、尺寸同值重置、容量/故障/epoch 均验证。已加入最大化 4K 合成/FPS 窗口，Tab 切换、移动/点击、Esc 退出；原 Canvas backing 仍为 800×600。运行命令、精确值范围与限制见 [Canvas 阶段](gallery-canvas.md)。

最终 **Debug 67/67，74.12 秒；Release 67/67，26.16 秒；强制 GC 的 Canvas/DOM/rAF/八轮 React 4/4，51.32 秒**。实际执行两组 `cmake --build --preset windows-{debug,release} --parallel 10` / 对应 CTest preset；GC 使用 `cmake --build build/windows-gc-stress --config Debug --target ce_canvas_probe_tests ce_script_dom_tests ce_animation_frame_tests ce_page_probe_tests --parallel 10` 与 `ctest --test-dir build/windows-gc-stress -C Debug -R 'canvas_2d_contract|script_dom_lifecycle|animation_frame_contract|page_react_contract' --output-on-failure`。Release 首轮新增 style 断言误用了序列化空格（已改为比较颜色值）；后续发现旧 HUD probe 假设资源必在途，已增加实际提交且完成确认前检查保活。修正后 Release 全量通过，Debug 最终 Canvas/4K 范围另 5/5、9.74 秒。日志分开保存，测试时间不是性能指标；部分任务并行，不用于比较性能。

Debug/Release 的硬件 RTX 5090 与 WARP 各运行两个原样案例。每 run 20,460 个 GPU/独立 CPU 合成采样最大 1 色阶，每例 65 个真实完成提交/65 张退休纹理，峰值两帧；VM reset 后旧 FramePacket 重放一致。Chrome 155 的六张固定 bitmap 有显式 AA/低 alpha 舍入差异：分量最大 51–76、亮区均差 2.16–5.08、交并比 0.9208–0.9843，属于有界形状/颜色回归，非逐像素一致。另自动验证隐藏 4K HWND 点击队列、切换与 FPS HUD 像素；实体输入仍未人工验收。256 mutation snapshot 分批差分、200 detached callback/context 循环、quota/overlap/版本/故障合同通过。

产物与日志在 `artifacts/local/gallery-canvas/{debug,release,gc-stress,reference}/`，固定浏览器 PNG/输入 hash 在 `tests/fixtures/canvas-gallery-reference/`，源/二进制/产物及只读原游戏 drift 记录在 `artifacts/local/gallery-canvas/run-metadata.json`。只验收新增两例，其余 70 例未增加像素验收；完整 Tailwind 页面、通用 Canvas/positioned hit-test、WebGL2/ANGLE、公共 external texture lease、移动和此路径性能均未验证。下一步先 external texture lease，再固定 ANGLE/WebGL2。以下为上一阶段历史证据。


Gallery/rAF 阶段（2026-10-09）：只读捕获 CRPG UI 的 101 个文本文件和全部 **72 个原样案例**，固定原 React 19.2.4/D3/Three 0.183.2 等 59 包、5,354 个源码/许可证文件。完整原页面与十个分组可离线构建，UE bridge 只记录本地值、游戏 socket 禁用。新增可选 owner rAF，公共浮点批次时间、回调间微任务、取消/下一批快照、隐藏恢复、GC/epoch 和整体预算均有反例测试；原 React Spring 三例各由 12 个 rAF 实际改变 native DOM。完整源/依赖和复现见 [gallery 阶段](gallery-foundation.md)。

72 例原生短观察为 **38 例未发现执行错误、34 例阻断**，不标为渲染通过。主要缺口是 Canvas/WebGL、组合 selector、setInterval、NodeList、computed style/geometry 与第三方动画所需浏览器对象。完整模块能够初始化，完整原页面运行仍有错误。真实 Chrome 155.0.8059.39 的 800×600 / DPR 1 / 250 ms 单例参照没有观察到脚本错误，两个 Three.js context 确认 WebGL2；四个 Lottie 远程 JSON 明确阻止，动画未成功加载。浏览器参照不等于自己的 renderer 通过。所有 gallery native pixels/interaction/performance 与移动均未验证。

本阶段全量 **Debug 63/63，83.29 秒；Release 63/63，46.46 秒；强制 GC 的 DOM/rAF/八轮 React 生命周期 3/3，46.09 秒**。实际构建使用两个 build preset，以及 `cmake --build build/windows-gc-stress --config Debug --target ce_animation_frame_tests ce_script_dom_tests ce_page_probe_tests --parallel 10`；全量用对应 CTest preset，GC 用 `ctest --test-dir build/windows-gc-stress -C Debug -R 'animation_frame_contract|script_dom_lifecycle|page_react_contract' --output-on-failure`。随后补充 Spring DOM 变化审计、异常消息/stack 和 callback invocation/coercion 反例，并重跑最终 rAF/gallery 范围（Debug/Release 各 5/5、GC rAF 1/1）；最终细分时间与 binary/source/产物 SHA 记录在 `artifacts/local/gallery-foundation/run-metadata.json`，全量和最终日志分开保存。部分任务并行，耗时不是性能指标。

最新 native JSON 在 `artifacts/local/gallery-foundation/{debug,release}/native-execution.json`，参照/截图在 `artifacts/local/gallery-foundation/reference/`，固定 bundle/metadata 在 `examples/crpg-gallery/native/`。正常构建不联网、不需要原游戏，源码 drift 检查只读。该 Foundation 阶段尚未实现 Canvas；现两例进展见上方。该 Foundation 阶段的公共 external texture API 尚未实现，现进展见上方；ANGLE 继续待实现；SVG/CSS/动画按逐例缺口扩展。旧连续运动窗口和 C ABI 不改变；下方为此前阶段证据。

| 模块 | 实际行为 | 边界 |
| --- | --- | --- |
| 构建 | CMake/CTest、Windows x64 Debug/Release presets、静态库、C11 调用测试 | 未验证 portable-ninja、SDK 打包与符号隔离 |
| 核心 | 实验 C ABI、Runtime/View、owner 检查、有界 View slots、跨 Runtime/代际句柄、busy destroy、失败原子性、单调 host 时间 | ce_view_record_tick 不执行 JS、布局或渲染；React 链路在独立实验宿主 |
| 原生 DOM 与脚本 | QuickJS-NG custom class 与 C++ Node graph；统一 tracing、roots、detached/expando/listener 循环回收、事件 capture/bubble、epoch、getElementById、自有值 snapshot；可安装 owner layout host 提供 fresh geometry/scroll/client rects 和可取消 wheel | 有限子集，无 HTML parser、computed style、焦点或完整 DOMRectList；scroll job 当前微任务时序，尚非完整 HTML scroll tasks |
| D3 | 原版 D3 7.9.0，在自己 VM/native DOM 执行共享 fixture；真实 keyed join、axis、line、HTML/SVG 与事件 | 固定 workload 的 L0/L1，未实现 native SVG/style/paint/GPU、transition 或 drag/zoom |
| 资源生命周期 | CPU/Prepared/Encoded/Submitted pin、取消、失败、提交前容量预留、epoch/abandon、shutdown drain；已由独立 GPU 探针接入实际 query | 此模块自身是合同状态机；device removal/移动 GPU 尚未实际验证 |
| 可扩展 backend API | 独立 v1 C ABI function table、capabilities、typed native pass 的 opaque 传递、owner/instance/epoch/generation、有界 prepared/use、提交/完成、部分失败、drain/abandon；实际 D3D11 reference 与 React window 接入 | 有序 quad/coverage/SDF profile；reference adapter 仍是 probe-private；Vulkan/原生 Metal/外部 texture lease/通用 paint IR 尚未实现 |
| D3D11 探针 | 实际 texture/SRV、command list、宿主 Execute、query 退休、颜色/alpha、direct/RTT、readback；新增真实 React 页面的背景/文字 | 已有独立 HWND/swapchain；无 SVG、通用 paint IR 或稳定 renderer API |
| 公共样式 | value snapshot 输入/输出，author/inline important、继承、属性 selector、:root、token var 与 selected-fallback cycle/IACVT、rgba；Flex/positioned 共用 | 有界严格 CSS profile；稀疏 formatting 输入，无 JS getComputedStyle/失效缓存或通用文档加载 |
| 自动布局 | 新 auto_page 值接口组合受限 block/Flex/absolute、px padding、内容高度/中英固有宽度、ASCII 多行；原样 AlertBar 自动尺寸、48 个 mutation 场景与八帧 DX11 | 零 margin/border、leaf text、显式 scope；无一般 inline/Unicode 换行、滚动、圆角、transform 或通用 hit-test |
| Flow / inline / 滚动 / 圆角 | 独立 flow profile 组合 nested inline/span/br、匿名 block、normal/nowrap/pre-line 与 UAX14/29、基线/分片；有界 LTR overflow hidden/auto/scroll、同步偏移夹紧；px/% 椭圆圆角、重叠归一化、嵌套 clip；27 个 mutation 场景和原样 React 八帧 browser/DX11 | 旧 auto profile 不扩张；零 margin/border、固定 normal/400 字体；跨源 grapheme/ligature、inline-block/replaced、bidi/字典断词/soft hyphen、完整 scroll tasks/scrollbar、实体 wheel/触摸及移动未验收 |
| Flow 输入窗口 | 逻辑身份树与 paint-order hit regions、inline 分片、滚动/圆角/viewport clip、fresh callback router 和完整 WheelEvent metadata；原样 React 接 HWND owner 队列、scroll-only repaint、细分/取消/两轴 wheel、signed screen/receipt viewport、resize/cancel/close | 隐藏 own-HWND 注入消息通过；实体设备、触摸/惯性、完整 focus/keyboard/IME/PointerEvent、可见 scrollbar、多显示器 DPI、移动及性能未验收；renderer C ABI v1 保持 |
| Flex | Yoga 3.2.1、CSS 默认/盒尺寸/shorthand、受限 auto-min intrinsic、measure/baseline、最终文本重排；React DOM snapshot 经 page adapter 使用同一几何路径 | 显式 Flex profile；已接共享有界 author cascade，无通用 block/inline；一个已保存的 column basis 补丁 |
| React/Vite 页面 | 原版 React 19.3.0/useState/keyed reorder、Vite 8.3.3 双构建；25 个元素经自己的 DOM/style/layout/font 输出三帧原生 GPU 像素 | 限定 inline/Flex/nowrap/Roboto；离屏直接 dispatch 与窗口布局命中主鼠标均通过；无通用文档加载或移动 |
| 主鼠标窗口 | HWND/flip swapchain、owner 消息队列、fresh layout hit、UIEvent/MouseEvent、弱 identity、disabled/display:none/pointer-events、capture cancel、letterbox resize 和 close | 主按钮单击实验；自动 own-HWND message 通过，实体鼠标、完整键盘/焦点/IME/多显示器与移动未验收 |
| 字体 | FreeType 2.13.3 metrics/灰度/SDF，HarfBuzz 11.2.1 glyph/advance/cluster；固定 Roboto/Noto CJK 的有界 LTR 整 cluster 回退、font_id；新 inline OpenType Win/typo 度量和 libunibreak UAX14/29 | 有界中英 Unicode 换行与灰度页面已验证；bidi、语言配置、字重/斜体、CJK SDF/rounded SDF、编辑/atlas 与移动未验收 |
| 连续运动展示 | owner 浮点时间 → JS inline left → native style/layout → 两帧 GPU；每帧/30 Hz 对照、pause/speed、minimize/resize/cleanup | 仅演示局部时钟；通用 rAF、CSS animation/transition、合成动画与移动尚未实现 |
| 4K/FPS 与固定缓存 | 3840×2160 高 ppem 中英字形、两级 letterbox 输入、实际 Present FPS HUD；拥有值输入相等才复用 geometry，scroll-only 复用 style；有界 shape/glyph/mask/immutable GPU 缓存与 actual query drain | 仅固定 owner CSS/字体/资产作用域；两帧队列满或截图/resize/退出时仍等待，无生产失效调度/atlas/字体热重载；实体长时间 FPS、移动性能和预算未验收 |
| CaseCheetah 状态组件 | 原样 AlertBar/NineSlice、Zustand 5.0.15/store/focus/双向 UE 协议，八帧中文 GPU 与浏览器状态/几何对照 | 作者预览 CSS 和固定数据；已接原始主题块/展开规则，尚无完整 operations.css、UE C++ 游戏服务、通用 positioned hit-test 或完整 UI |
| SDF GPU | 固定 R8 distance atlas、0.5×/1×/1.5×、Roboto 24/48 ppem、导数 AA、蓝色描边、独立直边 coverage oracle 与 actual query 退休 | 独立 Windows 探针，未实现页面/动态 atlas、完整尖角质量、移动后端或 MSDF |
| 固定依赖 | QuickJS、D3、Yoga、FreeType、HarfBuzz、Ahem、Roboto notices、React、Zustand、Noto CJK 和 libunibreak 6.1 共 5,106 文件逐项 hash 校验；Vite/工具链另以 package-lock 固定 | 更完整 paragraph/bidi 库和 PNG decoder 未锁定；依赖移动构建尚未验证 |

连续运动展示阶段：可见 4K 窗口默认增加每帧金色 LIVE / 30 Hz 绿色参照轨道，支持原生 Pause/Resume 与 0.5x/1x/2x。高精度单调宿主时间在 UI owner 修改 DOM inline `left`，经全量 style/layout、缓存与两帧 D3D11；最小化/暂停恢复不追赶隐藏时间，dispose 撤销本地 listener/node。原游戏、原始 React bundle、web host bootstrap、依赖与 C ABI 未改；这是局部演示时钟，不是通用 rAF/CSS animation 实现。

本阶段完整 **Debug 58/58，54.19 秒；Release 58/58，19.89 秒；强制 GC 的 DOM 生命周期/两帧/640+4K+motion HWND 五项 5/5，48.44 秒**。实际构建 `cmake --build --preset windows-release/windows-debug --parallel 10`（分别调用），GC `cmake --build build/windows-gc-stress --config Debug --target ce_d3d11_probe --parallel 10`；完整对应 CTest preset，GC 命令 `ctest --test-dir build/windows-gc-stress -C Debug -R 'd3d11_warp_(cheetah_flow_window|two_frame)|script_dom_lifecycle' --output-on-failure`。之后仅收紧 motion 场景的 React 字符串状态断言，三配置重建 probe 并复跑新增 motion test，Debug/Release/GC 各 1/1，22.68/3.89/27.04 秒；结果记录于同阶段 `ctest-final-motion.log`。部分构建/测试并行，时长不是性能指标。

Hardware 实际命令 `ce_d3d11_probe.exe --cheetah-flow-window-test-4k-motion-hardware artifacts/local/motion/hardware`；新增 motion hardware/WARP 范围各 158 page/3 retained/161 Present，161 actual completion、serial 319、148 texture 全退休、peak=2，3,910 CPU sample 最大 1 色阶，debug warning/error=0。测试覆盖浮点时间、跳帧反转、30 Hz 保持、非法时钟原子性、按钮速度/暂停、最小化恢复、resize 命中、React 共存和重复退出。Windows / 9950X3D / RTX 5090 / MSVC Release、3840×2160、固定字体图片、13 行/70 DOM、8 warmup+120 合成 +4 ms tick，含最终 drain 的完成吞吐 130.034 tick/s；owner 至 submission P50/P95/P99/max 7.495/8.197/8.325/8.422 ms，layout P50 6.728 ms。持续位置变化比静止 UI 多出每帧全量 style/layout，隐藏长列表吞吐不等于实体 FPS。冷帧/尾部/GPU/内存、可运行步骤及限制见 [连续运动证据](flow-input-window-probe.md#连续运动展示2026-10-09)；产物与源/二进制 hash 在 `artifacts/local/motion/`。实体遮挡/显示延迟、长时间动画 soak、CSS/rAF 与移动未验收。以下两帧及更早数字是历史阶段证据。

两帧宿主阶段：**Debug 57/57，35.95 秒；Release 57/57，16.30 秒；字体/输入/browser、HWND wheel、DOM 生命周期、缓存和八轮 React 强制 GC 范围 11/11，66.33 秒**。实际构建为 `cmake --build --preset windows-release --parallel 10 --target ce_d3d11_probe`、`cmake --build --preset windows-debug --parallel 10`、`cmake --build build/windows-gc-stress --config Debug --parallel 10`；测试为对应 Debug/Release CTest preset 的 `--output-on-failure`，GC 为 `ctest --test-dir build/windows-gc-stress -C Debug -R 'flow_input|flow_window|mouse_input|script_dom_lifecycle|page_react|render_cache|font_fallback|two_frame' --output-on-failure`。部分测试并行运行，时长不是性能指标；日志与源/二进制/产物 hash 在 `artifacts/local/two-frame/run-metadata.json` 及各 debug/release/gc 目录。

4K 窗口默认最多两份宿主提交记录，两个独立 constant buffer/event query，command list 与 immutable HUD/host lease 保留到真实 completion；DOM/JS/style/layout 仍同 owner。正常 tick 非阻塞 poll，队列满等待最旧 slot，截图/resize/close 先 drain。UI 和 host-only composition 同一队列排序；实际 Submitted 上传可被后续同队列 use 复用，只有 completion 标 Ready，取消/录制失败不留下假 ready。C ABI v1 不变，同步 reference 和旧 640×420 golden 保留。独立 hardware 与 Debug/Release/GC WARP 两帧 probe：seed 0xCE102026、48 帧随机 RGBA/coverage/位置/透明顺序、196,608 像素最大差异 0；两个 Submitted pin、host lease、上传复用、丢包、录制失败、第三帧容量、提交后失败和带两份 pending slot 退出均通过。55 实际提交/55 完成、serial 110、57 texture 全退休，调试层零 warning/error。原样 4K React 仍 7 page/5 idle/12 Present、serial 19、128 texture 退休、3,128 CPU sample 最大 1 色阶，额外验证在途 HUD 替换与 resize/close drain。

同一新版 Release exe 分别运行 `--cheetah-flow-window-perf-sync-hardware artifacts/local/two-frame/sequential` 和 `--cheetah-flow-window-perf-hardware artifacts/local/two-frame/pipelined`。Windows、Ryzen 9 9950X3D / RTX 5090、调试层开启、真实 3840×2160、固定 React/Roboto/Noto/三张 PNG、13 项列表/60 DOM 元素，8 帧 warmup 后 120 次 wheel，无测量期 readback，测量后再运行构建/测试。120 帧包含最终实际 GPU drain 的 wall clock **1858.030 → 701.837 ms**，全完成吞吐 **64.585 → 170.980 帧/秒**；两帧 peak pending=2，本轮 capacity wait=0，尾部 drain 14.190 ms 已计入总区间。同步 owner tick P50/P95/P99/max 为 15.340/15.899/16.051/16.297 ms；两帧为 5.658/6.059/6.184/7.657 ms，但前者截止到 completion、后者截止到 submission，不能直接当相同的端到端显示延迟。GPU UI P50 0.068 → 0.067 ms，P95/P99/max 同步 0.072/0.084/0.090、两帧 0.073/0.077/0.083 ms，不含窗口合成/Present。冷首帧 88.538 → 75.616 ms 是各一次观察；working set 峰值 169.496 → 169.555 MiB、末尾 private usage 638.184 → 639.188 MiB；GPU cache 有效载荷峰值均 25.924 MiB，未测总 VRAM。隐藏脚本吞吐不是实体 FPS 保证，垂直同步和桌面调度仍影响窗口。实际命令、数据定义和步骤见 [两帧流水线](flow-input-window-probe.md#两帧流水线2026-10-09)。原游戏、固定 bundle、依赖未改；移动、实际 device removal、游戏宿主和实体输入延迟未验收。以下缓存同步及更早数字保留为历史证据。

4K 滚动优化阶段：主改动全量 **Debug 56/56，37.05 秒；Release 56/56，17.05 秒；字体/输入/browser、HWND wheel、DOM 生命周期、缓存和八轮 React 强制 GC 范围 10/10，77.59 秒**。实际执行 `cmake --build --preset windows-debug/windows-release --parallel 10`（各 preset 单独调用）及对应 `ctest --preset windows-debug/windows-release --output-on-failure`。GC 实际命令为 `ctest --test-dir build/windows-gc-stress -C Debug -R 'flow_input|flow_window|mouse_input|script_dom_lifecycle|page_react|render_cache|font_fallback' --output-on-failure`。之后补空字形缓存配额和超容量准备失败回滚，三配置分别重建 `--target ce_d3d11_probe`，复跑 `-R 'd3d11_warp_(render_cache|cheetah_flow_window_4k)'`；Debug/Release/GC 各 2/2，3.44/2.70/5.26 秒。完整与最后局部日志在 `artifacts/local/flow-scroll-perf/{debug,release,gc}/`；测试时长不当性能数据。

先保存带相同阶段计时器的优化前基线，再在 RTX 5090 / Ryzen 9 9950X3D、MSVC Release、真实 3840×2160、固定原版 React/Roboto/Noto/PNG、13 项列表/60 DOM 元素下测量。隐藏 own-HWND、8 帧 warmup 后 120 次 wheel，测量中不 readback：总帧 P50/P95/P99/max 从 **155.257/201.367/217.471/218.812 ms** 降为 **15.384/15.996/16.558/16.963 ms**，131 次页面提交 texture 创建/上传从 16,563 次降为 128 次，GPU cache hit 16,435。GPU UI command-list P50 1.255 → 0.069 ms，不含 host composition/Present。冷首帧 82.946 → 88.849 ms；进程峰值 working set 179.840 → 170.188 MiB、末尾 private usage 693.574 → 639.090 MiB；GPU cache 有效载荷峰值 25.924 MiB，不是总 VRAM。前一轮有 total 92.576 ms/query wait 84.993 ms 尖峰及冷首帧 205.410 ms，保留在 `optimized/previous-run`，没有掩盖等待尾部和首次加载成本。实际命令 `ce_d3d11_probe.exe --cheetah-flow-window-perf-hardware artifacts/local/flow-scroll-perf/optimized`；这是固定脚本路径测量，实体长时间 FPS/移动/产品预算仍未验收。

缓存 hardware 与三配置 WARP probe 用 seed 0xCE902026 的 48 帧随机源图/字号/位置/椭圆/透明顺序 mutation 对照全量路径，200,704 像素最大差异 0；丢包、整包 LRU 依赖保护、9 项输入超过 8 项 cache 的失败 drain/reuse、录制失败后 Created 重传、提交后失败与真实 query 前拒绝退休均通过。164 个 texture 最终全退休，调试层零 warning/error。4K hardware/WARP 场景保留 7 次页面重绘、5 idle/12 Present、serial 19，当前 128 texture 创建/上传/退休、687 cache hit；四张 4K PNG 的 3,128 CPU sample 最大 1 色阶。旧 640×420 路径和像素阈值未改。源/二进制/产物 hash 封存在 `artifacts/local/flow-scroll-perf/run-metadata.json`，基线有独立旧 exe/hash，运行步骤与各阶段计时见 [flow 窗口与滚动优化](flow-input-window-probe.md#4k-滚动优化2026-10-09)。没有修改原游戏、刷新 bundle 或更改 renderer C ABI v1。

此前 4K/FPS 阶段历史结果为 Debug 55/55（57.47 秒）、Release 55/55（16.56 秒）；hardware/WARP 的 7 page/5 idle 场景优化前退休 815 texture，3,128 CPU sample 最大 1 色阶，调试层零 warning/error，产物在 `artifacts/local/flow-4k-{debug,release}/`。逻辑 640×420 不变，设备字号重新栅格、两级 viewport、实际 Present FPS 和 idle query completion 已落地；该轮没有重跑强制 GC。以下更早数字保留为历史阶段证据。

Flow 输入窗口阶段最终结果：**Debug 54/54，85.76 秒；Release 54/54，15.26 秒；输入/browser、HWND wheel、DOM 生命周期和八轮 React 强制 GC 7/7，106.84 秒**。实际执行对应 Debug/Release build/ctest presets，GC 为 `ctest --test-dir build/windows-gc-stress -C Debug -R 'flow_input|flow_window|mouse_input|script_dom_lifecycle|page_react' --output-on-failure`。测试配置曾并行运行，时长不是性能指标。3,150 点独立 Chromium 命中参考：3,130 精确、20 个边界差异在每轴 2 CSS px 内（总差异数同时限 20）；另有 seed 0xCE802026 的 128 次独立 scroll/z-order mutation 检查。新接口只传拥有值，命中使用绘制顺序、inline 分片和圆角/overflow/viewport clip，不依赖反向 DOM 或 union rect。

Debug/Release 另实际运行 `--cheetah-flow-window-test-hardware`，WARP 由 CTest 运行；四次各 20 帧、3 resize、18 mouse/12 wheel 消息、2,389 texture 实际 query 退休、completed serial 40、38,640 CPU readback 样本；硬件最大 2/WARP 1 色阶，debug layer 零 warning/error。覆盖原始 facility/company focus payload、store 回执、scroll-only repaint、细分/取消/两轴 page/zero policy、负屏幕坐标、入队后 resize 的旧 viewport、滚动后选择/内容收缩、minimize cancel/close。源/二进制/产物 hash 在 `artifacts/local/flow-input-{debug,release,gc-stress}/run-metadata.json`，窗口图片/report 在 `artifacts/local/cheetah-flow-window-*`，浏览器截图在 `artifacts/local/flow-input-reference/frame-2.png`。可见 exe 和简明步骤见 [flow 输入窗口](flow-input-window-probe.md)。只自动验证 own-HWND 注入消息；实体 mouse/wheel、触摸/惯性、完整 PointerEvent/焦点/IME、scrollbar、多显示器 DPI、移动、游戏服务与性能仍未验收，原游戏未修改。以下 flow 和更早数字保留为历史证据。

Flow 阶段最终结果：**Debug 50/50，24.62 秒；Release 50/50，11.18 秒；新增范围、DOM 生命周期及八轮 React 强制 GC 10/10，61.35 秒**。实际命令为 Debug/Release build/ctest presets，GC 为 `ctest --test-dir build/windows-gc-stress -C Debug -R 'flow|unicode|script_dom_lifecycle|page_react' --output-on-failure`。27 个确定 seed mutation 场景：1,860 个几何字段最大差异 1 px，952 个 client fragment 字段最大 0.850128 px，分片/元素数量完全一致；逻辑 1× OpenType Win/typo 字体度量与实际浏览器 DPR 1.005 的量化容差仅用于新 profile，旧 goldens 未放宽。原样 React 八帧：1,336 geometry 最大 0.299126 px、2,672 色分量最大 0.0000003922、8 个 scroll offset 最大 0.348259 px（容差 0.5）。未修改固定原 bundle 或原游戏。

libunibreak 原始数据测试：Unicode line 15.0 / UAX14 rev49 的 7,619/7,619 通过，并按上游 test_skips.h 明确跳过 35 项；grapheme 15.1 / UAX29 rev43 的 1,187/1,187 通过、0 跳过。自己的 CSS 空白/white-space/solidus tailoring、非法 UTF-8、NBSP、br、跨源字素/ligature 拒绝等反例另测。16 轮 scroll/geometry VM reset/GC、detached/epoch/owner/reentry/valueOf 反例通过；没有提升为最新 Unicode 或完整 CSS Text 兼容。

Debug/Release RTX 5090 和 WARP 另实际执行 `--flow-hardware/warp`、`--cheetah-flow-hardware/warp`。flow 四帧有 119,840 个 CPU readback 样本，最大 1 色阶；独立 64×64 椭圆积分的 8,000 texel 最大 5/255；436 textures 实际 host completion 后退休。原样 React 八帧有 417,585 CPU 样本，硬件最大 2/WARP 1 色阶、568 fallback glyph、858 textures 实际 query 退休；全部调试层零 warning/error。C ABI backend v1 不变，圆角只在值快照/准备路径扩展。图片/report 和源/二进制/产物 hash 封存在 `artifacts/local/flow-{debug,release,gc-stress}/`、`artifacts/local/cheetah-flow-*`；浏览器完成截图在 `artifacts/local/cheetah-flow-reference/frame-7.png`，固定 JSON 在 tests/fixtures。范围与复现见 [flow 合同](flow-layout-probe.md)。原游戏仍在修改，固定捕获不自动刷新；本阶段只读 drift 检查保存在 `artifacts/local/flow-debug/source-drift.json`。移动、浏览器像素 parity、完整 UI 和性能均未验收。以下旧数字保留为历史阶段证据。

自动布局阶段最终结果：**Debug 42/42，14.53 秒；Release 42/42，9.03 秒；自动布局及八轮 React GC 6/6，36.71 秒**。实际命令为 Debug/Release build/ctest presets，以及 `ctest --test-dir build/windows-gc-stress -C Debug -R 'auto|page_react' --output-on-failure`。新增 48 个 mutation 场景共 1,984 个几何字段最大差异 0.184082 px；原版 AlertBar 八帧 1,336 个几何字段最大差异 0.258087 px、2,672 个颜色分量最大差异 0.0000003922。Debug/Release RTX 5090 与 WARP 各运行 `--cheetah-auto-hardware` / `--cheetah-auto-warp`，每次 191,203 CPU 图片/coverage 样本，最大硬件 2/WARP 1 色阶、822 texture 实际完成后退休、568 fallback glyph，debug layer 零 warning/error。证据 hash 封存在 `artifacts/local/auto-{debug,release,gc-stress}/run-metadata.json`，原生图片/report 在 `artifacts/local/cheetah-auto-*`，浏览器截图在 `artifacts/local/cheetah-auto-reference/`。只读检查原游戏 57 文件发现 7 个文件与固定样本不同、0 缺失；未刷新捕获或写原游戏，详情在 `artifacts/local/auto-debug/source-drift.json`。范围、硬诊断和复现见 [自动布局合同](automatic-layout-probe.md)。完整 CSS/inline、移动和性能仍未验收。以下旧阶段数字保留为历史证据。

公共样式阶段最终结果：**Debug 37/37，11.55 秒；Release 37/37，7.76 秒；新增范围加八轮 React mount/GC/unmount/reset 的强制 GC 7/7，40.36 秒**。实际命令为 `cmake --build --preset windows-debug --parallel 10` / `ctest --preset windows-debug`，Release 对应 preset，以及 `ctest --test-dir build/windows-gc-stress -C Debug -R 'style|positioned_css|page_react' --output-on-failure`。RTX 5090/WARP 的新 `--cheetah-style-hardware`/`--cheetah-style-warp` 均通过；2,672 个颜色分量最大差异 0.0000003922、1,336 个几何字段最大差异 0.151123 CSS px。GPU 每次 217,065 样本，最大误差硬件 2/WARP 1 色阶，788 textures 实际完成后退休，调试层零 warning/error。独立 CSS 语义三帧 192 个颜色分量也通过。源码/二进制/产物 hash 封存在 `artifacts/local/style-{debug,release,gc-stress}/run-metadata.json`，图片/report 在 `artifacts/local/cheetah-style-*`。完成时只读对照原游戏 57 文件，0 变化/0 缺失。详见 [公共样式合同与复现](style-cascade-probe.md)。该阶段之后的 intrinsic 与受限 block/Flex 合并已在自动布局探针完成；一般 inline、完整原游戏 CSS、移动与性能尚未验收。以下旧阶段的测试数量和产物保留为历史证据。

D3 的同一份未修改脚本在 Chrome 154.0.8037.98 和 Windows 原生 DOM 各通过 **29/29** 检查；报告的 **12 个字段完全一致**。包括节点顺序、身份、柱高、轴更新、文字、path 和事件调用计数。报告明确保留 native_rendering_verified=false。详情见 [前端库验收](frontend-libraries.md)。

Flex 的同一份 JSON 输入分别生成 browser/native fixture，**33/33 场景、452 个字段**在 Debug/Release 与 Chrome 实际测量一致，逻辑尺寸容差 0.025 CSS px、行数无容差。包括 auto min、basis 0px/0%、嵌套/多行、baseline、最终文本、box sizing 与 containing block。受限 profile、原始 24 场景的淘汰基线与 Yoga 补丁见 [Flex/字体探针](layout-text-probes.md)。该结果允许继续验证 Yoga，不能宣布完整 CSS 或 M0 退出。

D3D11 的 RTX 5090 硬件与 WARP 在 Debug/Release 各通过 **121 项断言**，调试层启用且 warning/error 为零。十个颜色/coverage/clip 像素、透明目标的 premultiplied RGBA、真实提交/取消/失败重试/CPU 提前释放/完成退休和宿主 viewport/scissor 恢复均通过。Direct/RTT 全图最大差异为硬件 1、WARP 0 个 8-bit 色阶。实际 Chrome 截图另保存十个参考像素；50% 白叠黑为 browser 128、native 约 188，符合声明的颜色差异。见 [GPU 探针](gpu-probe.md)。

单通道 SDF 的 CPU 编码/padding/度量/空格与非法输入反例通过。GPU 用同一份 40 ppem field 绘制三个倍率，直边 7,811 个独立 area-coverage 样本的 linear 最大误差为硬件 0.056129、WARP 0.063315（阈值 0.12）；蓝色描边为 0.034307，direct/RTT 全图差异均为 0，实际完成后退休两个 atlas，调试层 warning/error 为零。GPU 每次 55,545 项断言包含大量逐像素检查，未做完整尖角/曲线差分。见 [SDF 实际合同](sdf-font-probe.md)。

真实 React/Vite 页面的初始、计数变为 1、列表反转三帧在 Debug/Release 硬件与 WARP 通过，且 eight-cycle mount/GC/unmount/reset 在强制回收下通过。实际 Chromium 同源 JSX 的 300 项几何字段最大差异 0.075882 CSS px（阈值 0.1，采集 DPR 1.005），React 状态一致。原生 direct/RTT 最大差异硬件 1、WARP 0 色阶；与浏览器逐像素一致未验证。页面采用灰度文字，SDF 页面材质尚未接入，详见 [React 实际合同和复现](react-vite-probe.md)。

新增窗口测试在自己的隐藏 HWND 上发送消息，经过 owner 队列、最新布局命中与 React，再实际 Present/readback。Debug/Release 硬件与 WARP 各完成 7 帧、3 次 resize、14 条 mouse message、605 个 texture 的真实 query 退休；最终 Count: 2、列表 moss/tide/ember。提交后宿主失败的 drain 反例通过，调试层 warning/error 为零。强制 GC 的同一路径也通过；physical_mouse_verified=false，实体鼠标与多显示器 DPI 尚未验收。运行与可见样例见 [窗口说明](native-window-probe.md)。

M0 selector 暂时支持 tag/id/class compound 和 *，其他 selector 显式诊断。childNodes/querySelectorAll 暂为快照数组；带 prefix 的 createElementNS、listener handleEvent 对象等尚未实现。Event/CustomEvent 是最小 bootstrap 语义；queue_callback 只接受 owner 调用。VM 配额不包含原生 DOM 字节，没有实现跨线程 completion queue，不能从 fixture 推断标准 DOM 完整兼容。

## 实际验证

此前 CaseCheetah NineSlice workload 的三帧在硬件/WARP 通过，原游戏保持只读。44 个元素的 528 个浏览器几何字段最大差异 0.123840 CSS px（阈值 0.2，实际 DPR 1.005），独立 CPU 图片/coverage 样本 6612 项最大差异 1 色阶，211 个 texture 等实际 completion query 退休且 debug layer warning/error 为零。该 NineSlice v1 报告使用英文标签，中文/完整游戏/browser pixel parity 为 false；本轮中文证据在下面单独记录，见 [组件与复现](casecheetah-workload.md)。

后续 AlertBar 中文阶段在 Debug/Release 硬件/WARP 输出八帧及一帧字体缓存碰撞反例：568 个 fallback glyph、217,065 个 CPU image/coverage 样本最大差异 1，788 texture 实际 query 退休且 debug layer warning/error 为零。卸载/reset/GC 后值快照仍可绘制，卸载的列表 target 被拒绝。独立浏览器的状态/两个 focus payload/UE 回执一致，1,336 几何字段最大差异 0.151123（阈值 0.2），四个中英文本 advance 差异 0.013681（阈值 0.1）、基线差异 0.441648（主字体浮点 strut vs 浏览器量化，声明阈值 0.5）。中文灰度为 true，CJK SDF、完整实际游戏 CSS、真实 UE 命令、完整 UI、浏览器像素 parity 与移动仍未验证，见 [完整合同](cjk-alert-probe.md)。

| 配置 | 当前结果 |
| --- | --- |
| Windows MSVC Debug | Flow 输入窗口最终全量 54/54 通过，85.76 秒；日志与 sealed metadata 在 artifacts/local/flow-input-debug/ |
| Windows MSVC Release | Flow 输入窗口最终全量 54/54 通过，15.26 秒；日志与 sealed metadata 在 artifacts/local/flow-input-release/ |
| Windows Debug，QuickJS 每次分配前强制 GC | Flow 输入/browser、HWND wheel、旧 mouse input、DOM 生命周期和八轮 React mount/update/unmount/reset 7/7 通过，106.84 秒；未重复全量 54 项 GC。历史 flow/Unicode 10/10 见下方记录；仅 React workload 放宽 instrumentation 时间预算 |
| 依赖逐文件校验 | 5,106 个固定文件通过 |
| 浏览器/native D3 对照 | 29 项检查、12 字段一致 |
| 浏览器/native Flex 对照 | 33 场景、452 字段一致；固定浏览器参考保存在源码中 |
| 真实 font probe | metrics、glyph/cluster/advance、scale 切换、coverage、缺字与 owner 检查通过 |
| D3D11 WARP / 硬件 | 两配置均各 121 项通过；实际 query retirement、readback 与颜色合同通过 |
| SDF WARP / 硬件 | 两配置均通过距离材质、缩放/描边 oracle、真实退休、全图 direct/RTT parity 与零 warning/error |
| React WARP / 硬件 | Debug/Release 三帧各 1,142 断言、522 glyph texture 实际退休、调试层零 warning/error；300 项 browser 几何比较通过 |
| 原生窗口 WARP / 硬件 | Debug/Release own-HWND 消息、resize/letterbox/capture cancel/close、7 次 Present 与 605 texture 退休通过；提交后失败实际 drain |
| backend API WARP / 硬件 | 宿主独立 context/RTV、颜色/coverage/SDF 与旧 reference 全图差异 0、float target `[0,0.5,0,0.5]`、宿主内容保留、取消/录制失败和 query 退休；每会话四帧/四 texture、零 warning/error |

行为压力包括 1,536 组原生/JS 循环图、1,200 次树 mutation（seed 0x51a37），以及资源合同的 13 组行为测试和 2,000 次生命周期循环（seed 0xCE102026）。它们验证正确性，没有测量产品 CPU/GPU/内存预算。

独立代码复查发现并增加反例：属性 value 的 toString() 重入 DOM 导致 vector iterator 失效、事件 options getter 的异常传播、VM OOM 被 JS 捕获，以及 Promise 吞掉超时中断后的故障状态。VM 超限保持 faulted，必须 reset；C++ 异常不跨 VM 的 C 回调边界。默认 VM 栈 **512 KiB**，React 实验改为 **1 MiB** 并为三个 Windows exe 保留 **8 MiB** 宿主栈；普通执行预算仍为 2 秒。强制 GC 原先触发 React 的 2 秒 deadline，现仅 React page/input/GPU workload 使用 30 秒预算/180 秒 CTest timeout，故障与修正日志都保留。不能据此声称任意线程栈安全或性能已验收。

## 本机与复现命令

| 项目 | 实际环境 |
| --- | --- |
| 系统 / 工作目录 | Windows x64、PowerShell；F:\Projects\CaseElephant |
| CMake / 工具链 | CMake 3.27.6；Visual Studio 2022 Community，MSVC 19.42.34444.0 |
| SDK | Windows SDK 10.0.26100.0 |
| GPU / 驱动 | 实际探针选择 NVIDIA GeForce RTX 5090，32.0.16.1062；另验证 WARP，均请求 feature level 11_0 |
| Python | C:\Python312\python.exe |
| CTest Python | CMake 当前选择 C:\Users\JiahaoLi\AppData\Local\Programs\Python\Python313\python.exe，3.13.7 |
| React 构建工具 | Node 24.19.0、npm 10.8.2、Vite 8.3.3；普通 CMake/CTest 无需 Node |
| Git | 已初始化 main；尚无 commit、remote 或外部 CI |
| Android / iOS | 当前没有已验证的 SDK/NDK、macOS/Xcode 或连接手机 |

~~~powershell
cmake --preset windows-msvc
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
cmake --build --preset windows-release --parallel 4
ctest --preset windows-release
python tools/verify_dependencies.py
~~~

强制 GC 的独立配置、输出目录与完整解释见 [building](building.md)。核心/探针程序位于 build/windows-msvc/bin/Debug 或 Release。CTest 日志位于各 build 目录的 Testing/Temporary。

| 产物 | 位置 |
| --- | --- |
| 浏览器参考报告/DOM/运行元数据 | artifacts/local/d3-reference/ |
| 原生 Debug 报告/DOM/浏览器比较 | artifacts/local/d3-native-debug/ |
| 原生 Release / 强制 GC 报告与比较 | artifacts/local/d3-native-release/、d3-native-gc-stress/ |
| 固定共享脚本与参考页面 | tests/fixtures/d3_probe.js、d3_chart_reference.html |
| Flex 唯一输入/生成文件/浏览器参考 | tests/fixtures/flex_layout_cases.json、.hpp、.js；flex_browser_reference.json |
| Flex 浏览器实际 DOM 捕获 | artifacts/local/flex-reference/browser-dom.html |
| Flex Debug/Release 报告与本轮测试日志 | artifacts/local/flex-native-debug/、flex-native-release/ |
| CTest 自动 Flex 报告 | build/windows-msvc/artifacts/flex-Debug/、flex-Release/ |
| GPU 硬件报告/readback PNG/CTest 日志 | artifacts/local/gpu-debug/、gpu-release/ |
| SDF 硬件报告/readback PNG/本轮 CTest 日志/元数据 | artifacts/local/sdf-debug/、sdf-release/ |
| CTest 自动 SDF WARP 报告/readback PNG | build/windows-msvc/artifacts/sdf-Debug/warp/、sdf-Release/warp/ |
| CTest 自动 WARP 报告/readback PNG | build/windows-msvc/artifacts/gpu-Debug/warp/、gpu-Release/warp/ |
| Flow 输入/window 当前测试/hash 证据 | artifacts/local/flow-input-{debug,release,gc-stress}/、cheetah-flow-window-* |
| 4K 滚动阶段计时/缓存差分/历史测试/hash | artifacts/local/flow-scroll-perf/；baseline、optimized（含 previous-run）、render-cache-hardware、4k-hardware、debug/release/gc 与 run-metadata.json |
| 当前连续运动、截图、采样与生命周期/hash | artifacts/local/motion/；hardware、debug/release/gc、interactive、samples.csv、profile.json、run-metadata.json |
| 两帧宿主、同步控制、吞吐和生命周期/hash | artifacts/local/two-frame/；sequential、pipelined、hardware、4k-hardware、debug/release/gc、baseline 和 run-metadata.json |
| Flow 命中 browser/source/font oracle 和截图 | tests/fixtures/flow_input_browser_reference.json、artifacts/local/flow-input-reference/ |
| 固定 browser 颜色参考 | tests/fixtures/gpu_color_browser_reference.json、.png、.dom.html；采集源为 gpu_color_reference.html |
| React JSX / native IIFE / 固定 browser golden | examples/react-vite/src/main.jsx、native/；tests/fixtures/react_browser_reference.json |
| 主鼠标窗口/GPU message 测试与会话 | artifacts/local/window-debug/、window-release/、window-gc-stress/、window-interactive/ |
| 新 backend API、迁移后窗口/CTest/hash 证据 | artifacts/local/backend-debug/、backend-release/、backend-gc-stress/ |
| CaseCheetah 固定样本/组件/browser golden | tests/fixtures/casecheetah/、examples/cheetah-nine-slice/、tests/fixtures/cheetah_browser_reference.json |
| CaseCheetah 像素/几何/CTest/GC 证据 | artifacts/local/cheetah-debug/、cheetah-release/、cheetah-gc-stress/、cheetah-reference/ |
| 中文字体/AlertBar 原样组件/bundle/browser golden | third_party/noto-cjk/、third_party/zustand/、examples/cheetah-alert/、tests/fixtures/cheetah_alert_browser_reference.json |
| 中文/AlertBar 像素/几何/CTest/GC/hash 证据 | artifacts/local/cheetah-alert-debug/、cheetah-alert-release/、cheetah-alert-gc-stress/、cheetah-alert-reference/ |
| React browser 与 native GPU/GC 证据 | artifacts/local/react-reference/、react-debug/、react-release/、react-gc-stress/ |
| 依赖 lock | third_party/manifest.json |

生成产物被 Git 忽略，固定源码与 fixture 随项目保存。构建不会再次下载依赖。Yoga 有一个明确记录的源码补丁；QuickJS、FreeType、HarfBuzz 和 D3 源码未修改。上游 QuickJS 的 MSVC 编译存在警告，未宣称完整无警告构建。Yoga/FreeType/HarfBuzz 的 MSVC target 指定 /utf-8，避免本机代码页 936 把 Unicode 注释读成 C4819；没有改写 vendored 文件编码。

仓库文档的 UTF-8、本地链接与 code fence 已检查；构建步骤和当前能力已与源码、实际报告核对。

## 平台证据

| 平台 | 配置 | 编译 | 链接 | 模拟器 | 设备功能 | 性能 |
| --- | --- | --- | --- | --- | --- | --- |
| Windows x64 | 已验证 | 核心与探针已验证 | 核心与探针已验证 | 不适用 | DOM/Flex/字体/合同、D3D11 readback 与受限 React 页面像素通过；受限 HWND/swapchain mouse/wheel 消息通过；实体鼠标/滚轮、完整焦点/键盘/IME 未自动验收 | 固定 4K/120 帧同步与两帧滚动全完成吞吐已测；实体长时间/产品预算未验收 |
| Android arm64 | 未验证 | 未验证 | 未验证 | 未验证 | 未验证 | 未测量 |
| iOS arm64 | 未验证 | 未验证 | 未验证 | 未验证 | 未验证 | 未测量 |

## 下一阶段

继续 M0：两帧宿主、真实完成退休与连续 DOM 运动演示已落地，接下来降低静态快照检查、仅位置变化的 style/layout、scroll-only 几何和冷加载开销，验证实体输入延迟与游戏宿主集成。4K/FPS 和固定字体/图片有界缓存已落地，生产失效调度、跨 API 多帧后端、纹理格式/动态 atlas 仍待实现。随后扩大 CaseCheetah 固定 workload 的实际 CSS、左右档案/视图条及 D3 SVG 像素，继续准备移动工具链与触摸输入。完整 UI 仍需 HTML 文档加载、更多 inline/font/scroll 语义、焦点/键盘/IME 和实际宿主数据服务。Vulkan/原生 Metal 后续各接同一 backend API，不要求经 MoltenVK；移动编译、GPU 和中文输入依然是必需验收。

每次新增实现都按 [阶段计划](implementation-plan.md) 保留反例和平台证据；不会把设计目标直接写成支持能力。
