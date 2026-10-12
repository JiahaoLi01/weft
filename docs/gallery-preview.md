# 当前检查入口（2026-10-10）

快速入口：双击 `tools\gallery.bat`，或在仓库根目录运行 `tools\gallery.bat <编号>`。菜单项：1 目录窗口、2 带 profiler 的窗口（关闭后自动分析 trace）、3 live 拖拽/Canvas 基准、4 隐藏自测、5 Canvas 窗口、6 构建 Release 探针、7 Vulkan scene 窗口（原生 Vulkan scene pass 逐个显示 34 个 scene，点击、空格或方向键切换）、8 Vulkan scene 窗口自测。可执行文件缺失时先构建；每次运行写入 `artifacts\local\gallery-runs\<时间>-<模式>`。

`weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-cluster/interactive`：4K、FPS/OWNER Hz、72标签/十组，66原例像素、6诊断。新30例的交互、共享能力和原始 Throw 问题见 [本轮合同](gallery-shared-cluster.md)。以下为此前36例阶段的历史记录，其计数和旧封存路径不代表当前二进制。

# 4K Gallery 检查窗口

2026-10-09。新增原生 DOM/CSS 目录窗口，保留固定 TestGallery 的 72 个案例、原始标签、10 个分组顺序、列数和内容高度。默认跳到 CSS；26 个原样 CSS、8 个原样 D3 和两个 React Canvas 案例在卡片内实时绘制。其余 36 例显示绘制缺口与新鲜的执行诊断，**没有把诊断卡片算成原始案例绘制通过**。这是显式检查 shell，不是完整原始 Tailwind/Grid 页面；原游戏、固定 React/第三方包和 bundle 未修改。

## 运行

在 `F:\Projects\CaseElephant` 的 PowerShell：

```powershell
cmake --build --preset windows-release --parallel 10
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/render-stages/interactive
```

窗口默认最大化，内部目标为 **3840×2160**；逻辑 shell 为 1920×1080，文字以设备字号重新栅格。实际窗口较小时等比例显示，不改变内部目标。鼠标坐标经过窗口、4K surface、shell 和 Canvas 卡片映射。原样 Canvas backing 仍是 800×600，卡片居中显示；4K 合成不等于把原始位图细节变成 4K。

- 左侧分组可点击跳转；滚轮、Home/End、PageUp/PageDown 浏览全部卡片。
- CSS 4–6 悬停触发变换/色值/透明度，CSS 7 点击进度宽度；D3 原动画持续运行。
- 粒子区移动鼠标，烟花区点击；滚动后输入继续使用最新几何。
- 卡片点击在底部显示阻断摘要，完整错误栈写入输出目录的 `preview-cases.json`。
- F5 重置当前性能采样，Esc 关闭；关闭后生成 `preview-session.json`。
- 原有 `--gallery-canvas-window-hardware` 继续保留，Tab 切换单独的粒子/烟花。

当前 34 个 CSS/D3 可见卡片使用 owned native GPU pass，未支持组合继续 D2D GPU backing，均通过 external lease；原两例 Canvas 保持 WIC 软件 backing。字体/裁剪/缓存/真实退休与性能测量见 [性能阶段](gallery-performance.md)。后续复杂动画优化见 [复杂 CSS](complex-css-performance.md)，此前调度/DOM revision/数值绘制见 [owner 热路径](owner-hotpath-performance.md)，当前显式源图/原生 GPU/独立 owner 与性能口径见 [本轮](render-stages-performance.md)。

## 时钟错误修复

原可见 Canvas host 用 `std::to_string(double)` 给 timer pump 传时间，只保留 6 位小数；rAF 则使用 17 位精度。同一个 `.2500006` 会先被 timer 舍入到 `.250001`，随后 rAF 读到更小的时间，触发 `WEFT_HOST_TIME`。新增 classic locale / `max_digits10` 的可往返序列化，两者使用同一个宿主时间。单调性检查保留。

`animation_frame_contract` 明确证明旧格式会被拒绝，再验证 1,024 个不同小数时间的 timer/rAF/pump 与共同时间戳。旧 Canvas 窗口隐藏测试现在也使用会触发旧 bug 的小数；新目录窗口直接使用实际 steady clock，覆盖离屏暂停、恢复和输入。

## 状态与帧率的含义

启动时逐个隔离运行 72 个固定案例，各观察 12 次 owner 调度机会。启用现有 D2D Canvas adapter，两例 Canvas 使用显式布局 adapter；在这项12机会的隔离观察中，其他案例不加载原始完整 CSS、不分发输入，也不作像素验收；34个CSS/D3的原生绘制验收另由scene和实际窗口测试完成。本轮正常 Debug/Release 观察为 **45 例执行未发现错误、27 例执行阻断**。原先未安装 Canvas 的 38/34 报告继续保留，条件不同，不替换历史证据。

