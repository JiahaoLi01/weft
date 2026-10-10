# 复杂 CSS 动画优化

2026-10-09。本轮针对固定 gallery 的翻转、波纹、圆环、透明叠加和组合动画。原始函数、依赖与 bundle 不改，动画时钟和更新频率不降级；原 26 CSS + 8 D3 与两例 Canvas 继续显示，其他 36 个诊断项仍待接入。

## 运行

在 `F:\Projects\CaseElephant`：

```powershell
cmake --build --preset windows-release --parallel 8
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/complex-css/interactive

# 原 CSS 中段 / 末段：240 帧、16 帧预热、224 个固定时钟样本，无截图回读。
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-benchmark-hardware artifacts/local/complex-css/manual/css-middle css-middle
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-benchmark-hardware artifacts/local/complex-css/manual/css-end css-end

# 新的独立复杂动画 GPU / 完整软件参考反例。
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --complex-css-test-hardware artifacts/local/complex-css/manual/reference
```

窗口默认 CSS，滚动到 Flip Card / Ripple Effect / Pulse Ring / Spin + Pulse Combo；保留 4K、FPS、当前可见卡片数和 CPU 分位数。最终正确性和基准证据在 `artifacts/local/complex-css/`，此前 `gallery-performance/` 和 `gallery-css-d3/` seal 保留。

## 实现

