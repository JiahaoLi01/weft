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
$exe = 'F:\Projects\CaseElephant\build\windows-msvc\bin\Release\ce_d3d11_probe.exe'
& $exe --gallery-window-profile-hardware 'F:\Projects\CaseElephant\artifacts\local\interaction-profile\manual'
# 操作D3拖拽、K-Line移入、播放/重放动画；Esc退出后写完trace。
python tools/analyze_gallery_trace.py artifacts/local/interaction-profile/manual
```

普通4K窗口仍用 `--gallery-window-hardware`。profiler窗口额外写 `owner-trace.json`、`render-trace.json`，二者用同一个steady-clock微秒时钟、17位精度。分析脚本离线验证时间、publication/input覆盖和排空计数，合并到 `combined-trace.json` 并输出 `profile-summary.json`。Trace为Chrome/Perfetto trace-event JSON，每条含tid、ts、dur和拥有值参数；owner显示input queue age/各案例style/build/layout/geometry，render显示每卡绘制和整帧（包括owner等待）。HUD显示Present/Owner Hz、owner active P50/P99、frame age P99、inputQ。

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
