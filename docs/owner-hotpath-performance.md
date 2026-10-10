# Owner 调度、DOM 版本与数值绘制参数

2026-10-09。完成前两项优化在固定 gallery scene 的接入：减少无任务/无 DOM 变化时的工作，并让热动画属性直接进入 GPU paint，复用不变的绘制参数。保持原始 34 个函数、React/D3 包、bundle、更新频率、72 目录 / 36 实绘 / 36 诊断、4K/FPS 与两帧完成退休。此前证据见 [CSS 中段阶段](css-middle-performance.md)，本轮独立证据在 `artifacts/local/owner-hotpath/`。

## 运行

在 `F:\Projects\CaseElephant`：

```powershell
cmake --build --preset windows-release --parallel 8
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/owner-hotpath/interactive
```

默认 CSS，可滚动到 Spinner、Flip、Ripple、Pulse 和组合动画；右上角继续显示 FPS、CPU 分位数与当前 LIVE 数量。窗口没有设置 affinity 或进程优先级。关闭后除原 `preview-session.json`，增加 `owner-work.json`，记录快照复用、JS eval/直接宿主调用、mutation、数值采样和绘制参数缓存计数。

## 调度与 DOM 失效

`ScriptDom::pump_tasks` 与 `animation_frame` 在同一 UI owner 上直接 `JS_Call` 已安装平台模块的函数，传入原始 double 时间，省去逐次生成、解析 JS 调度表达式和计数字符串。函数与对象的 JSValue 仅存在该 VM 内，reset/shutdown 前释放，不进入 scene 或 renderer；原 execution deadline、VM quota、rAF 每回调微任务检查点、timer 批次检查点、错误与隐藏恢复语义保留。timer 模块缓存最早到期时间，尚无到期任务时不创建 ready 数组或扫描整个 task map。新建的到期任务属于下一批；一批仍最多 1,000 回调，pending 容量 4,096，超过下一批容量仍按原语义拒绝。

native DOM 给实际 snapshot mutation 分配单调 revision，沿当前父链更新 subtree revision。树移动同时使旧、新父链失效；属性、id、character data、textContent、scroll setters、几何 clamp 和 wheel 默认滚动覆盖。属性同值写入/删除不存在属性可以复用；保守的 id/textContent 写入可能多报失效。固定 1,024 项 journal 只保存带 epoch/slot/generation 的弱身份，不能保活 detached 节点；返回时排除已失效身份，历史溢出要求全量快照。cursor 必须与 epoch 一起保存，reset 后丢弃；它不是跨 View 的 revision。

固定 `GallerySceneSession` 使用稳定 mount root 的 O(1) subtree revision，未变时复用自己拥有的 DOM 快照，取消每帧复制后再整树比较。变更仍复制完整 root，没有实现一般增量 DOM patch。静态无变化时跳过 scene，viewport 尺寸变化仍重新生成布局；CSS 动画继续每帧推进。D3 离屏 timer/rAF 继续运行，可见性和输入行为不改变。同步 geometry read 仍走 fresh owner layout，measure 不回调 JS。场景错误审计仍有一次 eval，shell/Canvas 的旧 pump 表达式也保留，未宣称整页零 eval。

## 数值动画与绘制参数复用

scene 在 target 变化时编译 opacity、2D transform、背景色、文字色和四边颜色为 API 无关的自有数值。transition/keyframe 端点按参考 numeric-token/单位兼容规则解码并有界缓存，采样数值直接供 Windows GPU paint 使用，currentcolor 跟随当前颜色。`none` 的受限 rotate/scale 处理、离散 list/单位、不支持值的旧字符串 fallback 保留；HSL 动画沿原参考字符串路径，不扩大兼容范围。

