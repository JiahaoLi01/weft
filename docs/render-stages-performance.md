# 显式图层、原生 GPU pass 与独立 UI owner

2026-10-09。接续 owner 热路径的前两项，完成方向 3、4、5 在固定 gallery 和独立宿主探针中的接入。实现为 C++20 的自有值接口与 Windows 私有 D3D11 adapter；公开 renderer/backend C ABI v1 保持兼容。原游戏和固定 React/D3 bundle 未改。仍为 72 个目录案例、36 个实时实绘、36 个诊断案例；未扩大一般 CSS、SVG、WebGL 或移动范围。

## 运行与宿主边界

```powershell
cmake --build --preset windows-release --parallel 8
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/render-stages/interactive
```

4K 检查窗口默认使用独立 UI owner。FPS 表示成功可见的 Present；HUD 另列 OWNER 更新 Hz、owner active P50 和已发布帧年龄，重复呈现同一完整 UI 帧不会被算成新的 owner 更新。检查 shell 的目录、几何与字体仍在主线程的独立诊断 runtime 上；真实 34 个 CSS/D3 VM 和 2 个 Canvas VM 在同一个 worker 上创建、访问、关闭和销毁。它没有让同一个 DOM 被多个线程访问。

`OwnerExecutor<Request, Input, Frame>` 是内部 C++ adapter：宿主发送 owned 输入和刷新值；输入按可靠顺序处理，队列默认 256、最大 4096，满时显式返回 Full，让宿主重试或施加背压。被丢弃的完整快照/请求移到队列锁外销毁，阻塞析构反例验证不会阻塞另一个游戏线程的发送 API。刷新请求可合并为最新一次；发布最多保留两个完整不可变值快照。身份携带 epoch，reset 后拒绝旧输入，正在执行的旧世代 publication 被丢弃。accepted 输入在正常 close 时排空，异常传回 host；不会在 render 线程销毁 owner 的 VM。独立 executor 验证真实 DOM/GC/reset，gallery 检查器重新加载则重建 executor 与对应 surface domain。

输入命中前在 owner 刷新同步几何；内部 measure 不回调 JS。renderer 只消费 `shared_ptr<const GalleryOwnerFrame>` 中的 owned scene 和 Canvas pixels，没有 Node 指针、JSValue 或借用 DOM 子树。交互窗口用 latest 非阻塞读取，owner 落后时保留完整旧帧；确定性 benchmark/self-test 显式等待当前更新，这两种口径不同。实际游戏引擎的输入队列、帧调度与 C ABI 接入仍需实现，本轮不是已经接入用户游戏的性能承诺。

## 显式源图缓存与局部重绘

宿主通过弱代际身份声明可复用的源图；固定 gallery 使用自身的 authored path manifest；native 仅选择复合背景/边框/文字（CSS 9/15），单个圆环/柱/emoji 使用便宜的 native shape/glyph，D2D reference 保留另一组明确路径，不改原函数、不按 CSS class 或 transform 自动提升图层。源图 key 保留所有内容、字体、尺寸、后代变换/透明度、裁剪与顺序，只移除图层根自己的位置、transform、opacity 和冗余的 raw style attribute。根的透明度在整层合成一次，不能分别乘到重叠后代。颜色、文字、尺寸或内部绘制变化使 key 失效；key/damage 另外比较实际数值 samples，不能用可能舍入相同的诊断字符串判断相等；未知越界文字、SVG/projective 源或不合格 bounds 回退完整参考路径。

D2D reference adapter 的显式缓存最多 32 项、32 MiB accounted bitmap bytes，资源域变化清空。native pass 的不可变字形/源图 cache 最多 128 项、16 MiB accounted texture bytes。缓存清空时当前 replay/slot 保留共享 lease，实际 submission 完成前不会释放仍被使用的图像。上述字节账目不包括 owned keys、驱动内部资源和在途 lease；不是总内存/VRAM 上限。

D2D 局部重绘以**将要重写的已完成 backing version 自己保存的完整 frame**为参照，比较旧/新子树与几何，重绘两者覆盖区域并加 AA guard。clip/尺寸变化、未知文本/SVG/projective bounds 或大面积变化走全量；viewport crop 变化必须清除和重建，滚动露出不能复用旧裁剪内容。局部清除后仍按完整 paint order 重放所有与区域相交的内容；透明叠加和 ancestor clip 保持。每卡最多三份 backing，仍最多两个真正的 GPU submission 在途，lease 退休/重写由实际 query completion serial 决定，不依赖可能被丢弃的上一个 candidate packet。

