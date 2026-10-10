# CSS 中段边框与 scene 更新优化

2026-10-09。针对原 gallery CSS 中段继续优化，D3 没有新增专用优化。保持原始函数、包、bundle、每帧时钟和更新频率；72 目录 / 36 实绘 / 36 诊断、4K/FPS 与两帧在途不变。前一阶段证据留在 [复杂 CSS 阶段](complex-css-performance.md)，本轮独立 seal 为 `artifacts/local/css-middle-performance/run-metadata.json`。

## 运行

在 `F:\Projects\CaseElephant`：

```powershell
cmake --build --preset windows-release --parallel 8
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/css-middle-performance/interactive

# 同一固定场景，无截图回读；默认调度，结果不能与下面的 affinity 测量直接混比。
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-benchmark-hardware artifacts/local/css-middle-performance/manual/css-middle css-middle
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-benchmark-hardware artifacts/local/css-middle-performance/manual/css-end css-end
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --complex-css-test-hardware artifacts/local/css-middle-performance/manual/reference
```

默认 CSS，滚动查看 Spinner、Ripple Effect、Pulse Ring 与末段组合动画。窗口本身没有设置进程 affinity 或优先级；下面的控制条件仅用于子进程基准。

## 实现与边界

Windows GPU border 路径将参考的 `shape stroke(width*2) ∩ outer shape ∩ side wedge` 预编译为 filled path geometry，之后只更新颜色和当前 transform。四边同宽同色 solid 仍合成一个环，异色/不等宽/禁用单边使用各自的完整区域与原顺序；消除每帧 border/side mask 的 PushLayer/PopLayer。缓存完整宽高、四角半径、四边宽度、描边宽度及 side index，最多 256 项，factory 改变清理；不按身份复用过时几何。使用 0.01 局部单位 flattening tolerance；当前二维矩阵两列长度之和超过 16 时保留旧 stroke/mask 路径。软件适配器继续原全量 mask 路径，GPU 透明背景零覆盖绘制直接略过。

**父子重叠的 group opacity 与 overflow clip 保持整体合成。** 没有分摊透明度，也没有按节点 border box 裁去越界子节点，没有自动图层提升。几何缓存保存 factory 资源，不访问 Node/JSValue；宿主卡片 backing、完整 packet 和 external texture 的真实 submission completion / 退休合同不变。公共 C ABI 不变，未增加一般 CSS、完整 3D 或移动支持。

owner 将已计算样式的既有 shorthand 展开移到 cascade 失效时，继续在每帧用当前父样式补继承值、采样 transition/animation。基础 target 从 State 的 owned 值读取，少复制一份 map。previous frame 只作保守布局比较与 box/element 的参考；布局复用时不再深复制整棵新 paint scene。returned frame 仍完整、独立拥有当前全部值；布局属性、身份、文本、containing block 或 viewport 变化继续重建参考。关闭增量的全量路径仍逐帧展开样式和重新布局。

## 测量

Windows 11 26200 / Ryzen 9 9950X3D / RTX 5090 driver 32.0.16.1062 / MSVC 19.42 C++20，Release + D3D11 debug layer，3840×2160 surface、1920×1080 shell，CSS body 387×288 / 2x，原始字体、图片、560 mount 节点 / 34 VM。旧版是前一阶段封存的原样 executable 与完整源快照。

每个场景 240 帧、16 帧预热、224 个固定 60Hz owner 时间样本，最后 drain 真实 GPU query。测试顺序每个场景 round 1/3 before→after，round 2 after→before，不并行构建/测试，不截图回读。所有初测、sanity 和重复样本保留。表中为三个 run 吞吐的中位数和各 run 分位数的中位数，**不是混合帧的分位数**。

默认调度出现明显波动，包括未改动的 JS/snapshot/encode 阶段一起变化；因此另外对同样的两版子进程设置相同 **affinity 0xFFFF（逻辑 CPU 0–15）和 High priority**，在 72 例启动阶段完成设置，保存实际读回值。没有记录实际 core/frequency/scheduler 遥测，不能确定默认波动的原因。

| 条件 / 场景 | before FPS 中位数（范围） | after FPS 中位数（范围） | 中位数比值 | CPU P50 中位数 ms |
| --- | ---: | ---: | ---: | ---: |
| 默认 / 中段 16 body | 83.66（53.61–91.91） | 79.08（71.50–98.14） | 0.945x | 11.042 → 12.110 |
| 默认 / 末段 10 body | 89.16（88.26–100.04） | 109.63（93.64–111.15） | 1.230x | 10.688 → 9.004 |
| 相同 affinity / 中段 16 body | 117.91（95.80–121.51） | 136.37（125.15–138.22） | 1.157x | 8.055 → 6.860 |
| 相同 affinity / 末段 10 body | 153.07（148.54–155.28） | 176.64（173.64–178.77） | 1.154x | 6.215 → 5.449 |

控制条件下各组 after 都高于对应 before；默认中段中位吞吐下降，不能据单卡改善或控制结果宣称默认整页稳定提速，也不能排除调度条件之外的退化。隐藏完成吞吐不等于可见窗口长期 FPS，没有同负载 Ultralight 对照。

控制条件的中段 scene/raster P50 中位数 1.207 / 2.908 → 0.961 / 2.362 ms；波纹单卡 0.291 → 0.157、脉冲圆环 0.418 → 0.191 ms。默认中段脉冲圆环也为 0.528 → 0.261 ms，但整页的其他阶段变慢。末段控制条件 scene/raster 0.905 / 2.116 → 0.787 / 1.445 ms。阶段分位数不可相加。

