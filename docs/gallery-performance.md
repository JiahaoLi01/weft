# Gallery 性能阶段

2026-10-09。固定 26 CSS + 8 D3 的可见窗口已切换为 Windows D2D GPU 绘制与既有 external texture lease；保留完整 WIC 软件参考路径、全部 72 个目录项、36 个实际绘制案例、4K/FPS 和原样 React/D3。原始游戏目录与依赖包未修改，另外 36 例仍有明确诊断。

本记录保留首次 GPU 阶段证据；最新翻转、圆环和动画计划优化见 [复杂 CSS 阶段](complex-css-performance.md)。

## 复现

在 `F:\Projects\CaseElephant`：

```powershell
cmake --build --preset windows-release --parallel 8
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-performance/interactive

# 固定 240 帧、16 帧预热，避免截图回读参与性能采样。
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-benchmark-hardware artifacts/local/gallery-performance/manual/css-top css-top
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-benchmark-hardware artifacts/local/gallery-performance/manual/css-middle css-middle
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-benchmark-hardware artifacts/local/gallery-performance/manual/d3 d3

# 原例 GPU 图片与新增透明滤波 / 裁剪露出反例。
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --scene-gpu-test-hardware artifacts/local/gallery-performance/manual/scene-gpu
```

窗口默认 CSS，侧栏跳转 D3；滚轮/侧栏、悬停、点击、Canvas 鼠标、最小化恢复保留。关闭后输出 `preview-session.json`；固定基准还输出 `benchmark.json`。已有的旧阶段 seal 不重写。

## 实现与边界