当前 native pass 对支持的卡片仍全量录制和重画，局部重绘主要用于 D2D fallback；encoded-to-linear 转换仍全卡执行。没有实现一般 retained display-list patch、所有 producer 的局部转换或自动图层策略。

## 图形 API 无关的 native pass

`NativeScene`/`NativePaint` 只含 owned 数值、仿射矩阵、clip、四角圆角、四边样式、最多八个梯度 stop、字形/源图值和 begin/end opacity group，没有 D3D11 类型。当前 adapter 使用结构化 instance buffer 与 shader 绘制矩形、圆角、solid 边框/圆环、linear/radial/conic 梯度，并按原透明顺序批处理连续 shapes。多绘制操作的 opacity 用最多四层独立 scratch group，合成后才乘整组透明度。全 adapter scratch accounted texture budget 为 128 MiB，超限回退；buffer 每 slot 最多 2048 records。所有可重写 buffer/group target 属于已确认完成的 slot。

支持的 opaque full-card root 可以直接清屏代替全卡通用 shader；crop、圆角、梯度、半透明或变换时保留正常绘制。encoded BGRA8 的 brush 量化顺序与参考一致，之后仍经逐 texel decode 到 linear-premultiplied RGBA16F 与既有 external lease 合成。没有取消转换和卡片合成成本，也没有改变引擎颜色合同。

字体 shaping/raster 复用 Windows DirectWrite/WIC，native pass 合成有界缓存纹理；ASCII 单色字形存白色 coverage、shader 应用当前文字颜色，非 ASCII/颜色字体保留 RGBA 源。此处没有接入页面 SDF atlas、MSDF 或 GPU 字体塑形。显式整层源图首次生成也使用参考字体/矢量 raster，稳定后的位移/opacity 直接 GPU 采样。

SVG、projective、圆角/变换子树裁剪和其他超出 native pass 的组合，返回完整 scene 的明确 fallback reason，继续使用已有 D2D GPU reference。不是接受部分 commands 后改写层叠语义。D3 因而继续有真实 SVG 像素与交互，不被移出产品范围。Metal/Vulkan 可消费这个 owned pass，但本项目没有相应 adapter 或移动验证。

## 验证与已知探索失败

软件全量与既有 102 张 Chrome 原例 fixture/阈值保持，GPU 原例测试还强制要求 native/fallback 和真实图层复用计数，避免“图像通过但优化路径未接线”。新增 320 帧/160 图 full-reference 对照覆盖 XHTML namespace、位移/透明重叠、尺寸/颜色、绘制顺序、节点移除/代际变更、缓存淘汰、crop/reveal 和真实两帧资源退休。owner executor 覆盖 512 可靠原生 DOM 输入、reset/stale、容量背压、刷新合并、异常、owner 创建/销毁与不可变旧快照。

独立 async 宿主用三个真实线程分别发送游戏请求、执行 UI owner、消费快照/GPU 渲染；以 4K、240 个机会、目标 120Hz 测量 queue API、render、owner 与 frame age。第 64 帧注入 50ms owner input stall 与 16 个 refresh burst，27 个输入全部处理，render 可以复用完整旧帧。120Hz 是请求节奏，不是实际输出帧率保证；producer 发完固定 240 次后停止，WARP/GC render 继续收尾时的 frame age 会增长，这个有限测试的年龄尾部不代表持续运行的输入到显示时延；样本中的 owner active 只来自消费到的 publication，丢掉的中间 publication 不进入该分位数。GC instrumentation 只用于生命周期，降低 update-count 门槛，不改变输入/合并/退休断言，不用于性能声明。