| 条件 / 场景 / 版本 | CPU P95 / P99 中位数 / 全 run 最大帧 ms | GPU elapsed P50 / P95 / P99 中位数 / 全 run max ms | working set / close private 峰值 MiB | 启动范围 ms |
| --- | --- | --- | --- | --- |
| 默认 / 中段 / before | 15.574 / 21.070 / 51.039 | 7.092 / 8.961 / 10.947 / 36.861 | 237.582 / 1079.695 | 3639.353–5029.217 |
| 默认 / 中段 / after | 14.784 / 16.169 / 18.706 | 7.115 / 8.523 / 9.423 / 10.298 | 236.438 / 1018.215 | 3744.630–4366.030 |
| 默认 / 末段 / before | 12.906 / 13.924 / 68.086 | 0.559 / 8.727 / 9.304 / 10.404 | 230.367 / 975.848 | 3652.553–3907.194 |
| 默认 / 末段 / after | 11.593 / 12.800 / 24.674 | 0.411 / 7.742 / 8.273 / 9.288 | 229.691 / 923.090 | 3739.640–3807.411 |
| 相同 affinity / 中段 / before | 10.790 / 11.968 / 18.009 | 5.239 / 6.523 / 7.144 / 10.561 | 236.570 / 1089.355 | 3384.920–3729.051 |
| 相同 affinity / 中段 / after | 9.032 / 10.202 / 12.672 | 4.309 / 5.466 / 6.533 / 7.352 | 236.391 / 1019.676 | 3486.157–3660.963 |
| 相同 affinity / 末段 / before | 7.859 / 8.624 / 10.046 | 0.539 / 6.072 / 6.933 / 7.566 | 229.812 / 981.363 | 3350.248–3356.407 |
| 相同 affinity / 末段 / after | 6.289 / 6.804 / 7.678 | 0.397 / 4.543 / 4.856 / 5.269 | 229.492 / 925.379 | 3514.126–3650.013 |

GPU query 是相同 producer+UI elapsed 范围，包含 CPU 提交调度空隙，排除 HUD/Present GPU 工作，不是纯 GPU busy time。内存列不等于 VRAM；renderer 的 cache bytes 不包含 D2D geometry/card backing/plane bitmap。启动包含 72 原例观察；总 VRAM、冷首帧尖峰、长时尾部和实际 CPU 频率未测。默认首屏仅一组 sanity 170.02 → 163.66、D3 130.63 → 145.38，不能作稳定性结论；没有做 D3 专用改动。

## 验证与证据

实际执行两组 `cmake --build --preset windows-{release,debug} --parallel 8` 和对应 `ctest --preset windows-{release,debug} --parallel 6 --output-on-failure`：**Release 75/75，26.82 秒；Debug 75/75，63.75 秒**。Debug 正确性检查期间另一个独立 build tree 构建/运行 GC，时间不作为性能数据。两配置均另跑 hardware 的 `--complex-css-test-hardware`、`--scene-gpu-test-hardware` 与 `--gallery-window-test-hardware`；WARP 来自 CTest。

新增 owner 64 帧反例将长时间复用的布局参考与 full scene / element geometry / software pixels 比较，包含背景/圆角 shorthand、继承 transition 中断、transform none 的 containing block 切换、width 和 viewport 改变；保留返回 scene 并改动另一份返回值，检验独立拥有。此前 120 随机 mutation、40 timeline、160 动画计划 mutation、失败计划重试/reset/owner 拒绝继续通过。

复杂 GPU/software 对照扩为 **184 次**：保留 64 次 projective/ring mutation、24 次 angle-only 和 8 次完成后 target 切换，增加 96 次 1/1.5/2/4x 的异色、fractional 不等宽、禁用单边、过大半径、非中心旋转/非均匀 scale、currentcolor/身份代际变化。父子半透明重叠、越界内容和 hidden/visible clip 作为反例，较大矩阵的旧路径继续被覆盖。硬件 worst mean / min IoU **0.595009 / 0.958484**，实际几何 build/reuse 387/252，24 次 angle-only upload 2；184 import 全退休，调试层零 warning/error。此 build 计数跨 slot 累加，不据此声称某个单 cache 已发生 eviction。

原 34 例 / **102 张 Chrome** 图片、1476 几何字段保持；硬件 mean / IoU 2.39076 / 0.757239，4117 提交/退休、两帧 peak、透明 filter 最大差 1、滚动 crop/reveal 继续通过。像素阈值 mean <=3 / IoU >=0.75 不变。窗口 160 帧、288 几何、17 cached/full 包、1680 像素最大差 <=3、输入/最小化恢复与真实 completion 继续通过。

强制 GC 实际运行：

```powershell
cmake --build build/windows-gc-stress --config Debug --parallel 8 --target weft_scene_contract_tests weft_d3d11_probe
ctest --test-dir build/windows-gc-stress -C Debug --parallel 1 -R '^scene_contract$|^d3d11_warp_complex_css$|^d3d11_warp_external_texture$|^d3d11_warp_two_frame$|^d3d11_warp_render_cache$' --output-on-failure
```

**5/5，15.02 秒**；本轮没有重跑 GC 的大型原 gallery/geometry/window，旧阶段对应证据继续保留。Release 首次新增测试编译因 ElementGeometry 没有 operator== 失败，改为显式字段比较后全套通过；日志保留，不改产品行为或阈值。所有版本源快照、9 个 executable、图片、两种调度条件的全 24 个主要 run、sanity、日志和原游戏 drift SHA 封存到本轮 metadata，旧 seal 逐项校验且不改写。

目前仅 Windows 固定 scene 范围；Android/iOS、Metal/Vulkan、device-loss、一般 CSS 3D、完整 72 实绘与长时真实游戏 UI 未验证。后续需定位默认调度的整页波动，继续减少 JS/scene/producer 提交成本，不能仅凭 affinity 结果提高默认窗口 FPS 承诺。
