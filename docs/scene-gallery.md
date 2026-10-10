# 固定 gallery：26 CSS + 8 D3

2026-10-09。34 个未修改原例已通过自己的 QuickJS/native DOM、owner scene layout/animation、Windows 软件参考/GPU 矢量 raster、D3D11 合成；不是只验证 DOM 输出。原游戏保持只读，原始函数、React/D3 和分组 bundle 不改。新增 34 例在 4K/FPS gallery 替换诊断卡，与已有两个 Canvas 共 36/72 native，其他 36 例仍标缺口。

可见窗口现默认走宿主 D2D GPU target → linear RGBA16F external lease，CPU WIC 路径保留；下方早期软件证据的 seal 不变。增量缓存、字体缓存、三版本资源池、同负载 before/after 与本次测试见 [性能阶段](gallery-performance.md)。

## 运行与证据

```powershell
cmake --build --preset windows-release --parallel 8
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/complex-css/interactive
```

默认 CSS；侧栏 D3 可直接跳转，滚轮/PageDown 查看全部 26+8。CSS 4–6 悬停触发 transition，CSS 7 点击改变进度宽度；其他原例继续按原时钟播放。完整窗口说明见 [gallery preview](gallery-preview.md)。静态和动画图片、原始错误/geometry、配置/二进制 hash 存在 `artifacts/local/gallery-css-d3/`。实际全量测试/GC 结果见 [status](status.md)，最终 seal 为该目录 `run-metadata.json`。

```powershell
ctest --preset windows-release --output-on-failure
ctest --preset windows-debug --output-on-failure
& .\build\windows-msvc\bin\Release\weft_scene_gallery_tests.exe artifacts/local/gallery-css-d3/release/native
python tools/compare_scene_geometry.py artifacts/local/gallery-css-d3/release/native tests/fixtures/scene-gallery-reference
python tools/verify_scene_fixture.py
```

## 原样源码与 utility 编译

固定源在 `tests/fixtures/crpg-gallery/UI/src/components/debug/TestGallery.tsx`，React/D3 原 bundle 在 `examples/crpg-gallery/native/`。`tools/compile_gallery_scene_styles.py` 直接从完整原 `gallery.css` 按 source order 选取 34 函数实际引用的 123 个 utility；静态 theme var、16px root rem、固定 calc 算术与逻辑边距转为具体 CSS，透明 color-mix 转为等价 sRGB rgba，保留全部 29 个原 keyframe block。没有逐例写替代图，也没有从浏览器捕获 computed style 作为运行输入。

`scene-style-manifest.json` 固定 compiler/source/output SHA256。普通构建不运行 compiler、不安装包、不读原游戏、不联网。这是固定 sheet 的显式适配器，不承诺一般 Tailwind、层叠 layer/media/container queries 或动态 theme 语义。窗口的目录由作者 shell 保持原 72 标签/10 组/列数/内容高度，完整原始 Grid 页面尚未实现。

## Owner 与布局/动画

SceneSolver 在 owner 上复用公共 cascade 的单独 `scene_syntax` profile：escaped ASCII class、SVG namespace/presentation attributes 和声明来源；旧 auto/positioned/flow profile 不扩张。Yoga 进行有界 border-box block/flex row/column、absolute/inset、px/%、边距/padding/border/gap 和文本固有度量。transform 正确建立 absolute containing block。混合 inline 内容、其他 formatting 值明确诊断；scene 不替代已经独立验证的 flow 换行/滚动路径。

关键帧逐属性补 underlying 端点，支持延迟、整数/infinite 次数、normal/reverse/alternate/alternate-reverse、fill、linear/ease/cubic-bezier/end steps。transition retarget 从当前呈现值开始；继承属性随动画传播，显式 child declaration 不被覆盖。支持当前例需要的 numeric/transform token 插值、px/% 圆角混合和 premultiplied sRGB 色值插值；复杂 transform decomposition/完整可动画属性清单未实现。节点状态按 view_epoch/slot/generation 保存，移除后清理，重新插入重启。