探索记录全部保留：最初 XHTML 被误判为 SVG，原例图像通过但全走 fallback；随后新增路径计数验收。native 初期文字百分比 opacity、encoded 低 alpha brush 量化、instance offset、窗口 owner 接线和 cache capacity 断言均有失败记录，最终报告必须来自修正后的构建。第一轮正式采样还发现为简单形状缓存纹理增加 draw call，保留原 paired/controlled/sanity 记录；调整宿主明确策略后的最终采样在 `sampling-v2/`，不与原轮次混合。native record 打包按 const reference 读取，避免复制整棵源图 key。GC 首次在 Debug build/test 同时进行时出现脚本 deadline 和 async update 门槛失败，安静环境重跑结果另列；不能用原来的失败轮次作为通过证据。

## 独立验证命令

```powershell
ctest --preset windows-release --output-on-failure
ctest --preset windows-debug --output-on-failure
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --scene-native-test-hardware artifacts/local/render-stages/check-native
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --render-stages-test-hardware artifacts/local/render-stages/check-damage
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --owner-async-test-hardware artifacts/local/render-stages/check-owner
```

保留的基线在 `baseline/source` 与 `baseline/build`；固定采样脚本 `tools/benchmark_render_stages.ps1` 默认写 `sampling-v2`，拒绝覆盖已有 benchmark。新测量应使用新的输出前缀，不能覆盖封存样本。记录中的 executable 仍会读取其编译时 WEFT_SOURCE_DIR 的脚本/shader，复现历史代码时应从相应 source-snapshot 独立重建，不能把一个旧 exe 与新 runtime assets 混用。

## 实测结果

最终 Release/Debug 全量各 **80/80**（114.77/221.04 秒），最终受影响范围各 **7/7**（77.15/119.02 秒）；最后安静 GC 范围 **10/10**（132.05 秒），未重跑 GC 大型原例/window。两配置 RTX5090 原 native scene / window、Release hardware damage / 三线程 async 均重跑，D3D11 debug warning/error 为零。

原 34 例 / 102 Chrome 图：native 2605、fallback 1512、源图 build/reuse 6/720，worst mean 2.32729 / min soft IoU .798493，不变阈值通过；4117 submission/retired import。新增 320 帧 / 160 对照：mean .20115 / IoU 1，native source 159 build / 1 reuse / 1 eviction，D2D source 298 build / 2257 reuse / 6 eviction，partial 98 / full 222，实际 320/320 完成；2-in-flight、crop/reveal/alpha oracle 保持。窗口 160 帧、288 几何、17 全量包、1680 CPU pixel samples（最大差 1），真实 36/72。

| 条件 / 场景 | FPS 中位（before → after） | 比值 | CPU P50 / P95 / P99 ms（before → after） | 三轮最慢 warm 帧 ms |
| --- | --- | --- | --- | --- |
| 默认 / 中段 | 122.68 → 147.23 | +20.0% | 7.788/9.772/10.474 → 6.363/8.739/9.655 | 11.104 → 11.025 |
| 默认 / 末段 | 181.44 → 194.36 | +7.1% | 5.269/6.584/7.422 → 4.771/6.404/6.828 | 8.792 → 15.128 |
| 同 affinity / 中段 | 158.60 → 197.80 | +24.7% | 5.996/7.760/8.264 → 4.668/6.230/6.828 | 9.005 → 7.990 |
| 同 affinity / 末段 | 203.41 → 228.65 | +12.4% | 4.695/5.609/5.970 → 3.873/5.073/12.847 | 20.912 → 19.712 |

表是各 run 吞吐和分位数的中位，不混合不同 run 的帧。controlled 中段范围 153.52–188.38 → 194.20–222.65，每一对 after 都更快；默认中段 120.85–131.64 → 142.29–151.76。controlled 末段 202.62–221.90 → 220.38–228.69，也每对更快；默认末段范围 155.64–184.00 → 167.95–206.66。

**末段尾部未收敛：controlled P99 从 5.970 增至 12.847ms，最慢 warm 帧仍约 20ms。** 后两轮 after P99 为 12.847/16.413，单个已计时 stage 未出现同等尖峰；等待/帧边界/调度需要进一步 trace，不能宣称所有时延都下降。main CPU tick 包含固定 benchmark 的 fresh-owner 等待。

controlled 中段 raster P50 2.367→1.088ms；JS .594→.504、scene 1.033→.912。after owner active P50/P95/P99 1.594/2.076/2.325ms，默认则 2.587/3.320/3.932；整页 CPU P50/P99 仍为 4.668/6.828ms，**不是整套 UI 已达到 2ms**。controlled 末段 raster 1.417→.659ms、owner 1.435/1.797/1.895ms。