- **显式 3D 平面的 GPU 投影。** 既有 `perspective(px) rotateY(deg)` 子树本来就要先整体展平。继续用完整软件源图，按身份/代际、内容、尺度缓存，改变外层角度时由 D2D device context 的 [DrawBitmap perspective transform](https://learn.microsoft.com/en-us/windows/win32/api/d2d1_1/nf-d2d1_1-id2d1devicecontext-drawbitmap%28id2d1bitmap_constd2d1_rect_f__float_d2d1_interpolation_mode_constd2d1_rect_f__constd2d1_matrix_4x4_f_%29) 完成 encoded-premultiplied 双线性采样。原 CPU inverse-map 路径仍作参考；没有新增一般节点的自动图层提升。投影前的内容包含原 group opacity、children、文字与裁剪，parent transform 和绘制顺序保持当前值。设备不提供该 interface 或平面穿越投影面时走原路径；近 90 度的奇异平面仍沿用参考的空绘制判定。
- **圆环和边框。** 仅四边同宽、同色、solid 时用一次原圆角 geometry 描边和外部 clip；异色、不等宽、非 solid 继续逐边路径。逐边 wedge 的创建结果按完整宽高、四边宽度和 side index 缓存，最多 256 项，factory 改变时清理。没有把 group opacity 分摊到各个重叠图元。
- **动画计划和端点解析。** owner 内按完整基础 computed style 缓存已解析 animation list、duration/delay/timing override 和每属性有序 keyframe track。继承值、隐式端点、动画列表/顺序、持续时间或 delay 变化会重建；仅数值采样每帧推进。失败计划保持 invalid，重试同一个非法值仍拒绝，不复用上次有效计划。端点 token/color 缓存最多 512 项 / 512KiB 有效载荷，reset 时清理；full reference 每帧重建计划并重新解析端点。
- **减少 owner 分配。** 保守 layout 比较直接过滤同一组 paint-only 属性，不再每次复制并删除整个 style map；transform 是否创建 containing block 仍单独比较。节点缓存 key 用三元身份值，数值串使用 locale-independent、12 位有效数字的 `to_chars`。布局变化与同步 geometry read 保持全量正确路径。

缓存只保存 owned scene 值或对应适配器资源，不持有 DOM 指针/JSValue。既有每卡最多三个 surface version、最多两帧在途和真实 submission completion 后重写/退休保持不变。projective CPU 源图仍最多 16 项 / 16MiB；新增对应 GPU source bitmap 最多再占相同有效载荷，总数不等于进程内存或总 VRAM。超大源图不缓存。公共 renderer C ABI 与图形 API 无关的 external lease 合同不变。

## 最终测量

Windows 11 26200 / Ryzen 9 9950X3D / RTX 5090 driver 32.0.16.1062 / MSVC 19.42 C++20，Release + D3D11 debug layer。surface 3840×2160、shell 1920×1080、CSS body 387×288 / D3 body 384×384，2x 绘制；34 VM 在 mount 时共 560 节点，shell 字体/系统 body 字体/原始 bundle 与上一阶段相同。

每场景 240 帧、16 预热、224 次固定 60Hz owner clock 样本，测量后 drain 真实 GPU query。旧版保存 executable 和源快照，额外仅加入末段 viewport 基准入口；最终 before-repeat / after 依次跑，不并行编译、测试，不截图/readback。两版使用相同 224 interval 吞吐公式与相同 producer+UI elapsed GPU query 范围。`before/` 和 `iteration/` 为定位阶段，首轮位于 `before-repeat/` 与 `after/`；最终主表使用 `paired/round-{1,2,3}/` 的全部交替样本。

首次全部 before 后全部 after 的采样出现首屏 145.05 → 125.11 FPS；未改动的 JS/encode 阶段也一起变化，不能把原因确定为优化代码或运行环境。因此又进行 3 组交替测量：每个场景 round 1/3 为 before→after，round 2 为 after→before。两版同帧数、固定时钟和字体；每个场景每版共 672 个 CPU/GPU 时间样本。主表是三个 run 吞吐的中位数，**不是混合全部帧计算的分位数**。全部首轮与重复数据保留。

| 场景 | before FPS 中位数（范围） | after FPS 中位数（范围） | 中位数比值 | CPU P50 中位数（ms） |
| --- | ---: | ---: | ---: | ---: |
| CSS 首屏 / 12 body | 123.16（121.76–137.11） | 128.80（120.12–142.25） | 1.046x | 7.914 → 7.429 |
| CSS 中段 / 16 body | 67.38（62.35–69.13） | 87.56（79.00–91.44） | 1.300x | 14.474 → 11.243 |
| CSS 末段 / 10 body | 80.60（74.87–86.30） | 106.61（94.04–111.22） | 1.323x | 11.946 → 9.115 |
| D3 / 8 body | 185.00（153.19–193.05） | 166.36（149.33–186.01） | 0.899x | 4.958 → 5.943 |

复杂中段/末段的三组 after 都高于对应 before；首屏重复样本没有一致下降。D3 重复样本中位数 185.00 → 166.36 FPS，范围重叠，首次独立采样又为 169.08 → 213.27；**尚未排除轻场景退化，不能据范围重叠断言只是噪声，也不宣称 D3 获得稳定提速**。未记录 CPU core/频率/系统调度遥测，具体原因仍待定位。中段 after 范围 79.00–91.44 FPS，没有保证全部复杂 CSS 稳定超过 90 FPS。

中段 scene/raster 各 run P50 的中位数从 3.077 / 5.143 降到 1.978 / 3.455 ms；末段从 2.298 / 4.317 降到 1.452 / 2.591 ms。首轮同条件翻转单卡 P50 1.014 → 0.169 ms，波纹 0.608 → 0.317、脉冲圆环 0.749 → 0.522。阶段分位数不能相加。

| 场景 / 路径 | CPU P95 / P99 中位数 / 全 run 最大帧（ms） | GPU elapsed P50 / P95 / P99 中位数 / 全 run max（ms） | working set 峰值 / close private 峰值（MiB） | 启动范围（ms） |
| --- | --- | --- | --- | --- |
| CSS 首屏 / 12 body / before | 9.861 / 10.273 / 11.538 | 0.242 / 0.286 / 0.444 / 0.653 | 237.867 / 870.562 | 3617.181–3736.878 |
| CSS 首屏 / 12 body / after | 8.926 / 9.704 / 10.694 | 0.170 / 0.215 / 0.226 / 0.445 | 237.348 / 865.332 | 3634.188–3772.597 |
| CSS 中段 / 16 body / before | 17.491 / 18.520 / 21.849 | 9.427 / 11.635 / 12.201 / 15.378 | 238.199 / 1145.340 | 3650.427–3705.409 |
| CSS 中段 / 16 body / after | 14.059 / 15.358 / 16.868 | 6.823 / 8.222 / 8.812 / 9.801 | 236.691 / 1088.012 | 3605.342–3760.140 |
| CSS 末段 / 10 body / before | 14.652 / 15.929 / 17.038 | 8.550 / 10.481 / 11.377 / 13.188 | 232.086 / 1080.312 | 3569.052–3664.325 |
| CSS 末段 / 10 body / after | 11.197 / 11.839 / 14.491 | 0.555 / 7.115 / 7.989 / 11.078 | 230.598 / 974.133 | 3558.659–3722.517 |
| D3 / 8 body / before | 7.167 / 7.767 / 13.301 | 0.137 / 0.155 / 0.165 / 0.661 | 225.980 / 844.234 | 3561.113–3775.497 |
| D3 / 8 body / after | 7.167 / 7.954 / 22.307 | 0.137 / 0.155 / 0.300 / 0.377 | 226.188 / 844.535 | 3633.836–3727.564 |

首轮 D3 after 有一次 CPU max 21.272 ms，仍保留于 `after/d3/preview-session.json`；重复统计不覆盖首轮尖峰。完整各 run/stage/p50/p95/p99/max 见 `run-metadata.json` 和原 JSON。

隐藏窗口的实际完成吞吐不能替代可见窗口长时 FPS；也没有同负载 Ultralight 测试。GPU elapsed 包含提交过程中的 CPU 调度空隙，排除 HUD/Present 的 GPU 工作，不是纯 GPU busy time。GPU cache bytes 仅统计 renderer 的 CPU asset texture payload，不含卡片 backing/plane bitmap/D2D 缓存；总 VRAM 与冷首帧尖峰未测，启动统计包含 72 原例观察。

## 验证

最终 Release **75/75，26.92 秒**；Debug **75/75，65.26 秒**，GPU 原例和新增复杂 CSS 为 CTest / WARP。两配置另跑硬件的 `--complex-css-test-hardware` / `--scene-gpu-test-hardware` / `--gallery-window-test-hardware`，日志和图片在 `final/{release,debug}/`。102 张 Chrome 对照的硬件 worst mean / min IoU 仍为 2.39076 / 0.757239；新增复杂 CSS 硬件为 0.592469 / 0.964642、WARP 为 0.574480 / 0.960874。24 次仅变角度的 source upload 实际 1 次。原 4117 与新 88 次 import 全退休，debug layer 零 warning/error。

实际构建和普通检查使用两组 `cmake --build --preset windows-{release,debug} --parallel 8` / `ctest --preset windows-{release,debug} --parallel 6 --output-on-failure`（各配置分别执行，日志 `final-*-build.log` / `final-*-ctest.log`）；新 angle-only cache proof 后再次运行这两个完整 CTest。强制 GC 实际命令：

```powershell
cmake --build build/windows-gc-stress --config Debug --parallel 8 --target weft_scene_contract_tests weft_scene_gallery_tests weft_d3d11_probe
ctest --test-dir build/windows-gc-stress -C Debug --parallel 1 -R '^scene_contract$|^scene_gallery_original$|^scene_browser_geometry$|^d3d11_warp_scene_gpu$|^d3d11_warp_complex_css$|^d3d11_warp_external_texture$|^d3d11_warp_two_frame$|^d3d11_warp_render_cache$' --output-on-failure
```

强制 GC **8/8，446.67 秒**，包括全部原 CPU/GPU scene。追加 angle-only 24 次证明后仅重建 weft_d3d11_probe，再跑 `-R '^d3d11_warp_complex_css$'`，1/1、2.90 秒；无 JS budget 或阈值调整。本轮未重复 GC gallery window。最后消除测试局部变量 shadow warning，三配置仅重建 weft_scene_contract_tests / 复跑 `-R '^scene_contract$'`，Release/Debug/GC 各 1/1，0.38/3.21/3.48 秒；渲染器和测量 executable 不变。最终源/三配置二进制/原图与实际图/日志/全部重复样本 SHA、原游戏只读 drift 封存在 `run-metadata.json`，先前 seal 不改写。

新增 owner 160 帧对照覆盖动画叠加/顺序、steps、duration/delay/时序函数、继承色与隐式 border 端点、字体/文本/宽度、移除/重插和动态 transform 字串；每帧完整 scene 值与软件图片与关闭增量的 reference 相同。另检验失败计划连续重试拒绝、有效值恢复与 reset 清理。

新增 GPU 64 次 mutation + 24 次 angle-only 对照覆盖 1/1.5/2x、正反角度/90 度、非中心 origin、嵌套 rotate/translation、父子 group opacity、半透明颜色、文字/字号/节点代际改变、圆角/圆环和异色/不等宽/禁用单边。8 次完成后的 CPU/GPU target 切换；真实 query completion 后 88 次 caller/native import 全退休，24 次仅改角度的绘制最多 3 次 source bitmap upload。GPU fast/fallback 计数必须实际执行。软件参考与原 Chrome 阈值均维持 channel mean <=3 / soft IoU >=0.75，没有放宽阈值。

原 102 张 Chrome 图片、1476 个几何字段、120 随机 mutation 和原窗口输入/裁剪/最小化/两帧退休继续验证。当前仅 Windows 固定 planar 3D 子集；一般 CSS 3D、自动图层合成线程、GPU 直接绘制 projective 内部内容、device-loss、Android/iOS、Metal/Vulkan 和长时复杂游戏 UI 尚未验证。下一步继续降低 owner scene 更新与 group isolation 的剩余成本。