GPU painter 的每节点参数程序保存静态样式、尺寸、圆角形状、边框宽度/样式、transform origin、clip 标志与稳定 z 排序的子节点下标。热数值改变时复用；字体/文本、静态样式、尺寸、子节点身份/顺序/z 变化重建。程序不保存借用 Node 指针，map 用 epoch/slot/generation；最多 256 项和 2 MiB accounted key/program bytes，factory 变化清空。递归过程中缓存可以清空，但当前 replay 持有自己的程序，不能被 eviction 悬空。几何归所属 factory，设备 bitmap 的既有 domain 与 submission serial 合同保留。

group opacity、overflow 和透明绘制顺序仍完整执行，未分摊透明度、裁掉越界后代或自动提升纹理图层。CPU WIC/D2D 全量参考忽略数值 cache，仍解析原 style、执行原 mask 和子节点排序；scene 关闭增量时仍全量 cascade/layout/采样。scene 的逻辑 equality 排除派生 paint cache，数值正确性另外用实际 GPU/software 图像对照验证。

**仍保留诊断/参考 style 字符串，每帧仍生成完整拥有值的 paint scene；字符串格式化、继承色值和 geometry transform 解析尚未全部消除。** 这轮减少 GPU 适配器的反复解析和静态参数重建，不是生产 retained display list、后台 UI 线程或多线程 renderer。C ABI 与公开 renderer/frame/backend v1 不变。

## 验证与测量

Windows 11 26200 / Ryzen 9 9950X3D / RTX 5090 driver 32.0.16.1062 / MSVC 19.42 C++20 Release + D3D11 debug layer；3840×2160 surface、1920×1080 shell、CSS body 387×288 / 2x、34 VM / 560 mount 节点，固定原字体/图片，scene body 使用本机 DirectWrite system-ui/Arial/Consolas/Emoji。旧版从上一阶段 642 个封存源文件重新构建到独立 source/build，`WEFT_SOURCE_DIR` 指向自己的旧 bootstrap，避免旧 executable 误读当前新平台脚本；固定依赖未改。原封存 executable 同时保留，实际测量的是 isolated rebuild。

240 帧 / 16 预热 / 224 CPU、GPU、Present interval 样本，固定 60Hz owner 时间，无截图回读，最后 drain 真实 query。每场景三组，round 1/3 before→after，round 2 after→before；构建/测试停止后串行测量。默认 paired 与 controlled 分开记录，controlled 两版子进程都在启动阶段设置 affinity 0xFFFF（逻辑 CPU 0–15）/ High priority，读回保存；窗口没有设置。表为 run 吞吐与各 run 分位数的中位数，不是混合帧分位数；每行最慢帧是三个 run 的 warm max。

| 条件 / 场景 | before FPS 中位数（范围） | after FPS 中位数（范围） | 比值 | CPU P50 / P95 / P99 ms（before → after） | 最慢 warm 帧 ms |
| --- | ---: | ---: | ---: | --- | ---: |
| 默认 / 中段 16 body | 85.63（85.12–105.63） | 116.21（113.49–125.65） | 1.357x | 11.197 / 13.901 / 14.544 → 8.364 / 11.515 / 12.439 | 17.080 → 13.539 |
| 默认 / 末段 10 body | 116.65（108.50–142.43） | 148.18（146.95–187.32） | 1.270x | 8.182 / 10.733 / 11.147 → 6.617 / 8.742 / 9.620 | 14.314 → 10.599 |
| 相同 affinity / 中段 16 body | 140.74（135.06–164.87） | 163.04（157.12–189.77） | 1.158x | 6.724 / 8.673 / 9.151 → 5.738 / 7.643 / 8.078 | 10.001 → 10.125 |
| 相同 affinity / 末段 10 body | 178.25（177.79–178.96） | 208.24（205.47–212.21） | 1.168x | 5.356 / 6.332 / 6.837 → 4.570 / 5.461 / 5.906 | 7.165 → 6.443 |