`setInterval` / `clearInterval` 使用 timeout 的共同 ID/取消空间，function-only、最大 4096 pending / 1000 ready。delay 和 cancel ID 采用 WebIDL signed-long 转换；interval 最小 1ms 是显式宿主策略。owner 暂停后每次只调一次、重新计 due，不追赶历史机会；异常不吞掉。完整 HTML timer nesting/4ms clamp、字符串 callback 不支持。D3 axis 使用新增 native descendant query `.tick line` / `.tick text`（最多 16 段 tag/id/class compound）；子代/list/attribute query 仍拒绝。

同步 geometry 从最新 copied DOM 求场景布局和累计变换 AABB，测量/布局不得回调 JS。HTML hit 逆变换后按 z/source order 与矩形 overflow clip 选节点，mouseover/out 的 relatedTarget 通过代际身份派发；本批悬停矩形/点击进度条已验证。圆角 hit、SVG descendant hit/geometry、完整 stacking context 和通用 D3 Interactive 仍在范围外。

## Windows 软件矢量参考适配器

文字测量、塑形与几何不依赖 Windows（重构第 6 步，2026-10-10）：
- `SceneText`（`src/scene/scene_text.*`）按 CSS Fonts 4 从 `FontCatalog` 选字体，用 HarfBuzz/FreeType 塑形，按 Blink 规则逐词塑形、按 cluster 加 letter-spacing、宽度向上取整到 1/64 px，并构造 Blink 的单行 line box。
- 字体列表是数据：Windows 上由 `weft::platform_win32` 的 `system_font_catalog()` 用 DirectWrite 枚举已安装字体（文件、TTC 索引、可变轴、本地化与 Win32 族名），其他平台可由宿主提供。
- 测试与探针使用本机 Chrome 155 实际的 generic 映射：sans-serif → Noto Sans SC，serif → Noto Serif SC，monospace → NSimSun，system-ui → Microsoft YaHei UI；最后依次回退 Noto Sans SC、Segoe UI Emoji、Segoe UI Symbol。
- 浏览器 CDP 的 platform-font 审计和对应 OS font hash 保存在 reference manifest；没有下载/复制 OS 字体。不同机器的字体设置可使 fixture 对照失败，跨平台固定字体打包未完成。
- `scene_text_metrics` + `scene_text_browser_diff` 把 852 个 span 与 Chrome 对照：字体选择一致，宽度差不超过 1/64 px，高度与基线完全一致。154 个合成粗体例只报告不要求（未实现 font-synthesis）。
- `SceneGeometry`（`src/scene/scene_geometry.*`）计算元素盒与命中，不用 D2D：精确的 Bézier/弧极值包围盒，nonzero/even-odd 填充，以及含端点与虚线的描边包含。

`SceneRaster`（D2D）仍是 Windows 参考 raster：它用同一字体文件/索引/轴值的 DirectWrite face 绘制 `SceneText` 的 glyph run，彩色字形（COLRv1）走 DirectWrite layout。几何与命中和可移植实现做差分：102 帧 369 个元素 rect 最大差 8e-6 px，337,920 个命中点中 234 个边界点不同（D2D 把 0.25 DIP 内的点算作内部）；cluster 210 帧 7,477 个元素 rect 最大差 0.0287 px（D2D 曲线包围盒是近似值）。

Direct2D SOFTWARE/WIC 绘制圆角、分边 solid border、线性/径向/锥形 gradient、2D transform、HTML 子树 group opacity 和 clipping。3D 仅支持本例的 `perspective(px) rotateY(angle)` planar subtree：先 raster 子树，再 inverse project/bilinear premultiplied sampling；不是通用 CSS 3D。SVG 为 g/rect（rx）/circle/line/polygon/path/text，支持 transform inheritance、fill/stroke/width、anchor/central baseline/dy，path M/L/H/V/C/Q/A/Z 的相对/重复参数（262144 byte / 65536 segment bound）。其他元素/命令、viewBox/preserveAspectRatio、clip-path/mask/filter、SVG descendant opacity、text stroke 明确诊断。

每张 backing 最多 16,777,216 pixels，scale <=4；owner viewport <=4096。adapter 输出独立 straight encoded-sRGB RGBA8，原 D3D11 图像上传转换为 linear premultiplied，按真实 GPU completion 退休。本批 case 本体在 encoded sRGB 软件合成，与浏览器语义匹配；不扩大全局 renderer 的 linear 合同。