- 样式表只解析一次。style key 保留节点身份/顺序、namespace、inline style、所有 stylesheet attribute selector 依赖与 SVG presentation 属性，忽略不参与层叠的坐标/文本更新；2026-10-11 起按 DOM 子树版本号原地维护，未变的子树不再重读。布局签名只排除已知 paint 属性，保留尺寸、字体、文本、结构与 `transform` 是否创建 containing block；改 viewport、字体或布局属性会重新 layout。静态 scene 仍泵原 timer/rAF，DOM 不变且没有待推进动画时复用 scene。同步 geometry read 继续经过最新布局。
- 可见卡片由宿主明确分配 GPU backing，不做自动图层提升。每卡最多三个完整 version，UI 最多两帧在途；重写前验证最后消费 serial 已真实完成并释放旧 caller lease。GPU producer 转换/合成在同一 device/queue，异常中的 producer-only 工作也通过 event query 完成后退休。
- D2D 写 encoded-premultiplied BGRA8，逐 texel shader 转 linear-premultiplied RGBA16F，然后由已有 renderer 外部纹理接口滤波/合成。半透明纹理不能先滤波再 decode。公开 C ABI 不变，私有 D3D11 probe 外部 registry 从默认 32/64MiB 可显式配置到 128/512MiB；卡片池还持有 BGRA backing 与 D2D 缓存，512MiB 只是 linear import payload 上限。
- 线性不透明 gradient 走原生 brush，使用 sRGB 插值，其他 gradient 保留软件生成与有界缓存；[D2D gamma 枚举](https://learn.microsoft.com/en-us/windows/win32/api/d2d1/ne-d2d1-d2d1_gamma) 区分这两种插值。GPU brush 按参考 RGBA8 premultiplied 精度量化，保持低透明度背景的阈值一致，没有放宽原图 oracle。
- DirectWrite text layout、路径、圆角 geometry 与 D2D scratch layer 重用；检测到四字节 Unicode 的 HTML color-text 路径使用有界设备字号文本图缓存，保持当前文本/字体/盒子与外部 transform/opacity。原本缩放动画中重复生成的彩色 emoji 是明显热点。这不是通用 color-font/SDF 声明，SVG 文本仍走原绘制路径。
- projective 平面保留原 inverse-map/bilinear 实现，未变的平面内容先复用再投影。可见区域裁剪变化发布新 version，滚动露出不能复用旧裁剪图。文本图缓存最多 32 项/8MiB，gradient 32 项/32MiB，平面内容 16 项/16MiB；超大内容走非缓存路径。字体布局/路径/geometry/layer 都有容量上限，缓存属于各适配器，不保留 DOM/JS 引用。
- CPU asset identity 改为弱引用元数据；不再因累计 64MiB 动画位图清空所有活跃纹理身份。过期地址重新分配身份，活跃 source ID 稳定。全不透明 RGBA8 可用 hardware sRGB decode-before-filter 的紧凑 texture；半透明仍转成 linear-premultiplied float texture，防止边缘颜色污染（2026-10-10 起由 GPU 从原始 RGBA8 转换，结果逐位不变）。

## 固定性能条件

Windows 11 26200，Ryzen 9 9950X3D，RTX 5090 / driver 32.0.16.1062，MSVC 19.42 / C++20，Release，D3D11 debug layer 开启。内部 surface 3840×2160，shell 1920×1080，CSS body 387×288 / D3 body 384×384，以 2x 绘制；Canvas backing 800×600 不变。原始 34 VM 在 mount 时共 560 native nodes；shell 字体为既有 Roboto/Noto/Mplus，body 为同机 system-ui/Arial/Consolas/OS Emoji，字体证据沿用 reference manifest。

每场景 240 次固定 60Hz owner clock，前 16 次预热，224 个 CPU/stage/GPU 时间样本；最后 drain 真实 completion。三个场景在全部正确性任务完成后依次运行，不并行编译/测试，也不截图/readback。保存 pre-optimization executable 和 source，再用相同 final shader 复跑 before/after；新增 conversion material 在旧 binary 中未使用。baseline 的旧采样公式计 223 个 presentation interval，优化版计完整 224 个 interval，原始 JSON 与该差异均保留。CPU 分位数的 224 次采样完全相同。

这是隐藏 HWND 中实际完成提交的吞吐，不能直接当作可见窗口长时间 FPS；也没有在同负载运行 Ultralight，不能据此宣称公平胜过用户报告的 90 FPS。初期 `iteration-*` 是探索，最终数值只取 `baseline-repeat/` 与 `optimized/`，见 `run-metadata.json`。CPU 时间包含 owner、绘制、提交与 Present；baseline GPU 时间只含 UI 合成，优化版增加 producer+UI elapsed 范围（包含 queue 中的 CPU 调度空隙），都不包含 HUD/Present 的 GPU 工作，不能把这两列 GPU 数值直接相减。

| 固定场景 | 原版本 FPS | 优化后 FPS | 提升 | CPU P50（ms） | CPU P95 / P99 / max（ms，优化后） |
| --- | ---: | ---: | ---: | ---: | --- |
| CSS 首屏（12 个可见 body） | 14.09 | 117.56 | 8.34x | 66.980 → 8.313 | 10.179 / 10.940 / 11.094 |
| CSS 中段（16 个可见 body） | 5.22 | 55.14 | 10.57x | 188.805 → 17.347 | 21.203 / 23.626 / 31.003 |
| D3（8 个可见 body） | 21.91 | 128.39 | 5.86x | 45.425 → 7.470 | 9.315 / 9.796 / 11.585 |

原版本 CPU P95 / P99 / max：CSS 首屏 103.922 / 113.677 / 117.564 ms，中段 218.983 / 235.638 / 244.682 ms，D3 69.736 / 75.280 / 80.050 ms。中段新版本仍低于用户报告的 90 FPS，不能把前期约 80 FPS 的探索结果替换进最终表。

中段 CPU 分项 P50：JS 2.921 → 2.105 ms，snapshot 0.514 → 0.429，scene 35.535 → 3.747，raster/producer 75.026 → 6.484，prepare 57.604 → 0.120，encode 9.323 → 1.418。各阶段分位数不相加；剩余热点是复杂 scene 更新、projective 内容/投影和 producer。

| 场景 / 路径 | GPU 测量 P50 / P95 / P99 / max（ms） | 进程 peak working set / close private（MiB） | 启动含 72 例观察（ms） |
| --- | --- | --- | ---: |
| CSS 首屏（12 个可见 body） / 原版本 UI | 0.975 / 3.787 / 4.066 / 4.159 | 851.070 / 1812.801 | 3954.748 |
| CSS 首屏（12 个可见 body） / 优化后 producer+UI elapsed | 0.240 / 0.292 / 0.475 / 0.482 | 237.480 / 870.992 | 3679.150 |
| CSS 中段（16 个可见 body） / 原版本 UI | 4.881 / 5.573 / 5.731 / 5.810 | 1135.199 / 1972.191 | 4019.186 |
| CSS 中段（16 个可见 body） / 优化后 producer+UI elapsed | 10.848 / 13.523 / 18.879 / 21.562 | 236.016 / 1075.641 | 3853.635 |
| D3（8 个可见 body） / 原版本 UI | 0.868 / 2.787 / 2.973 / 3.129 | 681.547 / 1695.172 | 4029.953 |
| D3（8 个可见 body） / 优化后 producer+UI elapsed | 0.137 / 0.153 / 0.162 / 0.414 | 224.852 / 840.078 | 3922.099 |

GPU 两种测量范围不同，中段的 producer+UI elapsed 还包括 CPU 提交空隙；该数字不是纯 GPU 忙碌时间。原版本 CPU asset texture cache payload 峰值约 504–510 MiB，新版本约 0.088 MiB；后者不含明确分配的卡片 BGRA8/RGBA16F backing、D2D cache 或其他驱动资源，不能据此宣称总 VRAM 降为 0.088 MiB。实际卡片池首屏/中段/D3 分别使用 16/32/10 个 slot，240 帧累计 800/3317/391 次 paint/import 全部退休。总 VRAM、冷首帧尖峰和长期采样尚未测量。

## 验证

普通 Release/Debug 分别全量 74/74；GPU 原例测试新增到 CTest。每例 121 帧、frame 24/63/120，仍用原 102 张真实 Chrome PNG，RGBA channel mean <=3 / soft IoU >=0.75；CPU 的 34 个空图反例仍拒绝。几何仍核对 1476 字段，随机 120 mutation 与额外 40 帧动画缓存/full 对照、attribute selector/SVG presentation key、reset/owner/卸载保留值均验证。

新 GPU 测试额外检验半透明图像在非整数坐标缩放/滤波/0.7 opacity 下的独立 CPU oracle、viewport crop/scroll reveal，累计 4117 次真实提交/退休，最大两帧在途。窗口继续检验 160 帧、288 个卡片几何字段、17 次缓存/full packet、1680 GPU/CPU 像素、真实 HWND 输入与最小化恢复。Debug/Release 均另跑 RTX5090 原例与窗口；WARP 为 CTest。每个实际 GPU run 要求调试层零 warning/error、caller/native import 全部退休。

Release 全量 74/74，32.39 秒；Debug 全量 74/74，73.08 秒。RTX5090 原例的实际 worst channel mean / minimum soft IoU 为 2.39076 / 0.757239，WARP 为 2.33236 / 0.757239；新增透明滤波 oracle 最大差 1 色阶，window 像素最大差 1 色阶。阈值 3 / 0.75 保持不变。

强制 GC 重建同三个 target，实际运行：

```powershell
cmake --build build/windows-gc-stress --config Debug --parallel 8 --target weft_scene_contract_tests weft_scene_gallery_tests weft_d3d11_probe
ctest --test-dir build/windows-gc-stress -C Debug --parallel 2 -R '^scene_contract$|^scene_gallery_original$|^scene_browser_geometry$|^d3d11_warp_gallery_preview$|^d3d11_warp_scene_gpu$|^d3d11_warp_external_texture$|^d3d11_warp_two_frame$|^d3d11_warp_render_cache$' --output-on-failure --output-log artifacts/local/gallery-performance/final-gc-ctest.log
ctest --test-dir build/windows-gc-stress -C Debug -R '^d3d11_warp_gallery_preview$' --output-on-failure --output-log artifacts/local/gallery-performance/final-gc-gallery-preview-ctest.log
```

首轮并行 7/8 通过，269.16 秒；窗口加载在并行强制 GC 下触及原有 5000ms JS 执行预算。随后单独复跑该项 1/1，342.68 秒。8 项不同检查均有通过证据；不宣称首轮 8/8。没有改变 JS 预算、断言或测试超时，失败日志另保留 `initial-concurrent-gc-ctest.log`。大型 GPU/CPU original 分别 257.51 / 254.16 秒。普通 build/CTest、硬件 scene/window、GC 初次与重跑日志和完整图片/几何/原始基准 JSON、最终二进制/源 SHA 都封存于 `artifacts/local/gallery-performance/run-metadata.json`。

初期图像比较定位了 native gradient 的 gamma 选择，以及低透明度背景在 8bit brush 精度上的一个色阶差别；阈值保持原样。新增半透明 oracle 还发现旧 CPU `cheetah_sample` 忽略 draw opacity，已修正并通过全量回归，未改变实际 renderer 的 opacity 行为。

平台范围仍是 Windows 固定 scene；Android/iOS、Metal/Vulkan、GPU device-loss 恢复、长时尾部与通用页面性能尚未验证。projective 平面和部分 gradient、color text 首次生成、Canvas 仍有软件工作。生产跨平台 GPU paint IR 与稳定字体资源接口继续待实现。