本轮每组中段/末段 after 均高于对应 before。默认调度仍有明显波动；隐藏固定短场景吞吐不等于实体窗口长期 FPS 或游戏 UI 的独立成本，也没有同负载 Ultralight 对照。控制条件下中段 CPU P50 **6.724 → 5.738 ms**，仍未达到先前讨论的 P50≤2ms/P99≤3ms 目标。

相同 affinity 中段 JS / snapshot P50 **1.071 / 0.136 → 0.582 / 0.025 ms**，末段 **0.781 / 0.111 → 0.492 / 0.023 ms**，收益主要来自第一项。第二项的数值与静态程序确实被复用，但本轮 scene P50 中段 **0.963 → 1.004**、末段 **0.788 → 0.831 ms**，raster 只从 **2.315 → 2.269**、**1.434 → 1.371 ms**；不能宣称 typed paint 已大幅降低整体 scene/绘制开销。完整诊断字符串和 owned scene 的成本仍在，阶段分位数也不能相加。

一个 controlled 中段 run 记录 **419 次 root snapshot / 5,375 次复用**；其实际数值样本 6,566，程序 build/reuse 898/12,205，typed draw 44,855。计数含构造与全部 240 帧，34 VM 的错误审计仍有 eval；新 `owner-work.json` 不把复用当作增加案例通过数。

| 条件 / 场景 / 版本 | GPU producer+UI elapsed P50 / P95 / P99 / 全 run max ms | peak working set / close private 峰值 MiB | 启动范围 ms |
| --- | --- | --- | --- |
| 默认 / 中段 / before | 6.662 / 7.976 / 8.465 / 10.421 | 237.301 / 1020.707 | 3549.274–4498.885 |
| 默认 / 中段 / after | 5.117 / 6.694 / 7.182 / 8.250 | 240.082 / 1017.730 | 3562.974–3881.205 |
| 默认 / 末段 / before | 0.402 / 6.932 / 7.610 / 9.511 | 229.441 / 926.523 | 3463.586–3820.293 |
| 默认 / 末段 / after | 0.400 / 5.922 / 6.591 / 8.100 | 232.281 / 927.211 | 3527.772–3622.531 |
| 相同 affinity / 中段 / before | 4.204 / 5.238 / 5.794 / 6.889 | 237.254 / 1018.859 | 3353.559–3567.039 |
| 相同 affinity / 中段 / after | 3.717 / 4.577 / 5.169 / 7.353 | 239.695 / 1020.348 | 3524.162–3549.960 |
| 相同 affinity / 末段 / before | 0.403 / 4.553 / 5.015 / 5.555 | 229.941 / 924.062 | 3519.915–3548.707 |
| 相同 affinity / 末段 / after | 0.402 / 4.193 / 4.567 / 4.871 | 231.523 / 925.270 | 3379.390–3570.083 |

GPU query 包含 CPU producer/提交空隙，排除 HUD/Present，不是 GPU busy time。进程内存不等于 VRAM，renderer cache bytes 不包含全部 D2D geometry/card backing。启动含 72 原例诊断观察；总 VRAM、冷首帧尖峰、长时尾部和实际 CPU core/frequency/scheduler 遥测未测。默认首屏/D3 仅一组 sanity：130.85→185.43 / 151.49→220.73 FPS，不据此作稳定性结论或 D3 专用优化宣传。

实际构建与全量检查：

```powershell
cmake --build build/windows-msvc --config Release --parallel 8
ctest --test-dir build/windows-msvc -C Release --parallel 6 --output-on-failure
cmake --build build/windows-msvc --config Debug --parallel 8
ctest --test-dir build/windows-msvc -C Debug --parallel 6 --output-on-failure
```

两配置全量 **76/76，Release 27.65 秒 / Debug 62.67 秒**。最后 quota/module/owner 边界修正后重建，两配置相关 DOM / owner / rAF / scene / 原 scene / geometry / GPU / window **8/8，27.42 / 57.59 秒**；两版又分别运行 hardware complex / 原 scene GPU / gallery window。WARP 来自 CTest。构建/正确性验证有独立 GC build tree 并行，检查时间不作为性能指标。