SceneSolver/SceneRaster 是 M0 内部 C++ 探针，不是新增公开 STL/C ABI。平台 API 隔离在独立 target，Windows raster 不能推断 Android/iOS 可用。Windows scene GPU adapter 已完成本固定范围验证。原生 GPU pass 的文字图像改由 `scene_text_image`（FreeType 覆盖率，与 D2D 共用 `scene_text_origin` 定位）生成，只有含彩色字形的文字和显式 layer 仍用 D2D raster。跨平台 GPU adapter、生产 paint IR 与稳定 scene API 仍待实现。

## 独立对照与反例

真实 Chrome 155.0.8059.39，Windows 同机字体，完整原 CSS/原 bundle，CSS body 387×288 / D3 body 384×384，DPR 2；seed 0xce202610。先 drain 初始化 timer（不额外推进 D3 rAF），构造后 step(0)，每例再跑 121 帧，0–2000ms、60Hz。CSS 4–6 在 300ms 悬停、1200ms 离开；CSS 7 两个时点点击。浏览器真实 CSS animation/transition 以 WebAnimations 暂停采样，reference clock 与 native owner 分开实现。

每例截取 frame 24/63/120（400/1050/2000ms），共 **102 张 PNG**。每 RGBA channel 的 mean absolute error 必须 <=3 个 8bit 色阶；相对底色的前景经 3×3 [1,2,1] soft mask 后 IoU >=0.75，参考前景至少 200 pixels。字形覆盖（DirectWrite 或 FreeType 对 Skia）有容差，不能称 byte-identical。每例还把空底色图送入 oracle，34 个空图全部必须拒绝，避免低平均误差掩盖缺失内容。

HTML 元素与 SVG viewport 的真实 getBoundingClientRect 共 **1,476 字段**，每字段 <=0.3 CSSpx；实际最大 **0.274975**。SVG descendant 尚不在同步 layout geometry profile 内，本轮 path/轴/文字靠独立图像 oracle 检查。D3 animated bars 与 force 的图像必须随帧改变；CSS 动画/transition 的对应原例也检验 changed pixels / React input 状态。

合约另验证 implicit border-color 终点、动态继承/显式子颜色、delay/forwards、detach/reinsert、反向交替、owner 线程拒绝、变换 hit/像素、卸载 VM 后 scene/bitmap 保持有效，以及 **120 轮随机 style/class/移除/重插入/重排**的持久场景与全新 full scene layout/像素差分。每例 unmount/reset/GC 后 native nodes 回到 3；两帧资源完成与窗口输入由实际 GPU query/readback 验证。

## 性能与后续

34 个独立 VM 保留运行状态，8 个 D3 的 timer/rAF 在滚动离屏时继续运行；CSS time/timer 继续，重新可见时按当前时间取样。当前只对可见 body 做 D2D GPU 绘制与 linear surface 发布，静态 scene/纹理与完整 shell packet 复用。原两例 Canvas 的软件 backing 与离屏 rAF 暂停策略继续保留。

首次软件阶段的 CPU raster/动态完整图像上传昂贵，尤其同时显示 16 个 CSS body（2x）时；旧 seal 与带截图的正确性统计仍保留。性能阶段新增独立无回读基准，同机 4K 首屏/中段/D3 从 14.09/5.22/21.91 提升到 117.56/55.14/128.39 FPS；各分位数、启动与内存见 [完整性能证据](gallery-performance.md)。cache/import 预算和进程 working set 不能作为总 GPU 内存。完整原72例、实体输入延迟、Ultralight同负载对照和移动仍未验收。

后续已接显式翻转平面的 GPU 采样、圆环/side wedge 和动画计划缓存，见 [复杂 CSS 阶段](complex-css-performance.md)；下一步继续降低 scene/group isolation 成本；另四个 D3 Interactive 需要 SVG geometry/hit 与 drag/zoom/brush、clipPath/selection 等按实际阻断扩展。不能把目前8例直接外推为任意 D3 应用支持。