GPU elapsed 的 run 分位中位（P50/P95/P99）：controlled 中段 3.871/4.677/5.046→.242/2.219/2.691ms，末段 .402/4.212/4.490→.164/1.796/2.126；默认中段 4.871/5.779/5.984→.241/2.590/2.905。query 的 CPU queue gap / producer flush / Canvas 范围影响见下，不能作为纯 GPU busy time 比值。

controlled 中段进程峰值 working set 中位 238.51→253.61MiB、关闭时 private 1017.70→965.89MiB，含 72 例隔离观察的启动 3472.74→3493.23ms；末段 working set 231.30→237.91MiB、private 925.63→851.96MiB、启动 3518.80→3460.31ms。默认中段 working set 238.90→253.93MiB、private 1023.59→964.82MiB、启动 3586.93→3600.40ms。working set 与 private/驱动映射口径不同，不将 private 减少解释为 VRAM 下降；冷首帧单独尖峰未测。

三真实线程 hardware 压力测试（未设置 affinity，含 50ms owner stall、初始冷 producer）：游戏发送 API P50/P95/P99/max **.0040/.0098/.0130/.0686ms**；renderer 1.945/3.824/12.758/60.152ms；消费到的 owner 3.090/4.098/4.281/4.920ms；frame age 15.541/18.896/61.010/76.398ms。240 render opportunities、122 fresh publications / 118 reuse、127 coalesced refresh，27/27 可靠输入、240/240 actual completion。120Hz 是 sleep-until 请求目标，Windows timer/OS 调度未受控，不将更新次数解释为 owner 只能 61Hz；年龄使用请求时间，非逐卡内容或输入到显示时延。GC 三线程验证另为生命周期数据，不使用这些耗时评估性能。

仅一对默认 sanity：CSS top 162.40→172.85 FPS；D3 215.09→212.17，基本持平且不可由一对断言回归/提升。D3 仍使用 reference SVG producer。第一轮较广的 native layer policy 完整原始数据在根 paired/controlled/sanity：controlled 中段吞吐中位 156.88→160.10、每对并非都更快；最终 narrowed host policy 的三轮数据在 sampling-v2，不丢弃第一轮、不混合计分。

产物与 SHA-256 seal 位于 `artifacts/local/render-stages/`。baseline 650 个旧封存源在自己的 WEFT_SOURCE_DIR 重新构建；前两项及更早 seals 校验保持，原文件 101 项核对无 drift。当前新封存覆盖 663 个源文件（含 source-snapshot）与各配置 binaries；完整命令、报告和失败探索均保存。

环境：Windows 11 26200 / Ryzen 9 9950X3D / RTX 5090 driver 32.0.16.1062 / MSVC 19.42 C++20 Release + D3D11 debug layer；3840×2160 surface、1920×1080 shell，34 scene VM / 560 mount 节点、2 个 Canvas VM。shell 固定 Roboto/Noto CJK/Mplus；scene 使用本机 Microsoft YaHei UI(system-ui)/Arial/Consolas/Emoji，图片、原例与 bundle 固定。240 固定 60Hz owner 帧 / 16 warmup / 224 样本，无 readback，最后 drain 实际 query；三交替对，默认与两版同 affinity 0xFFFF/High 子进程条件分开。构建/测试停止后串行采样。

CPU tick 包含确定性等待 owner、main shell、producer 与提交，不等于游戏主线程占用；独立 async probe 的 game queue call 才测发送请求/输入的同步耗时，也不包括实际游戏逻辑。GPU producer+UI elapsed query 含 queue 中 CPU 空隙与 owner wait，本轮包括 Canvas producer，之前口径未完全包括 Canvas；不是纯 GPU busy time，不能相减声称 GPU 节省。同机同负载 Ultralight 对照、实体输入、总 VRAM、冷首帧尖峰、长时间功耗/调频、实际引擎时延、移动配置/编译/链接/模拟器/真机均未验证。

下一步按实际游戏的 owner + render + GPU + 输入时延预算继续收敛，减少完整 scene/诊断字符串和 encoded conversion，扩大 bounded clip/字体 atlas；实际引擎接入与移动 adapter 分级验收。