新增 `owner_hotpath_contract` 与封存旧平台模块做 **320 次 timer/rAF/微任务/取消/隐藏/精确时间差分**；512 次随机 mutation 的 revision-retained snapshot 与 full snapshot 相等，另验证属性 no-op、namespace revision（既有 snapshot 仍拒绝 namespace 属性）、scroll/wheel、journal overflow、weak GC、slot/epoch、missing-module 恢复、跨 owner、timer quota/error/deadline 与 12 次 reset。新静态 scene 反例验证 idle 不复制 root、viewport 改变仍 fresh layout。此前动画计划、中断继承、几何/图像与 detached 生命周期继续通过。

复杂 GPU 对照共 **316 次**：184 原 projective/border/angle-only、128 次 owner typed transition/关键帧/不同单位/transform list/matrix/弧度/中断/currentcolor/不等宽/尺寸/裁剪/z/移除恢复，与关闭增量的 full scene 和软件图比较；另 4 次 300 子节点、排序/代际/递归缓存清空。RTX hardware worst mean / min IoU **0.720556 / 0.958484**，4 次 program eviction、1,468 build / 894 reuse、5,456 typed draw，316 import 全实际退休；24 次 angle-only upload 2，8 次完成后 software/GPU target 切换。阈值沿用 mean≤3 / IoU≥0.75，调试层零 warning/error。

原 34 例 **102 张 Chrome 图 / 1,476 几何字段**、4,117 GPU submission/retirement 保持，hardware mean / IoU 2.39076 / 0.757239。4K 窗口 160 帧 / 288 几何 / 17 cached-full packet / 1,680 像素最大差 1、悬停/点击/最小化恢复/FPS 与真实两帧退休通过；本轮人工检视 CSS 中段截图，实体设备操作仍未人工验收。

强制 GC 构建五个相关 target，并执行 DOM / owner / rAF / scene / complex / external texture / two-frame / cache **8/8，46.86 秒**；最后边界修正后同范围再次 **8/8，44.22 秒**。没有重跑 GC 大型原 gallery/geometry/window，旧阶段证据继续保留。命令可复现：

```powershell
cmake --build build/windows-gc-stress --config Debug --parallel 8 --target weft_owner_hotpath_tests weft_script_dom_tests weft_animation_frame_tests weft_scene_contract_tests weft_d3d11_probe
ctest --test-dir build/windows-gc-stress -C Debug --parallel 1 -R '^owner_hotpath_contract$|^script_dom_lifecycle$|^animation_frame_contract$|^scene_contract$|^d3d11_warp_complex_css$|^d3d11_warp_external_texture$|^d3d11_warp_two_frame$|^d3d11_warp_render_cache$' --output-on-failure
```

所有失败/探索日志保留：初次 native JSValue const 参数编译修正、timer lexical ready 重名、测试误把固定 Document root 计为零和误对 namespace 做 snapshot 已纠正；未改变既有兼容限制或图像阈值。容量 suite 前初次全量 76/76 单独保存。源/二进制/图片/24 个主要 run 与四个 sanity run 由 `tools/record_owner_hotpath_evidence.py` 封存到 `run-metadata.json`；逐项核验上一阶段 seal，不改写历史证据。复跑基准使用 `tools/benchmark_owner_hotpath.ps1`，已存在同名 benchmark 会拒绝覆盖。


Windows 固定 scene 范围之外，移动真机、Metal/Vulkan、device-loss、完整 72 实绘、完整 HTML/CSS/3D、长时游戏 UI 与公平 Ultralight 对照仍未验证。后续应进一步减少完整 scene/诊断字符串和 producer 提交成本，按毫秒预算验收，不能只用 UI 独占时的 FPS。