绿色 LIVE 指已有原生证据的 26 CSS、8 D3 和两例 Canvas。其余卡片分别列出 JS 状态和 CSS、SVG、WebGL、图表 Canvas、动画/几何或资产请求缺口。JS 未报错不等于布局、绘制或交互通过。

FPS 统计成功、可见的 Present，每秒更新；OWNER 单独统计实际 UI publication Hz、active P50 与帧年龄；显示当前可见原生 body 数量。**负载是检查 shell 加可见 CSS/D3/Canvas，不是完整 72 例原始 gallery。** 34 个 scene VM 保留状态，8 D3 在离屏时继续 timer/rAF，CSS 在重新可见时取当前时间，GPU paint/转换仅对可见 body 执行。两例 Canvas 移出 viewport 时不派发 rAF，保留待执行回调，恢复时只提供当前机会，不补历史帧。最小化/遮挡停止有效采样，滚动、窗口尺寸变化和恢复时重置采样，先暖身 16 帧，保存最近最多 600 帧的 CPU tick、GPU producer+UI elapsed 和 Present interval 的 P50/P95/P99/max。

CPU tick 截止到本帧提交/Present 返回；确定性 benchmark 包含等待 worker owner，交互模式非阻塞消费 latest，此外包含主线程目录、快照准备与 GPU producer/提交；不是 GPU completion 或显示延迟。当前 query 包围 producer 与离屏 UI elapsed 范围，含 queue 中 CPU 调度空隙，不含 HUD、窗口合成和 Present；旧软件阶段 query 仅包围 UI command list，两个范围不能直接比较。报告另记录真实 completion 数、资源退休、GPU 名称、启动（含 72 例观察）、进程峰值 working set/private bytes。shell 字体为固定 Roboto、Noto CJK；scene body 用 Windows DirectWrite/OS system-ui（本机 Microsoft YaHei UI）、Arial/Consolas/Emoji fallback，详情见 [scene 字体条件](scene-gallery.md)。历史 `✗` 标签另使用同一个固定 HarfBuzz 源码中的 Mplus fallback，出处/校验和/OFL 1.1 在 `examples/crpg-gallery/MPLUS-FONT-NOTICE.txt`。

可见模式没有每帧 readback；隐藏自动测试带截图复制、脚本输入与 readback，属于正确性验证，帧率不作为性能 benchmark。测试环境 Windows 11 26200、Ryzen 9 9950X3D、RTX 5090/MSVC 19.42，以及 WARP；实际验证报告区分驱动和配置。未做同机、同源码、同可见案例/Canvas backing、同窗口/字体/输入/热状态的 Ultralight 对照，因此 `ultralight_comparable=false`；不能据此声称快于用户原来的 40 FPS。

## 验证与产物

```powershell
ctest --preset windows-release --output-on-failure
ctest --preset windows-debug --output-on-failure
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-test-hardware artifacts/local/gallery-performance/manual/release/window-hardware
& .\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --gallery-window-test-hardware artifacts/local/gallery-performance/manual/debug/window-hardware
```

首次软件路径34例的实际结果与 seal 在 `artifacts/local/gallery-css-d3/`，当前 GPU/优化版证据在 `artifacts/local/gallery-performance/`；完整 Debug/Release/GC 命令和通过数见 [status](status.md)。原例102张截图与1476字段独立浏览器对照、120轮随机mutation和卸载反例见 [scene gallery](scene-gallery.md)。隐藏4K窗口仍跑160帧/72标签/288卡片几何/17完整绘制包差分/1680 GPU-CPU像素采样，并新增 CSS悬停/点击与滚动中段/末段/D3截图。10组首屏截图只覆盖当时可见的53张卡；其余位置通过全目录几何检查，不声称十张图片展示所有卡片。

当前 `preview-session.json` 的 `last_visible_native_case_count` 包括 scene 和 Canvas，GPU缓存预算512MiB仅供此检查host。34 scene 使用 GPU backing，最复杂中段仍有 scene/producer 与部分软件投影成本，详见 [性能阶段](gallery-performance.md)。移动与同负载 Ultralight 比较继续标false。

此前仅两例Canvas的阶段原始证据和统计保留在 `artifacts/local/gallery-preview/`；当时 Debug/Release 69/69 与 GC 3/3 对应旧实现，不能当作本轮34例通过依据。历史尝试的GC大型模块审计停止日志也保留。现在GC仍跳过72模块的隔离短观察，34 CSS/D3和两例live Canvas由独立实际窗口/scene测试覆盖，报告不把未运行审计算作no_error。

尚未实现其余 36 例的原始可见内容、完整 Tailwind/Grid、通用 SVG/WebGL、Canvas 高级 API、IME/触摸与移动。实体设备操作和显示延迟未自动验收；平台证据仍只有 Windows。下一步降低复杂 scene/producer 成本，并按目录的实际阻断扩展 SVG 交互、DOM/调度接口与 Canvas/WebGL 集成，再开展相同负载的 Ultralight 对照。
