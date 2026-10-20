# Weft 当前状态

更新日期：2026 年 10 月 21 日。当前是 **M0 部分交付**：C++20 核心和风险探针已落地，真实 React/Vite 页面已贯通 native DOM、inline CSS、Flex、字体与 D3D11，并接入 HWND/swapchain 主鼠标窗口。flow 的 inline/Unicode 换行、同步几何/滚动和圆角裁剪现已接上 paint-order/分片命中及 HWND wheel 消息；可见样例使用 4K/FPS、有界固定资产缓存和最多两帧在途，滚动吞吐采样已完成，并增加可见的连续 DOM 运动与 30 Hz 对照。一般 HTML/CSS 页面加载、通用 SVG、键盘/IME 与移动尚未实现，仍未达到 M0 全部退出条件。

## 待办

用户决定暂缓、之后一起处理的事项：
- **交互性能优化**（2026-10-13，11.3 之后一起看，数据见 [交互剖析](gallery-interaction-profile.md#悬停扫动与-scene-图像缓存2026-10-13)）。2026-10-14 起按第 12 步的顺序做（[计划](refactoring-plan.md#第-12-步计划2026-10-14交互性能优化)），三段均已完成。剩下的图表热点用户定为低优先级（2026-10-14：图表特有的问题由业务侧负责）：
  - 12.3 之后：K 线与散点图的场景 pass 每帧绘制 58 → 9、30 → 3 次；隔离基准（固定 CCD）D3D11 K 线渲染 CPU p50 0.91 → 0.83 ms、p99 1.73 → 1.34 ms，散点图 p50 0.78 → 0.66 ms；画廊窗口 K 线重绘 p50 0.99 → 0.83 ms；
  - 剩下的（两个场景分开采样）：K 线主要是虚线十字线每帧重新光栅化（约 32%）与它们的大纹理（约 10%），画成一串盒子估计省约 40%；散点图是刻度文字的字形轮廓随平移重画（约 41%）与属性变了的元素重新解析（显示列表约 50%），路径按形状与亚像素位置缓存估计省约 35%，只重新解析变了的属性约 15%；SVG 值缓存的逐项校验只占 2–7%；
  - 半透明窗口（11.5c，4K）：32 个窗口时 owner 约 2 ms、渲染 CPU 2.3–2.9 ms，随窗口里的元素数增长，三种窗口相近；背景拷贝本身 GPU 约 0.5–0.8 ms；
  - 低优先级，暂不做。
- **与引擎通信**（2026-10-14 起，第 13 步，[计划](refactoring-plan.md#第-13-步计划2026-10-14与引擎通信)，[技术路线](engine-communication.md)）：血条这类每帧变位置的元素不靠每帧推 JSON（锚点在渲染线程取最新值），双向事件、带 Promise 的引擎调用与视图模型，C ABI 与 CRPG UI 的兼容层。基线：200 个血条每帧约 13 ms，页面自己算出同样的值（零传输）也一样慢，瓶颈在逐个改 DOM 之后的样式、布局与场景更新。13.1 已完成（2026-10-14）：400 个名牌只移动时 UI 线程每帧约 0.005 ms、渲染 CPU 约 0.02–0.04 ms（推 JSON 时分别约 23 ms 与 4 ms）。13.2 已完成（2026-10-14）：带类型的值、双向事件、调用与 Promise、同步函数、视图模型与 CRPG 兼容层；每秒约 1,000 个 10 KB 的事件时 UI 线程每帧 0.79 ms（CRPG 的拼脚本求值约 4.5 ms）。13.3 已完成（[计划](refactoring-plan.md#第-13-步记录2026-10-14)）：13.3a 对外 View 与它的 C ABI、13.3b 渲染 C ABI 接 View 的帧与模拟引擎的宿主样例（2026-10-14），13.3c 页面显示引擎的纹理（2026-10-15，用户提出，如 UE 的 render target）。13.4 已完成（2026-10-15）：CRPG 原样的浮动 UI 跑在 scene 上，200 个单位时锚点方式的 UI 线程每帧 p50 29 ms、推 JSON 191 ms（剩下的是动画每帧改 DOM 后按整个文档的计算，13.5）。引擎纹理的限制已做（下一条）。13.5 增量管线已完成（2026-10-15）：13.5a owner 侧只处理变化的节点，13.5b 渲染侧只重做变化的子树，13.5c 每个元素的开销与局部布局。400 个单位（约 1,600 个节点）每帧改 1 个时 owner p50 / p99 5.3 / 12.1 → 0.111 / 0.194 ms，渲染 CPU 4.1 → 0.043 / 0.078 ms；400 个只移动 owner p99 3.7 ms；锚点加 20 个血条改宽度 owner 10.7 → 0.91 ms。
- **引擎纹理的限制**（13.3c 之后，用户 2026-10-15 定为中优先级）：2026-10-15 做了铺排（`background-size`、`-position`、`-repeat`、`-origin`、`-clip`）、sRGB 与 BGRA 视图、mip、D2D 参考光栅、Vulkan 的目标别名检查、D3D11 上别的设备或 API 共享的纹理（见下方“本次已实现”）。剩下：文档页面（整体没有背景图片）、Vulkan 的跨 API 导入（未测）、UE 插件（M5；2026-10-15 起在 `unreal` 分支与 weft-unreal 仓库做，D3D12 后端已完成，2026-10-17 合入 main，见下方“本次已实现”）。
- **Android 示例应用与模拟器、真机运行**（2026-10-12）：需要下载 Android SDK 命令行工具、模拟器与系统镜像；NDK r30 已安装，交叉编译已验证。

## 本次已实现

View 的命中图一帧只建一次，只建 View 上能命中的部分（2026-10-21，`weft-op` 分支；宣传页的 Unreal 版在 4K 下指针在页面上时游戏线程每帧多 2.6 ms）。
- **起因**：`View::pointer` 与 `View::hit` 每次调用都为整个 scene 建一遍命中图（`SceneGeometry::hit_map`：每个元素的命中区域、变换的逆、圆角与裁剪），同一帧的命中测试与指针移动各建一次。宣传页约 4,000 个元素，缩放 2.25 时每次约 1.3 ms；UE 的 Slate 在指针不动时也合成移动事件，每帧一两次。
- **实现**：
  - `view.cpp`：命中图按 scene 的 revision（每份内容唯一）、锚点表的 serial 与 View 的矩形缓存，一帧里的指针事件与命中测试共用一份；View 外的点（被捕获的指针）仍问整个 scene。
  - `scene_geometry.cpp` 的 `hit_map` 加 `within`（会被问到的点所在的矩形）：裁剪（`overflow: hidden`、`auto`）整个在它外面的盒子不列出后代，只列出其中的锚定元素（锚定元素越过祖先的裁剪），按绘制顺序；子树的命中范围（边框盒、行内分片与不被它裁剪的后代的范围，经变换；有锚定元素或投影变换的子树没有范围）整个在它外面时整个子树不访问。范围按子树的存储缓存（相等的句柄是相等的子树），条目只观察存储（`weak_ptr`）不持有：渲染器的缓存在只有自己持有子树时才释放，持有会让两边都放不掉（第一版这样写，4K 下渲染线程每帧涨到 6–12 ms）。
- **验证**（Release）：
  - `scene_contract` 加 `hit_within_differential`：用变换滚动的长页面，被子元素溢出的裁剪盒、越出不裁剪的 section 的子元素（定位的与变换的）、旋转缩放、行内文字、svg、z-index、`pointer-events: none`，远处裁剪盒里的两个锚定元素被锚点带到 View 上；120 步随机滚动、显示与隐藏、改 overflow、移动、写锚点，长期存在的 `SceneGeometry` 带 within 的命中图与新建的整帧命中图在 View 上每 3 px 的点都命中同一节点，建图不改变子树的引用计数；四分之三以上的帧列出的区域更少，四分之一以上的帧命中了远处的锚定元素。去掉对锚定后代的处理或子树范围里的后代时这个测试失败。
  - `view_contract` 加 `hit_cache`：脚本改了 DOM、指针事件的处理函数移动了元素之后，没有 advance 的命中测试命中新的位置；View 外的点照样命中。
  - 页面运行器（缩放 2.25，每帧一次命中测试加一次指针移动）：命中测试 1.3 → 0.19–0.31 ms，随后的指针移动 1.3 → 0.02 ms。宣传页的 Unreal 播放器 3840 × 2040（D3D12，trace）：HUD 处游戏线程的命中测试与指针移动每帧 2.58 → 0.48 ms。全量 ctest 176 项：除既有的 14 项外通过。

D3D12 与 Vulkan 的槽保留上次录制用过的暂存纹理（2026-10-21，`weft-op` 分支；宣传页 HUD 在 4K、D3D12 下渲染线程每帧 6.7 ms）。
- **起因**：槽在录制结束时把暂存纹理（组、毛玻璃的链）留给下次，但按 64 MiB 的字节上限留：4K 下两层全尺寸的组纹理（各 31.6 MiB）加毛玻璃的链就超过上限，超出的纹理销毁，下一帧再建。宣传页的 HUD 每帧新建约 14 个，D3D12 上每个 0.19 ms。
- **实现**：`d3d12/scene_renderer.cpp` 与 `vulkan/scene_renderer.cpp` 的 `release`：本次录制用过的暂存纹理全部留作备用（下一次多半画同样的 pass），以前留下没用上的备用在数量与字节上限内保留。
- **验证**（Release）：`render_vulkan_scene_tests` 加：4096 × 2304 的目标上两层嵌套的组（共约 75 MB，超过上限），同一槽的第二、三次录制不新建对象；旧的做法下每次新建两个。宣传页的 Unity 播放器 3840 × 2040（D3D12）：HUD 帧时间 p50 6.68 → 4.17 ms（240 fps 上限），渲染线程 5.7 → 1.0 ms。全量 ctest 176 项：除既有的 14 项外通过。

到这里，宣传页的 Unity 播放器在 3840 × 2040（缩放 2.25）与 1600 × 900、D3D11、D3D12、Vulkan 上，四个停靠点与连续滚轮滚动的帧时间 p50 都是 4.17 ms（`targetFrameRate` 240 的上限），4K 滚动 p95 ≤ 4.8 ms、最慢 ≤ 10.7 ms；修复前 4K 是 5 fps。

宽模糊的盒阴影与文字阴影在粗网格上模糊（2026-10-21，`weft-op` 分支；宣传页滚到新的区域时一帧 100–270 ms）。
- **起因**：盒阴影在 CPU 上做：逐像素算到圆角盒的距离（`hypot`）作形状，可分离高斯（核宽六个标准差：缩放 2.25、模糊 60 px 时约 400 个系数），再逐像素算一次距离切出盒外。340 × 274 的卡片、60 px 模糊的阴影（1040 × 892 px）要 266 ms，大卡片 1 px 不模糊的环也要 18 ms。文字阴影的模糊同样用全分辨率的核（一行标题 47 ms）。
- **实现**：`shadow_image.cpp`：标准差超过 `shadow_coarse_sigma_px`（6 设备 px）的盒阴影模糊在粗到标准差为 6 的网格上做，再双线性读回；盒的覆盖只在边上半个像素宽的带里算距离，其余是 0 或 1；外阴影跳过盒里的像素（结果为 0）。`gaussian_blur`（文字阴影）标准差至少 12 时按 n × n 的块取平均，在块上模糊，再双线性读回。
- **验证**（Release）：`paint_contract` 加：与全网格的高斯相比，外阴影与内阴影每个像素差不超过两级（8 位 alpha），块上模糊的类文字形状也不超过两级。页面运行器（缩放 2.25）：阴影图像每个 17.6 → 0.9 ms，最慢 266 → 5.1 ms；滚动巡览里超过 50 ms 的帧只剩第一帧。浏览器对照的场景测试照常通过。全量 ctest 176 项：除既有的 14 项外通过。

平移整像素的路径复用图像（2026-10-21，`weft-op` 分支；宣传页滚动时 SVG 图标与图表的路径每帧重新光栅化）。
- **起因**：路径覆盖图像的键含设备映射的平移和掩码的绝对位置，页面滚动时每个可见路径的键都变，每帧重新光栅化（4K 的滚动里每次准备约 27 条）。掩码又被裁剪与目标切开：根的裁剪就是目标，跨在目标边上的路径滚动时每帧掩码不同。
- **实现**：`scene_native.cpp` 的路径键取平移超出整像素的部分（`path_key_subpixels` 分之一像素一步）与相对整像素的掩码：整像素平移的路径覆盖的是平移了的同一图像。`scene_pass.cpp` 的 `path_mask`：被裁剪或目标切开的路径，掩码取路径的整个范围（目标四边各外延一个目标尺寸，在光栅器的上限内），记录的裁剪切出显示的部分；超过上限时仍是切开的掩码。
- **验证**（Release）：`scene_display_cache_contract`：整像素平移（缩放 2 时平移 1）复用两条图像，分数像素平移重建；svg 视口变窄画同样的图像；跨在目标右边上的 svg 整像素移动复用路径图像。全量 ctest 176 项：除既有的 14 项外通过。

长页面只列出能显示的部分，隐藏元素的动画不运行（2026-10-21，`weft-op` 分支；宣传页的 Unity 版在 4K 下渲染线程每帧 6–7 ms，主线程 3–6 ms）。
- **起因**：
  - 每帧都整页准备：保留的 pass 的补丁每帧被拒（`a node without a span`）。宣传页的 Unity 版用 `display: none` 隐藏了 Web 版手绘的场景，其中的 CSS 动画（湖面的四道波光、26 颗星）每帧仍在采样，变了的节点在没有列出的子树里，没有 span。
  - 整页准备每帧列出约 1,800 项（缩放 2.25 时 6–7 ms）：显示列表逐节点解析边框、颜色、阴影的样式字符串，与节点在不在目标上无关。
  - 隐藏的动画每帧改 scene（每帧约 128 次类型化采样），增加求解器的样式与构建工作。
- **实现**：
  - `scene_display_list.cpp`：子树的范围（extent），在节点自己的 px 里：边框盒，文字行（留出一行高给超出行盒的字形、斜体外伸与文字阴影），盒阴影（偏移、扩展，模糊三个标准差，加 guard，同 `shadow_geometry`），轮廓，子节点的范围经子节点的变换（行内盒不变换）。有锚定元素、粒子、透视平面或滤镜的子树没有上限，不剔除。访问框架里的节点时，范围加 `list_cull_guard_px` 后与继承的裁剪不相交的子树不列出（span 为空），与节点画在目标外时 GPU 本来也画不出任何像素一致。
  - 范围按子树的存储缓存在 `SceneDisplayCache`：缓存持有存储（`SceneChildren` 相等的句柄是相等的子树，写时复制不会原地改它），没变的子树不再遍历；只有缓存还持有的条目在每个 pass 开始时释放，旧版本的节点随帧放下逐个释放（一次释放几千个节点会卡 40 ms）。
  - `scene_native.cpp` 的 `changed_parts`：span 里没有项的节点（`display: none`、没有盒、超出裁剪）下面没有 span，其下有变化时把它整体重新列出，不再拒绝补丁：仍在裁剪外时列出 0 项、尺寸相同，进了裁剪时尺寸不同、整页准备。
  - `scene.cpp`：元素不显示（自己 `display: none`，或在这样的元素里）时不运行过渡与动画，值是目标值；再显示时动画从头开始（CSS Animations 1 §3、CSS Transitions 1 §3）。
  - `scene_list_culls()`：本线程建的列表是否剔除；测试的参考关掉它。
- **验证**（Release）：
  - `incremental_contract` 加 `cull`：高四个目标的页面在目标里滚动（整像素与分数像素），文字、阴影、颜色、可见性随机变化，隐藏元素里与目标外各有动画；剔除并跨未列出的子树打补丁的渲染器与列出全部子树的渲染器 240 帧逐像素相同（72 帧滚动，134 帧补丁）。范围漏算盒阴影的模糊时第 111 帧不同（卡片在目标上方、阴影的下缘在目标里）。
  - `incremental_contract` 加 `hidden`：增量与完整求解器里，`display: none` 的元素里的动画不让帧变成动画帧、帧内容不变（增量的 revision 也不变）；显示后动画从第一帧开始，100 ms 后在 25%。
  - `scene_native_contract` 的图集用例里不画任何像素的毛玻璃移到根的角外 guard 之内（更远就不列出了）。
  - 页面运行器（缩放 2.25，D3D11，Unity 式的战斗模型每两帧更新）：首屏 paint 6.6 → 0.4 ms/帧，渲染 CPU p50 6.5 → 0.4 ms；22 屏的滚动巡览渲染 CPU p50 6.6 → 0.7 ms；类型化采样每帧 128 → 88。全量 ctest 176 项：除既有的 14 项外通过。

画不到目标上的文字与阴影不建图像，图像缓存保留上一 pass 的值（2026-10-21，`weft-op` 分支；weft 宣传页的 Unity 版在 4K 显示器上最大化时只有 5 fps）。
- **起因**：原生 pass 的准备器给整页建文字与阴影图像：宣传页约 18,000 px 高，缩放 2.25（3840 × 2040 的窗口）时一个 pass 的工作集约 826 个图像、128 MiB，超过每个缓存 32 MiB 的上限。缓存只保护本 pass 用过的值：本 pass 新建的值把上一 pass 的、本 pass 还没轮到的值挤出去，后面再轮到时重建，又挤出下一批，每个 pass 重建 450–660 个文字图像（Unity 渲染线程上 90–240 ms）。缓存的注释写的是复用上一 pass 所建的“全都找得到”，实现没做到。
- **实现**：`scene_native.cpp` 的 `reaches`：记录的范围（项在裁剪内的范围）与目标不相交、又不随锚点移动的文字与阴影项不建图像（`prepare` 与 `patch` 都是），与掩码为空的路径不建图像相同。`scene_image_cache.hpp` 只淘汰上一 pass 之前用过的值：一个 pass 新建的值不再挤掉上一 pass 的。
- **验证**（Release）：`scene_native_contract` 加：同一页面里目标之下的文字与阴影没有图像，移进目标后有；超过条目上限的 pass 新建一个图像时，上一 pass 的其他图像全都复用（修复前重建）；上一 pass 的图像在下一个 pass 被淘汰；已淘汰但仍被持有的图像的图集区域不被占用。`scene_image_cache_contract` 按新约定改写（上一 pass 的值越过上限也保留，再下一个 pass 才淘汰；使用顺序在都可淘汰时决定去留）。页面运行器（缩放 2.25，D3D11）：渲染 CPU p99 234 → 12 ms，最慢 1,609 → 84 ms（第一帧）；Unity 播放器 3840 × 2040（D3D11）帧时间 p50 158–223 → 7.6–8.9 ms。全量 ctest 176 项：除既有的 14 项外通过。

D3D11 的毛玻璃在无类型（typeless）的目标上（2026-10-21，`weft-op` 分支；上一条日志里的失败）。
- **起因**：`backdrop-filter` 拷贝下方内容时，按目标纹理资源的格式建复制纹理，再用默认视图建它的 RTV。引擎的渲染纹理是无类型的（Unity 的 BGRA8 渲染纹理是 `B8G8R8A8_TYPELESS`，由视图选编码），无类型纹理没有默认视图，建 RTV 失败（E_INVALIDARG），场景 pass 抛出，那一帧不画。复制纹理按尺寸缓存，所以只在毛玻璃出现新尺寸（滚动时被裁剪）时失败：宣传页巡览一次约 6 帧。
- **实现**：`d3d11/scene_renderer.cpp` 的 `backdrop` 按目标视图的格式建复制纹理，与读背景的效果的 `copy` 相同。
- **验证**（Release）：`render_view_tests` 的 D3D11 帧加：带毛玻璃的名牌页面画进 `B8G8R8A8_TYPELESS` 的纹理（视图 `B8G8R8A8_UNORM`），与画进有类型纹理的结果逐字节相同；修复前绘制失败（`WEFT_RENDER_BACKEND_ERROR`）。宣传页的 Unity 播放器（D3D11，1600 × 900）巡览里失败的帧 6 → 0。全量 ctest 176 项：除既有的 14 项外通过。

视图后端说出场景 pass 抛出的原因（2026-10-21，`weft-op` 分支；weft 宣传页的 Unity 播放器在 D3D11 上偶尔整帧不画，日志只有 `BACKEND_ERROR`）。
- **起因**：三个视图后端的 `encode` 里场景 pass 抛出的异常由 C 接口的转接层变成 `WEFT_RENDER_BACKEND_ERROR`，`weft_renderer_last_failure` 只说已知的失败，宿主的日志只有错误码。
- **实现**：D3D11、D3D12、Vulkan 的视图后端捕获 `std::exception`，把 `the native pass failed: <原因>` 记为最后的失败再抛出，结果不变。
- **验证**：没有让场景 pass 抛出的注入点，未加自动测试。weft-unity 的插件把原因接进日志之后，Unity 播放器（D3D11）的那几帧显示为 `the native pass failed: native backdrop RTV HRESULT=0x80070057`（下一条修了它）。全量 ctest 176 项：除既有的 14 项外通过。

跟着时间变的元素里的文字与阴影不再重排（2026-10-20，`weft-op` 分支；weft 宣传页的 Unity 版在页面运行器里滚动并淡入时卡顿）。
- **起因**：元素在过渡或动画中（滚动淡入的 `opacity`、`transform`）时每帧重建，它混排内容里的文字段也跟着重建；新建的文字段没有布局结果，与上一帧的永远不等，于是每帧标记布局。文字段在正常流里，局部布局一直找到最近的另行布局的盒子，通常是整页：淡入的一秒多里每帧约 25 ms（1600 × 900，约 3,000 个节点）。文字段只变了颜色（继承的 `color`）也重排。另外 `box-shadow`、`text-shadow` 不在布局签名的忽略表里，冷却图标的光晕一切换就整页布局。
- **实现**：`scene.cpp` 的增量重建让新建的文字段带上旧文字段的布局结果（`box`、`lines`、`fragments`、滚动位移），与元素在布局再跑之前保留布局结果的做法相同：没变的文字段与旧的相等、共用存储；变了的只在布局读的值变了时（`same_layout_own`：文字、字体等，不含颜色）才标记布局。`box-shadow`、`text-shadow` 加入布局签名的忽略表（布局从不读它们，显示列表与伤害区域在绘制时读）。
- **验证**（Release）：`scene_inline_contract_tests` 加：混排内容的段落在 `opacity`、`transform`、`color`、`box-shadow`、`text-shadow` 都在动画时，第二帧复用布局，文字段的行不变、颜色跟着动画；文字段的文字改了时重新布局。去掉文字段的修复或阴影的修复各自失败。页面运行器里宣传页的滚动加淡入（760 帧，1600 × 900，D3D11）：超过 8 ms 的帧 141 → 10，`advance` p99 64.7 → 10.4 ms，最慢 138.9 → 33.4 ms。全量 ctest 176 项：除既有的 14 项外通过。

`white-space: pre` 等之下混排内容的空白（2026-10-20，`weft-op` 分支；weft 宣传页的 Unity 版里，高亮的代码块挤成了一行）。
- **起因**：元素的混排内容（元素与文字节点相间，如高亮的一行代码）里的文字节点建成文字段时，空白总按 `normal` 折叠：`pre`、`pre-wrap`、`break-spaces` 下的空格与换行、`pre-line` 下的换行都丢了，代码块的几行挤成一行。元素自己的文字（没有子元素时）一直按 `white-space` 处理。
- **实现**：`scene.cpp` 的 `text_run` 只在 `white-space` 为 `normal`、`nowrap` 时折叠空白，其他值保留原文，交给行内布局按 `white-space` 断行（与元素自己的文字相同）。
- **验证**（Release）：`scene_inline_contract_tests` 加：`white-space: pre` 的块里一个 `<span>` 与它后面的文字节点，文字段的两行是 `" x  = 1;"` 与 `"let y = 2;"`，第二行在第一行之下；修复前失败。全量 ctest 176 项：除既有的 14 项（签出换行的散列校验）外通过。

SVG 文字的 `fill-opacity`（2026-10-18，`weft-dev` 分支；CRPG 的 d20 检定揭晓叠层按面的朝向让骰面数字渐隐，背面的数字设为 0）。
- **起因**：显示列表与参考光栅给 SVG 文字取颜色时只读 `fill`，`fill-opacity` 只乘到形状上。骰子上从未转到正面的数字停在 svg 原点、`fill-opacity="0"`，仍被画成一团字；斜面上渐隐的数字也是不透明的。
- **实现**：`scene_display_list.cpp` 把 `fill-opacity`（夹到 0–1）乘进文字的填充色，为 0 时不取字形轮廓、不出项；`scene_raster_d2d.cpp` 画 SVG 文字时同样相乘（SVG 2 §13.2，与 Chrome 一致）。
- **验证**（Release）：`scene_cluster_platform` 的 SVG 用例加：标签 `fill-opacity: 0.5` 时文字路径颜色的透明度为 0.5，为 0 时显示列表少一项，参考光栅在 0 时与没有文字的帧逐字节相同（有文字时不同）。核对过去掉参考光栅的修复时失败。全量 ctest 176 项：除既有的 14 项（签出换行的散列校验）外通过（`d3d11_warp_gallery_preview` 在 `-j 6` 下偶发失败，单独重跑通过）。CRPG 的骰子在页面运行器里不再有原点的残字。

CSS Grid 的轨道、放置与对齐（2026-10-18，`weft-dev` 分支；用户定的顺序：HTML `filter`、读背景的效果之后做 grid。CRPG 的 Pick 17 构筑页、Market 页与设置页色盲预览的网格都在等列子集之外）。
- **起因**：scene 布局的 grid 只有 1–8 个等宽 `minmax(0,1fr)` 列，其他写法抛 `WEFT_UNSUPPORTED_FORMAT: grid supports 1-8 equal minmax(0,1fr) tracks`，整帧失败，UE 里界面停在上一帧。测试关卡的战斗开局推出 Pick 17 构筑页（`UPick17BattleSchema`，网格 `repeat(auto-fill, minmax(320px, 1fr))`），从推入起每帧失败，第一屏画不出来（2026-10-18 游戏日志里发现；此前的游戏内检查在 6 秒时把界面换成了 HUD，没有看到）。Market 页的 `grid-cols-[auto_auto_1fr_auto]`、`grid-cols-[1fr_auto_auto]` 同样失败；设置页色盲预览的 `repeat(8, 1fr)` 也在子集外（滤镜那次发现）。子集里 `1fr` 被当成 `minmax(0, 1fr)`，网格项不拉伸到行高，`height: 100%` 的色块没有高度。网格修好之后，Pick 17 页又露出两处原先被它挡住的失败（见实现的最后一条）。
- **实现**：
  - 值（新文件 `src/layout/grid.hpp`、`grid.cpp`，只处理数值，与 Yoga 无关）：`grid-template-columns`、`-rows` 的 `none` 或轨道表：px、%、`fr`、`auto`、`min-content`、`max-content`、`minmax()`（最小值不能是 `fr`）、`fit-content()`、`repeat(<整数>, …)`（展开）与至多一个 `repeat(auto-fill | auto-fit, …)`（只重复固定尺寸，旁边的轨道也要是固定尺寸，同 CSS 语法）；`grid-auto-columns`、`-rows` 的轨道表；`grid-auto-flow`；放置属性的 `auto`、非零整数（负数从显式网格末端数）与 `span n`；自对齐的值。线名、命名线与区域、px/%/`fr` 以外的单位、语法以外的值抛 `WEFT_UNSUPPORTED_FORMAT: <属性> <值>: <原因>`（自对齐是 `WEFT_UNSUPPORTED_ALIGN`）。行号与跨度夹到 ±1000，显式轨道至多 1000 条，隐式网格至多 2^22 个格子，一次放置至多检查 2^26 次格子（`constants::layout`）。
  - 放置（§8.5）：先放两轴都确定的项目，再放锁定行的项目（稀疏时在同一行上次放过的之后），再定列数（确定的列号与最宽的跨度），最后按游标自动放置（稀疏，或 `dense` 每项从头找）；`column` 流交换两轴。确定的行号可以在显式网格之前生成隐式轨道，`grid-auto-*` 在那里倒序循环。放置按显式网格的大小缓存。
  - 轨道尺寸（§11.3–§11.8）：初始化；单轨道项目的内在尺寸（`auto` 最小值在确定尺寸下取最小贡献，在最小、最大内容约束下取受限贡献）；跨多轨道的项目按跨度分组的六个子步骤（分配到上限、超出上限、无限可增长标记、`fit-content()` 的上限）；跨弹性轨道的项目一起、只按 `fr` 比例增长弹性轨道；无限的增长上限归为基础尺寸；最大化；弹性轨道（确定尺寸下找一个 `fr` 的大小，基础尺寸更大的轨道当作不弹性；最大内容约束下取各轨道与跨弹性轨道项目的最大值）；拉伸 `auto` 轨道（内容分布为 `normal`、`stretch` 时）。`auto-fit` 没有项目的重复轨道折叠为 0，两侧的间隙一起折叠。百分比轨道在确定尺寸下解析，不确定时当 `auto`。`justify-content`、`align-content` 的全部分布值，`safe` 溢出时靠起点。
  - 项目的贡献：列方向取适配器的内在宽度（px 宽度就是它；否则内容的最小、最大内容宽度，夹在 px 的 `min-width`、`max-width` 之间，加 px 外边距）；自动最小尺寸（§6.6）在项目跨 `auto` 最小值的轨道且不是滚动容器时是最小内容宽度（所跨轨道都有固定最大值时再夹到区域），否则是 `min-width` 或 0。行方向取项目在其列宽下的 Yoga 布局（外边距盒的高度）。`align-*: baseline` 的项目按所在行的第一基线下移，下移量计入行高（§11.5 第一步）；基线取文字盒的第一行，或第一个有基线的流内子项，都没有时是边框盒底边。
  - Yoga 适配器（`src/layout/flex.cpp`）：网格节点仍是带测量函数的叶子。测量时按 Yoga 给的确定宽度、最大内容宽度或收缩适配（最小与最大内容宽度之间）算列，按确定高度或最大内容算行，返回内容尺寸；最终尺寸下再算一次并放置子项。每个网格子项有自己的 Yoga 根 `Node::area`：与网格区域同大的列方向 flex 盒，项目在其中按 `justify-self` 横向拉伸或对齐（Yoga 的 align-self），按 `align-self` 纵向拉伸（auto 高度且没有 auto 外边距时 flex-grow、flex-shrink 为 1）或用 justify-content 对齐；百分比尺寸与 auto 外边距由 Yoga 按区域解析。`normal` 对替换元素（svg）是 start。绝对定位的子项在网格是包含块时放在它的行号围出的区域里（`auto` 与跨度在内边距边），否则只取静态位置（内边距盒的起点），这时带 inset 是诊断。`display: inline-grid` 与 `grid` 同样布局。网格的内在尺寸（`intrinsic`）按最小、最大内容约束算列，行取项目在其最大内容宽度下的高度。`align-*`、`justify-content` 也接受 `start`、`end`、`left`、`right`、`self-start`、`self-end`、`safe`/`unsafe` 与 `first baseline`（映射到 Yoga 的 flex-start、flex-end；flex 容器同样受益），`justify-content: stretch` 是 flex-start。百分比宽度的替换元素在内在宽度里贡献 0 的最小内容宽度、它自然宽度（宽度属性，或默认 300px）的最大内容宽度（CSS Sizing 3 §5.2.2），flex 仍保留诊断。轨道表不受 128 字节的样式值上限（网格自己的上限 1024 字节）。`css::grid_repeat` 及它的差分测试删除。
  - 层叠（`src/style/style.cpp`）与 scene profile：新属性 `grid-template-rows`、`grid-auto-columns`、`grid-auto-rows`、`grid-auto-flow`、`grid-row-start`、`-end`、`grid-column-start`、`-end`、`justify-self`（严格与宽松页面）；`grid-row`、`grid-column`、`grid-area` 在层叠里展开成长写（省略的线是同一轴前一条线的自定义名，否则 `auto`；带 `var()` 是诊断，宽松页面丢弃）；宽松页面的 `place-items` 像 `place-content`、`place-self` 一样展开成 `align-items` 与 `justify-items`（此前它整条进层叠，flex 容器不读它）。
  - 宽松页面（`src/scene/scene.cpp` 的 `drop_grid`，同 `drop_filter`）：构建 scene 时用适配器的同一套解析检查网格属性与 `justify-*`，不接受的值丢弃并诊断（`WEFT_STYLE_DROPPED: grid-column-start header on <div> (named lines and areas are outside the profile)`），属性回到初始值（没有显式轨道、`auto` 轨道、行流、自动放置）；严格页面仍是诊断，保留上一帧。`scene_orders_children` 也认 `inline-grid`。
  - 网格修好后 Pick 17 页露出的两处（都在网格之外）：
    - flex 项目的自动最小尺寸（CSS Flexbox 1 §4.5）：基于内容的最小尺寸取内容与指定尺寸建议（px 尺寸，或布局前已知的高度的百分比）中较小的一个；内容尺寸未知时取指定尺寸建议，不再报 `WEFT_UNSUPPORTED_INTRINSIC`。布局前已知的高度（`known_heights`）：视口给出的根、px 高度、已知高度的包含块的百分比（绝对定位的盒按最近的包含块的内边距盒）、上下 inset 都是 px 且高度 auto 的绝对定位盒。px 尺寸的盒不再把里面未知的内在尺寸传给祖先（它对祖先的贡献就是那个 px）。此前 Pick 17 页 92vw 的外框（`height: 100%` 的纵向 flex 项目）因为里面 `h-full` 的技能图标（34px 框里的 svg）报 `replaced box without a px height`；即使不报，它的最小高度也是全部卡片的高度，滚动区不滚、页脚被挤出屏幕。
    - 宽松层叠把超出范围的数夹到 ±1e7（`constants::max_style_number_magnitude`，`calc(infinity)` 原本就夹到这里，CSS Values 4 §4.2）：Tailwind 4.1 的 `rounded-full` 是 `border-radius: 3.40282e38px`，原生 pass 拒绝这个数，有 `rounded-full` 的帧画不出来（`WEFT_SCENE_RASTER: numeric value 3.40282e38`；战斗单位面板、任务标记也用它）。
- **验证**（Release）：
  - `flex_browser_diff`：共享适配器夹具（`tests/fixtures/flex_layout_cases.json`）加 27 例网格——Market 的 `auto auto 1fr auto` 与带 `overflow: hidden` 的 `1fr auto auto`，Pick 17 的 `auto-fill` 与行内拉伸，`auto-fit` 折叠，`repeat(8, 1fr)` 在确定高度里拉伸的行与 `height: 100%` 的色块，`1fr` 的自动最小值与 `minmax(0, 1fr)`，百分比，`min-content`/`max-content`/`fit-content()`，稀疏与 `dense` 的跨度，确定行号与 `1 / -1`，负行号生成的隐式列，项目与容器的对齐，`place-items`，内容分布，flex 行里的最大内容宽度，绝对定位的收缩适配，`auto-fill` 按 `max-width` 与 `min-width`，基线对齐，盒模型，绝对定位子项与行号区域，`column` 流，`fr` 行，`minmax(30px, auto)` 行，`inline-grid`——和 2 例 flex（百分比高度的纵向项目：内容更高时保持指定高度，内容更矮时照常收缩）。参考由 Chrome 155 headless `--dump-dom` 采集（`tools/extract_flex_reference.py`），原有 33 例与 Chrome 154 的旧参考逐值相同；62 例 1061 个值都在 0.025 CSS px 内。探针的 raw Yoga 报告只含所有容器都是 flex 的例子。
  - `box_layout_contract`：网格值的诊断（线名、`auto-fill` 的弹性轨道、`em` 轨道、`grid-auto-rows` 里的 `repeat()`、命名线、行号 0、未知的 `justify-self`、包含块在网格外的绝对定位子项带 inset），网格是包含块时的绝对定位子项；34px 框里百分比尺寸的 svg 不再让上面的纵向 flex 项目报错，没有框住时由指定尺寸建议给出最小值。
  - `style_forgiving_contract`：网格属性不被丢弃；Pick 17 与 Market 的轨道表原样计算；`col-span-2`、`col-span-full`、`grid-area` 三个值与命名区域的展开；`place-items`、`place-self` 的展开；放置简写里的 `var()` 丢弃并提示；`border-radius: 3.40282e38px` 计算为 `1e+07px`。
  - `web_platform_contract`：宽松页面里 React 式内联样式的 Pick 17 网格（1000px 宽三列各 328px，auto 高度的卡片拉伸到 50px 的行，第四张在第二行 58px 处），Tailwind 类的 Market 列（30、12、278、56px），命名线丢弃并提示后自动放置，帧照常出来；严格页面同样的值是诊断。
  - 全量 `ctest -C Release -j 6`（BelowNormal）：176 项，只有已知的 14 项 CRLF 夹具哈希失败（与 `build/now-failed.txt` 相同）。没有这次的改动，新测试都失败（夹具探针在网格例子上抛错；属性未注册；网格整帧失败；百分比高度的纵向项目报错或长到内容高度）。
- **CRPG**：`npm run build:weft`，`weft_page_runner` 跑 UI/dist-weft（963×541，缩放 1.66，WARP）。Pick 17：`pushUI` 后由 `setPick17Draft` 推两个角色 11 个选项：7 个分类网格，880px 宽的滚动区里两列各 436px，同一行的卡片等高（71.4px），滚动区在页脚上方裁剪，帧画出来，没有布局诊断。Market：`loadMarket` 推三支股票：交易记录行 `auto auto 1fr auto` 为 18.2、11、139.8、56px，持仓行 `1fr auto auto`，“买/卖”按基线下移 1px。设置页“辅助功能”悬停色盲选项：四行色块各 8 格 13.38×18px，`url(#cb-*)` 滤镜照常。剩下的诊断只有 `object-fit`。截图在本组会话的共享笔记目录。插件的 weft.dll 用 `BuildWeft.ps1` 从这次提交（64e05fb）重建；游戏内（UE `-game`，副屏，BelowNormal，截图用游戏自己的 `shot showui`）：测试关卡的战斗开局推出 Pick 17 页（`Opening page then dispatching draft`），日志里没有网格或光栅的失败，帧照常推进（此前失败的帧每帧约 0.8 秒），页面画出两列等高的卡片、变体列表与页脚。
- **限制**：命名线与线名（`[name]` 在层叠里就丢弃）、`grid-template-areas`、`grid-template` 与 `grid` 简写、`subgrid`、`masonry`、`last baseline` 没有；只有从左到右的横排；项目的列贡献用适配器的内在宽度，flex 行的最小内容宽度仍是各项自动最小值之和，不是 CSS 的最小内容贡献；列尺寸不随行尺寸重算（§11.1 的第二轮）；不确定高度下百分比行轨道当 `auto`、百分比行间隙为 0；`fit-content()` 与跨度项目超出上限的分配是简化实现；基线对齐只在跨一行的项目之间，网格容器自己没有基线；绝对定位子项的 `justify-self`、`align-self` 不起作用；包含块在网格之外且带 inset 的绝对定位子项是诊断；布局前已知的高度不含 flex 拉伸或增长出的高度，那里的百分比高度纵向项目仍按内容取最小值。

读背景的 UI 着色器效果：折射与热浪（2026-10-18，`weft-dev` 分支；用户要求：HTML `filter` 提交之后先做读背景的效果，再继续 grid）。写法见 [效果的写法](effects.md)。
- **起因**：UI 着色器效果只读得到自己的输入与纹理，看不到元素背后画了什么，折射、热浪这类扭曲写不出来。架构里的背景拷贝已经为 `backdrop-filter` 实现（11.5a），效果一侧还没有接上（[兼容性](compatibility.md) 记为“读背景的效果尚未做”）。
- **实现**：
  - 接口：`weft_effect_desc` 末尾加 `backdrop`、`backdrop_margin`。`struct_size` 等于 `offsetof(weft_effect_desc, backdrop)` 的旧调用方照常注册，效果不读背景；`backdrop` 只能是 0 或 1，边距 0 到 256 CSS px，不读背景时边距须为 0，否则 `WEFT_INVALID_ARGUMENT`。`weft_page_runner` 的效果清单与 Unreal 插件（weft-unreal 的 `UWeftSubsystem::AddEffects`）读 `"backdrop"`、`"backdropMargin"`；`weft_add_effect` 加 `BACKDROP <margin>`。
  - paint IR：`EffectProgram::backdrop`、`backdrop_margin`；`Effect::backdrop_at` 是拷贝点。`paint::check` 要求拷贝点不晚于效果、其间没有结束拷贝点所在组的 `EndGroup`、边距在界内；`paint::append` 随项重新编号。
  - 显示列表：拷贝点是元素的第一个绘制项，元素开组时是组的开始。于是背景是元素开始绘制前目标里的内容，不含元素自己的背景色、外阴影与 `backdrop-filter`；元素自己因透明度开组时，读的是组外的目标（不需要 `backdrop-filter` 的 `under` 那样另选目标），效果画进组里随组合成；祖先的隔离组里只读到该组已画的内容，与 `backdrop-filter` 的背景根相同。
  - 计划（`plan_scene_pass`）：在拷贝点前插入 `Copy` 步骤（`ScenePass::copies`）。范围是盒子在自身坐标里四边加边距，经元素变换与设备缩放后的外接矩形，取整并限制在目标内；为空时既不拷贝也不画。合批不跨过拷贝点，第一项前有拷贝时不把它折成清屏色；读背景的效果单独一批，`Batch::copy` 指向它的拷贝。效果记录的 `uv` 是拷贝的设备像素，`mode.x` 为 `image_mode_backdrop`。
  - 外壳（`scene.hlsl`）：`EffectInput::backdrop`（本像素在拷贝里的 uv）、`backdropAcross`、`backdropDown`（盒子 uv 每单位对应的拷贝 uv：记录的逆变换求逆，乘设备缩放、除以拷贝大小，所以随旋转与缩放）、`backdropCopied`；`effectBackdropUV(input, uv)` 把盒子的 uv 映到拷贝，`effectBackdrop(input, uv)` 线性过滤、超出拷贝取最近的纹素，转成线性预乘。清单未声明的效果读到透明，三个后端相同（起初 D3D12、Vulkan 绑的是未初始化的 1×1 R8 图像，读出不透明的颜色，由 D3D12 的 C ABI 测试发现）。
  - 后端：拷贝在步骤到来时进行，效果绘制时绑在 `image`（t1）。D3D11 用 `CopySubresourceRegion` 拷到按（目标视图的格式、尺寸、pass 内序号）保留的纹理，两帧未用即释放；D3D12 在计划时为每个拷贝分配与所读目标同格式的临时图像，录制时转换状态后 `CopyTextureRegion`；Vulkan 结束渲染过程、`vkCmdCopyImage`、再以 load 开始，目标不可读时拷贝透明并计入 `backdrop of an unreadable target`。计数 `backdrop_copies`。文档页面的线性渲染器拒绝读背景的效果。
  - 损伤：帧里有读背景的效果时任何变化都整幅重画（同 `backdrop-filter`）；有效果的列表本来就不打补丁。
  - 样例（`samples/effects`）：折射 `refraction.hlsl`（离边 `--bevel` px 内视线向中间偏折，最多 `--strength` px，越靠边越强；周期 `--cell` px 的平滑波纹按 `--ripple` 乘强度偏移；`--tint` 盖在上面；边距 24）与热浪 `heat-haze.hlsl`（相距 `--wavelength` px 的波纹以 `--speed` px/s 上升，最多移动 `--amplitude` px，往顶部按 `--fade` 减弱、离边一个波长内渐弱为零，只依赖位置与时间；边距 8），清单 `effects.json`。`weft_sample_effects` 目标用 `compile_effect.cmake` 编到 `build/<preset>/samples/effects/`，连同清单，可直接给 `weft_page_runner --effects`；测试也把它们编进程序。
- **验证**（Release）：新的 `scene_backdrop_effects_contract`：显示列表的拷贝点（元素的背景色这一项；开组时是组的开始）；pass 的拷贝与区域（盒子加 8 px 边距，缩放 1 与 2）、记录的 `uv`、拷贝在效果之前、批次不跨拷贝点、第一项不折成清屏色、目标之外的效果不拷贝不画；检查拒绝拷贝点在效果之后、其间结束了组、边距越界；线性 pass 拒绝；损伤（有它整幅，没有它只是变化的矩形）；样例的清单经 `SceneEffects` 注册。像素：D3D11（WARP）在缩放 1 与 2 下、Vulkan（RTX 5090）上，与不带效果画出的背景逐像素对照已知的位移：(3, 2) px 经过边距且不读元素自己的红色背景、无边距时两个方向夹到拷贝边缘、夹到目标边缘、元素自己的透明度组（半透明合成）、祖先的透明度组（只读到组里的蓝块）、旋转 90° 的盒子沿自身轴位移、未声明的效果读到透明；折射强度 0 与热浪振幅 0 时与背景相同，`--tint` 盖在背景上（线性光混合），斜面只移动边缘、中间不动，热浪同一时间画面相同、换时间不同。最差 1 级（透明度组的合成），Vulkan 与 D3D11 最差 1 级；不可读的 Vulkan 目标拷贝透明并计入原因，验证层无消息。`render_d3d12_view_contract` 加 `effects`：经 `weft_runtime_add_effect` 注册位移效果（拒绝 `backdrop` 为 2、边距 -1、257、NaN、不读背景却有边距、结构停在两项中间；旧大小的结构注册为不读背景），页面上它在目标上、自己的透明度组里、另一个组里各一个：D3D12 与 D3D11 逐字节相同，盒子里是下面页面移动 (3, 2) px 的样子，旧结构注册的效果读到透明。核对过去掉修复时失败：把拷贝点放在效果自己那一项，位移经过边距一项失败（读到元素自己的背景色）；放在元素自己的组里面，透明度组一项失败。全量 ctest 176 项：除既有的 14 项（签出换行的散列校验）外通过。
- **CRPG**：样例拷进 CRPG 的 `UI/effects`，`effects.json` 加两项（`backdrop`、`backdropMargin` 由 `build-weft.mjs` 原样带进 `dist-weft/effects.json`），`npm run build:weft`（`WEFT_SOURCE_DIR` 指向 weft）编出三个效果。`weft_page_runner` 跑 UI/dist-weft（963×541，缩放 1.66）：三个效果注册成功；推入设置页后用 `eval` 在页面上加一块折射玻璃（320×190 px，圆角 24px，`--strength:16`、`--bevel:30`、`--ripple:0.25`）与一块热浪（380×220 px，`--amplitude:4`、`--wavelength:24`），没有新的诊断（仍只有 `object-fit`）。与加之前的帧相比，玻璃里 76,890 个设备像素变化（最多 183 级），热浪里 63,483 个（最多 154 级），设置项的文字在玻璃边缘向中间弯折、热浪里的边框线与提示文字晃动；第 60 帧与第 90 帧之间玻璃不变、热浪 72,624 个像素变化；强度与振幅设为 0 后，热浪与加之前逐像素相同，玻璃只差它 1px 的半透明边框（3,430 个像素）。盒子之外唯一的变化是设置页顶部一处与效果无关的动画，离两块的拷贝范围都远。没有接进正式页面，CRPG 仓库没有提交。
- **限制**：拷贝的边距由清单给出，CSS 不能逐元素改；超出拷贝范围（边距之外、目标之外）的读取取边缘纹素，即拉伸；带 `filter` 的元素连同后代与透视平面里的效果只读到所在图像已画的内容，元素自己带 `filter` 时什么也读不到；文档页面、D2D 参考光栅与 C ABI 显示列表不画；每个读背景的效果拷贝一次、不合批，帧里有它时任何变化都整幅重画；效果矩形的四角在编码 pass 里向外对角那一个像素有少量抗锯齿（各效果原有的行为，测试跳过这一圈）。

HTML 元素的 `filter`（2026-10-18，`weft-dev` 分支；CRPG 的 UI 移到 weft 之后宽松样式还在丢弃的一项，`object-fit` 之外）。
- **起因**：CRPG 字幕的光晕 `.subtitle-glow { filter: blur(10px) }`（随字幕淡入淡出）在宽松页面上被丢弃（`WEFT_STYLE_DROPPED: filter blur(10px) on <div> …`），光晕只是一块不模糊的渐变；回合横幅阵营纹章（HTML 元素）的 `drop-shadow()` 同样被丢弃；设置页色盲预览的 `filter: url(#cb-*)` 在层叠里就整条丢掉：宽松样式规范颜色时把 `url()` 里的 `#cb-deuteranope` 当成十六进制颜色，解析失败。scene 此前只画 svg 根元素上的单个 `drop-shadow()`（浮动 UI 的图标）。
- **实现**：
  - 解析（`src/scene/scene_filter.*` 的 `scene_filter`）：按顺序的任意个函数，至多 16 个：`blur()`（px）；`drop-shadow()`（颜色在长度前或后，缺省 `currentcolor`；两到三个 px 或 em 长度，第三个是标准差，同 Chrome）；`brightness()`、`contrast()`、`grayscale()`、`sepia()`、`saturate()`、`invert()`、`opacity()`、`hue-rotate()`，矩阵与 `backdrop-filter` 共用，但每个函数自成一步、之后钳制（`backdrop-filter` 合成一个矩阵）；`url(#id)` 指向页面里的 SVG `filter`，图元只能是 `feColorMatrix`（`matrix`、`saturate`、`hueRotate`、`luminanceToAlpha`；`in` 为空、`SourceGraphic` 或前一个图元的 `result`；`color-interpolation-filters` 取图元自己的、否则 `filter` 元素的，`sRGB`、`auto` 作用于编码值，初始值 `linearRGB` 先转到线性光），没有图元的 `filter` 让元素什么也不画；找不到滤镜元素时整个滤镜忽略（Filter Effects 1 §5）。不改变画面的函数（`blur(0)`、单位矩阵、透明的 `drop-shadow()`）略去。
  - 层叠与 scene（`src/style/style.cpp`、`src/scene/scene.cpp`）：宽松样式规范颜色时不进入 `url()`。宽松页面保留 scene 能画的滤镜，丢弃并诊断其他的（`drop_filter`）：SVG 内部元素上的、不支持的函数或长度、其他图元的 `url()`（在这次更新读的文档里找），诊断写出原因。`filter` 不是 `none` 的元素是固定定位与绝对定位后代的包含块。
  - 显示列表（`src/scene/scene_display_list.cpp`）：新的绘制项 `Filter`（`paint::Filter`：内容列表、函数、图像的像素与分辨率、缓存键 `source`）。元素与子树去掉自己的 `transform`、`opacity`、`filter` 与锚点，放在原点、根不裁剪，列成内容列表（`scene_filter_content`）；图像范围（`scene_filter_region`）是内容各项的墨迹（文字图像的边距、阴影的模糊与偏移、路径的描边、平面的投影，各在自己的裁剪内）加上函数的扩散（模糊 3.5σ；`drop-shadow()` 在四周都留 3.5σ 加偏移，偏移的拷贝读到图像外时镜像回来只读到透明）与 2 个设备像素的保护，按元素自身空间的设备像素取整，超过 2048×2048 像素时降低分辨率。内容平移进图像，项带元素的变换、透明度、裁剪与锚点。svg 图标的单个 `drop-shadow()`（盒子不画、内容只有形状、没有不透明度低于 1 的元素，`scene_path_shadowed`）仍是路径阴影；画了背景的 svg 等其他情况画成滤镜图像，不再回退；行内元素整个滤镜，不按片段。锚定的后代、粒子、同一元素上的 `backdrop-filter` 回退。
  - 滤镜链（`src/render/filter_chain.*`）：`plan_filter_chain` 把函数变成背景拷贝着色器（`backdrop.hlsl`）的全屏 pass：模糊同 `backdrop-filter`（σ 大于 4 个设备像素时先按面积缩小，再横竖两遍高斯，边缘镜像），颜色矩阵一遍，`drop-shadow()` 是图像模糊后经矩阵着色、按偏移读进新图像，再把图像以源上（source-over）画在它上面。`BACKDROP_MATRIX` 加了读源的 uv 偏移与线性光开关，背景拷贝的结果不变。`run_filter_chain` 在 CPU 上逐遍执行（遍与遍之间按 8 位量化），D2D 参考光栅用它。
  - 原生 pass（`scene_native`）：每个滤镜项一张 `SceneFilterImage`（内容的 pass 与链），按（视图、元素）缓存，以 `source`（`scene_paint_source_equal`）、函数、像素与分辨率校验：元素的透明度、变换与锚点变化时取缓存，内容或滤镜变化时重画；内容随时间变化（读时间的效果）或画宿主纹理时不缓存，后者所在的 pass 不保留。保留的 pass 打补丁时接受滤镜项（只改它的记录）。计数 `filter_builds`、`filter_reuses`、`filter_passes`。D3D11、D3D12、Vulkan 第一次用到一张滤镜图像时把内容画进离屏图像，用背景拷贝的管线逐遍画链，结果按 id 留在纹理缓存（D3D12、Vulkan 在提交前算待定，槽位租用），之后像图层一样采样。D2D 参考光栅取同一范围，用一个参考光栅画内容，在 CPU 上跑链，带透明度画出。
  - 损伤（`scene_damage`）：滤镜元素或其子树变化时整面重画。
- **验证**（Release）：新的 `scene_filter_contract`：解析（顺序、百分比与上限、角度单位、`drop-shadow()` 的颜色位置与 `currentcolor`、Tailwind 的两个 `drop-shadow()`、紧挨的函数、16 个的上限、四种 `feColorMatrix`、`linearRGB`、没有图元、找不到元素、拒绝的图元与 `in`）；显示列表（40×20 的盒子 `blur(4px)` 的图像 72×52 像素、项的位置与透明度、缩放 2 时像素加倍、`drop-shadow()` 四周的范围、svg 图标仍是路径阴影、带背景的 svg 画成滤镜、SVG 内容上的滤镜回退、页面里的 `url()`、锚定后代回退、`paint::check`）；链（宽一个纹素的线经 σ=2 模糊后逐列等于核的权重，σ=12 的边缘降采样后与误差函数最差 1 级，`drop-shadow()` 的合成）；原生 D3D11（WARP）与参考光栅画 18 种滤镜（字幕光晕的 `blur(10px)`、两种 `drop-shadow()`、八个颜色函数、两个 `url()`、链、元素透明度），缩放 1 与 2 逐像素最差 2 级，没有超过 2 级的像素；Vulkan（RTX 5090）与 D3D11 最差 2 级；缓存：透明度与平移变化时不重新过滤（`filter_builds` 不变，保留的 pass 打补丁），旋转与缩放同样，内容或滤镜变化时重新过滤；宽松页面三条丢弃的诊断。`style_forgiving_contract` 加 `filters`（Tailwind 的 `blur`、`drop-shadow`（`#rrggbbaa` 规范成 `rgba()`）、`grayscale`，只有 `.filter` 时为空，`url(#cb-deuteranope)` 原样保留）。`render_d3d12_view_contract` 的页面加了锚定铭牌的 `drop-shadow()`、`blur(8px)` 的光晕与面板的 `sepia()` `brightness()`：D3D12 与 D3D11 逐字节相同（调试层、保留的 pass、锚点移动、丢弃的录制）。`web_platform_contract`、`scene_floating_css_contract` 按新行为更新。核对过去掉修复时失败：图集合批把滤镜项当成无图像的路径时原生 pass 什么也不画（像素对照失败）；颜色规范进入 `url()` 时 `url(#cb-deuteranope)` 被丢弃。全量 ctest 175 项：除既有的 14 项（签出换行的散列校验）外通过。
- **CRPG**：`npm run build:weft`，`weft_page_runner` 跑 UI/dist-weft（963×541，缩放 1.66）：加载时不再有 `filter` 的丢弃诊断，只剩 `object-fit`。字幕（`showSubtitle`）与回合横幅（`setBattleSession`、`setBattleRound`、`setBattleTurn`）：8 个池化的 `.subtitle-glow` 与横幅纹章的 `motion.div` 带滤镜；与去掉滤镜的同一页面相比，纹章笔画下多了阴影（最多差 35 级），光晕的模糊把渐变向外扩开 1–3 级。设置页“辅助功能”的色盲预览三行 `url(#cb-*)` 都带上滤镜，但这一页有两个与滤镜无关的布局问题：`grid-template-columns: repeat(8, 1fr)` 不在网格 profile 里，预览一出现每帧布局失败（`WEFT_UNSUPPORTED_FORMAT`，每帧 100 多毫秒）；换成 `repeat(8, minmax(0, 1fr))` 后色块（`height: 100%` 的网格项）高度为 0，只见黑底。在临时拷贝里改掉这两处后，色块颜色等于矩阵的结果（`#e74c3c` 在 deuteranope 下为 173,184,65，protanope 下为 164,162,64）。耗时（硬件 D3D11，每帧绘制，与去掉滤镜的同一页面对照，各两次）：字幕加横幅 180 帧，有滤镜 advance p50 0.007–0.024 / p99 2.48–2.56 ms、render CPU p50 0.004–0.013 / p99 11.1–11.6 ms，没有滤镜 advance p50 0.008–0.014 / p99 2.50–2.83 ms、render CPU p50 0.007–0.011 / p99 11.1–11.4 ms（多数帧画保留的 pass）；字幕每秒出现又消失、横幅每两秒重放的 600 帧，有滤镜 advance p50 0.13–0.14 / p99 9.0–10.0 ms、render CPU p50 0.054–0.060 / p99 5.39–5.43 ms，没有滤镜 advance p50 0.12–0.13 / p99 8.3–9.5 ms、render CPU p50 0.056–0.058 / p99 4.94–4.97 ms（p99 的差是新光晕第一次过滤的帧）。`BuildWeft.ps1` 重建插件的 weft.dll。
- **限制**：SVG `filter` 的滤镜区域与图元子区域（`x`、`y`、`width`、`height`、`filterUnits`）不裁剪；只有 `feColorMatrix`；`url()` 只指向页面里的 `#id`；图像按设备缩放画在元素自身空间，元素自己放大时不重新栅格化（会变糊），超过 2048×2048 像素时降分辨率；没有与 Chrome 逐像素对照（参考光栅是同一算法的 CPU 版，函数的语义由解析与链的测试核对）；锚定的后代、粒子或同一元素上的 `backdrop-filter` 整帧回退；文档页面与 C ABI 的显示列表没有滤镜项；`object-fit`（等 `<img>`）仍丢弃；色盲预览还要网格支持 `1fr` 轨道与拉伸自动行。

`overflow-wrap`、`word-break` 与 `order`（2026-10-18，`weft-dev` 分支；CRPG 的 UI 移到 weft 之后宽松样式还在丢弃的几项，`object-fit` 与 HTML 元素的 `filter: blur()` 之外）。
- **起因**：CRPG 的页面在宽松模式下丢弃这三个属性并诊断（`WEFT_STYLE_DROPPED: … unsupported property overflow-wrap`、`word-break`、`order`）。战斗单位面板、状态面板、Pick 17 构筑页与 Buff 实验页的说明文字用 Tailwind 的 `break-words`（`overflow-wrap: break-word`），字幕文字是 `white-space: pre-wrap` 加 `word-break: break-word`：没有空格的长串撑出行宽而不断开。字幕叠层的 `.subtitle-item` 是池化的槽位，靠 `style={{order: seq}}` 让新字幕排在下面而不移动 DOM；没有 `order` 时按槽位下标排列，一个槽位空出来又被新字幕占用之后，新字幕排到了旧字幕上面。
- **实现**：
  - 样式：`overflow-wrap`、`word-break` 是继承属性，`order` 不继承，三者加进 scene profile（严格与宽松页面都接受，`src/scene/scene.cpp`）；`word-wrap` 是 `overflow-wrap` 的旧名，层叠时换成它（`src/style/style.cpp`）。`src/base/css_scalar.hpp` 加 `parse_integer`：整数值像 `calc()` 那样取整并夹到 int 范围（`calc(-infinity)` 层叠化简为 `-1e+07`）。
  - `word-break`（CSS Text 3 §5.2，`src/scene/scene_inline.cpp` 的 `tailor`）：在 UAX 14 的换行机会上按字形簇定制。簇边界两侧都是字母单位（簇首字符的 UAX 14 类是 AL、HL、NU、AI、ID，或 SA、CJ 与韩文各类，`probe_letter_units`，`src/layout/inline_layout.cpp`）时，任一侧的文字是 `break-all` 就加一个机会，两侧都是 `keep-all` 就去掉；仍然只在两字符的最近共同祖先允许换行时才是软换行机会。
  - `overflow-wrap`（CSS Text 3 §5.5）：`break-word`、`anywhere` 与 `word-break: break-word`（等于 `anywhere`）在会换行的文字里，把词在字形簇边界切成小块（也要是字形簇的开头，不拆连字；空格之后不切），作为紧急机会：贪心排行时只在一个词独占一行仍然放不下时才用（`rescue`），每行放下尽量多的簇，至少一个；放得进下一行的词先整个换到下一行，这一点与 `break-all` 在当前行填满不同。行宽逐块累加（`Advance`，`width` 也改用它），一个词的紧急断行是线性的。最小内容宽度改由 `SceneInlineContent::min_content` 给出，只用 `anywhere` 的紧急机会、不用 `break-word` 的：flex 项目的自动最小宽度因此随 `anywhere` 收缩、不随 `break-word` 收缩；有紧急机会的文字也算会换行（`breakable`，列方向的自动最小高度要用）。其他值（如 `word-break: auto-phrase`）是 scene 诊断。
  - `order`（CSS Display 3 §3.1、CSS Flexbox 1 §5.4）：Yoga 适配器（`src/layout/flex.cpp`）把 flex 项目按（`order`，文档顺序）插入 Yoga，网格（等列子集）按同样的顺序放格子、算各行高；绝对定位（含 fixed）的子项算 0；不是数的值是 `WEFT_CSS_VALUE`。scene 的 `scene_child_order`（`src/scene/scene.hpp`）给出 HTML flex、grid 容器子项的这个顺序，显示列表、D2D 参考光栅（逐帧绘制与它保留的绘制程序）、命中图与参考命中都按它排子项，再按 `z-index` 稳定排序；flex 容器的基线取这个顺序里的第一个项目。原生 pass 给保留的 pass 打补丁时，子项的顺序变了就整块重列这个容器（`changed_parts`）；参考光栅保留的绘制程序也比较顺序。不这样做时两者都会沿用旧的层叠。
- **验证**（Release）：`scene_inline_contract` 加换行用例：80px 宽的块里一个长词在 `overflow-wrap: break-word`、`anywhere`、`word-break: break-word`、`word-wrap: break-word` 与 `pre-wrap` 加 `break-word` 下断成多行，拼起来是原词，每行不超过 80px，再放一个字母就超；`nowrap` 不断；前面有短词时 `break-word` 先把长词换到下一行，`break-all` 用长词的开头填满第一行；CJK 默认在字间断，`keep-all` 只在空格处断；带组合附加符号的字母不拆开；80px 宽的 flex 行里，`anywhere` 与 `word-break: break-word` 的项目收缩到 80px 并断词，`break-word` 的项目保持词宽、一行（最小内容宽度不降）；原生 D3D11 pass 与参考光栅逐行对照断开的行（质心差 < 1px，墨量差 < 25%）；两个值的诊断。`style_forgiving_contract` 加 `wrapping_and_order`（不丢弃、继承、`word-wrap`、`order` 不继承、负值、`calc(-infinity)`、style 属性里的写法）。`scene_background_contract` 加 `flex_order`：一行三个互相压住 10px 的项目按 `order` 排成 b、c、a 并依次盖住前一个，绝对定位的子项按 0 排，`z-index` 仍压过 `order`，列方向按 `order` 叠放（字幕槽位的写法），网格按 `order` 放格子；每一点的颜色在原生 pass、参考光栅与参考光栅的保留程序里都对，命中两套都对；改一个项目的 `order` 之后，同一个原生 pass（打补丁）与同一个保留程序的参考光栅画出新的层叠。核对过去掉修复时失败：不在补丁里比较顺序时原生 pass 把 a 画在 b 上；参考光栅保留的程序不比较顺序时同样画错；最小内容宽度用 `break-word` 的机会时 `break-word` 的 flex 项目缩到 80px。全量 ctest 174 项：除既有的 14 项（签出换行的散列校验）外通过。
- **CRPG**：`BuildWeft.ps1` 重建插件的 weft.dll，`npm run build:weft`，`weft_page_runner` 跑 UI/dist-weft（963×541，缩放 1.66）：加载时不再有 `overflow-wrap`、`word-break`、`order` 的丢弃诊断，只剩 `object-fit` 与 `filter: blur(10px)`。字幕的时间线：显示 s1、s2，隐藏 s1 让它的槽位空出来，再显示 s3（占用槽位 0，DOM 里排在 s2 之前，`order` 是 3）：s2 的顶在 261px、s3 在 374px，新字幕排在下面；s2 里 105 个字母的长串（`word-break: break-word`）在 698px 宽的文字框里排成三行，`Second line:` 独占第一行，长串换到下一行再断开（行里有别的换行机会时先用它，CSS Text 3 §5.5）。
- **限制**：紧急断开只在一个文字节点之内，一个词跨元素边界（如 `aa<b>bb</b>`）时不在边界处断；不在连字内部断开；字母单位按簇首字符的 UAX 14 类判断，不按 General Category，`break-all` 下标点前后的规则照旧；`keep-all` 没有词典断词，`word-break: auto-phrase`、`line-break`、`hyphens` 没有；`order` 只排 flex 与网格容器的子项，非整数的值取整，不像浏览器那样丢弃整条声明；`object-fit`（等 `<img>`）与 HTML 元素的 `filter: blur()` 仍丢弃。

生成内容（`::before`/`::after`）、`outline`、`visibility`、ruby 与 `text-decoration`（2026-10-17，`weft-dev` 分支；CRPG 的 UI 移到 weft 之后宽松样式还在丢弃的几项）。
- **起因**：这几项在宽松页面上丢弃并诊断，CRPG 的页面因此画错：设置页分类标签的下划线与按钮的角标是 `::after`/`::before`；焦点环与选中框是 `outline`；`invisible`（悬停才出现的按钮、工具提示）照常显示，也挡住下面的命中；字幕的振假名 `<ruby>`/`<rt>` 排成一行，读音跟在汉字后面；`underline`、`line-through` 没有线。
- **实现**：
  - 生成内容（层叠，`src/style/style.cpp`）：主体是 `::before`/`::after` 的规则在原元素的路径上匹配（`matches_originating`，`:hover`、类等看原元素）；只有声明了 `content` 的规则决定盒子在不在（Tailwind 预置样式的 `*, ::after, ::before` 不声明 `content`，不生成盒子），`content` 计算为 `normal`、`none` 时没有盒子。生成盒的样式继承原元素，存在 `ComputedStyle::generated`，从上次的同一盒子增量计算，所以它们的过渡与动画照常。scene（`src/scene/scene.cpp`）：生成盒是原元素的第一个与最后一个子项（标签 `::before`/`::after`，身份是原元素的身份加高位：`generated_identity`、`is_generated`、`originating`），像元素一样布局与绘制；文字是 `content` 的字符串（含转义）与 `attr()`，引号、计数器与图片不给文字。命中算原元素；元素几何（`getBoundingClientRect` 等）不列出它们。
  - `outline`（CSS UI 4 §5）：简写与 `outline-width`、`-style`、`-color`、`-offset`。样式不是 `none`/`hidden` 时画一个环：内边在边框盒外 `outline-offset` 处（负值在里面），圆角随边框（半径 + 偏移 + 宽度），画在元素的子项之上，不占布局；`auto` 与其他样式都画实线（`scene_values::outline`）。
  - `visibility`（CSS Display 3 §4，继承）：`hidden`、`collapse` 的元素不画自己的任何东西（背景、边框、阴影、边框图片、效果、粒子、文字、轮廓）也不接受命中，子项按自己的值画与命中，布局不变。过渡与动画中两端有一个是 `visible` 时中间一直是 `visible`（`lerp_property`）：淡入一开始就可见，淡出到最后才隐藏（Tailwind 的 `transition` 把 `visibility` 与 `opacity` 一起列出）。
  - ruby（CSS Ruby 1）：`ruby` 元素是一个原子行内（`lay_out_ruby`）：内容分列，每列是一个 `rt` 之前的基文字（几段文字并排）与 `rt` 的文字，列宽取两者较宽，都居中；基文字在行的基线上，注音在其上一行（`ruby-position: over`、`ruby-align: center`）；`display: none` 的 `rt` 不注音，`rp` 不显示（UA 样式）。
  - `text-decoration`（CSS Text Decoration 3/4）：简写（线、样式、颜色、粗细，任意顺序）与四个分量。每一行的下划线在基线之下字号的 0.09 处、上划线在行的文字框顶，在文字之下；删除线中心在基线之上字号的 0.3 处，在文字之上；粗细 `auto` 是字号的 1/14，至少 1px；`currentcolor` 跟着文字的颜色，包括颜色的过渡（`scene_values::text_decoration_bands`）。装饰传给子孙：没有自己的线的元素画父元素的线，用父元素的颜色、样式与粗细；有自己的线的元素，它没写的装饰属性取初始值，不取父元素的。显示列表每条线是一个盒子，在行的矩形上（不在文字图像的整像素框上），裁剪同文字；D2D 参考光栅一样。
- **验证**（Release）：`scene_background_contract` 加 `visibility_and_outlines`（隐藏的盒子不画、它可见的子项画并接受命中，三种轮廓的位置、负偏移与圆角，原生 pass 与参考光栅逐点对照，缩放 1 与 2）与 `generated_boxes`（`::after` 的位置，`::before` 命中原元素，字符串、转义与 `attr()` 给出的文字，原元素的类改变时 `::after` 的过渡逐帧，两种光栅的颜色）；`style_forgiving_contract` 加 `generated`（顺序与身份、自己的规则、继承、`*::after`、`content: none`、没有 `content`、预置样式不生成、原元素的类改变时增量重算）与轮廓简写的层叠；`scene_inline_contract` 加 ruby（两列、较宽的注音撑开列、基线、`display: none` 的 `rt`）与 text-decoration（下划线的位置、粗细与颜色在文字之下，子元素的文字画父元素颜色的下划线，删除线在文字之上，元素自己的上划线用自己的颜色与自动粗细，原生 pass 与参考光栅逐点对照）；`scene_transition_browser_contract` 加 `visibility`（淡入、淡出）；`c_api_view_contract` 里“profile 外的属性”的例子由 `outline` 换成 `column-count`。传递的两条、`visibility` 的过渡核对过去掉修复时失败。全量 ctest 174 项：除既有的 14 项（签出换行的散列校验）外通过。
- **限制**：生成内容只有文字（没有 `content: url()`、计数器与引号），没有 `::marker`、`::first-line`、`::first-letter`、`::placeholder`、`::selection`；轮廓与装饰的各种样式都画实线（没有 `dashed`、`dotted`、`double`、`wavy`）；装饰不避让字形的下伸部分（`text-decoration-skip-ink`），位置不看字体的度量（没有 `text-underline-offset`、`-position`）；装饰按继承传递：子元素自己的线替换父元素的，不叠加，`text-decoration: none` 能去掉父元素的线（CSS 里不能）；ruby 的基只能是文字，`rt` 只取它的文字，没有 `ruby-position: under`、`ruby-align` 的其他值、`rtc` 与跨列注音；`visibility: collapse` 同 `hidden`；`overflow-wrap`/`word-break` 仍丢弃。

边框图片，以及 CRPG 页面在游戏里暴露的五处层叠、布局与绘制的错误（2026-10-17，`weft-dev` 分支；CRPG 的 UI 在 UE 里跑起来之后）。
- **起因**：CRPG 的页面在游戏里（UE 的 `-game` 进程，界面截图走 Slate 自己的读回）与 `weft_page_runner` 里逐页对照：所有 Tailwind 边框都是文字色（`border-amber-500` 不起作用）；右上角“结束轮次”按钮的最后一个字换行；工具提示演示的第三张卡片压在图例上；设置页的两块雕花边框（`border-image`）是两圈米色粗框。
- **实现**：
  - 层叠：`border-color` 是一到四个值的盒简写，在层叠里展开成 `border-<side>-color`（同 `border-width`、`border-style`）。此前它整个进层叠，到构建 scene 时才填进没有被设过的边，Tailwind 预置样式 `*, ::after, ::before { border: 0 solid }` 设过每条边的颜色，于是优先级更高的 `.border-amber-500` 总是输。`transition-property` 里的简写（`border-color`、`border`、`border-<side>`、`border-width`、`border-style`、`border-radius`、`margin`、`padding`、`inset`、`gap`、`background`）启动其分量的过渡（CSS Transitions 1 §2.1；Tailwind 的 `transition-colors` 写的是 `border-color`）。
  - 行内排版：可用宽度取到最近的 LayoutUnit（此前截断）。收缩适应的盒是文字的最大内容宽度加内边距与边框，文字在盒减去这些边（浮点）里排，浮点误差让它少一个 LayoutUnit，最后一个词就换行。
  - 绘制：没有面积的盒（宽或高为 0）不再连子孙一起跳过：自己什么也不画，子项与文字照画，除非它裁剪（`overflow` 不是 `visible`、svg 视口、透视平面）——高度为 0 的 body 里绝对定位的子项此前不画。显示列表与 D2D 参考光栅一样（`scene_shows_overflow`）。
  - 布局：列方向的 flex 项基准与高度都是自动时不收缩（`flex-shrink: 0`），因为基准就是它在自己宽度下的内容高度，自动最小值也是。此前自动最小值取最大内容宽度下的高度，换行的行（`flex flex-wrap`）只算一行，短的列里它缩到一行高，第二行压到下一项上。
  - `border-image`（`src/scene/scene_border_image.*`）：见 [兼容性](compatibility.md) 的“边框图片”一行。scene 着色器的适配图片按 `uv` 那一块的纹素算边缘夹取（背景图的 `uv` 是整张，不变）。
- **验证**（Release）：`style_forgiving_contract` 的 `borders`（预置样式之后的单值、`var()`、两个值、之后的单边）；`scene_transition_contract` 的 `shorthand_names`；`scene_inline_contract` 加收缩适应的盒在六种小数内边距下一行（去掉修复时 `End turn` 换行）与没有高度的包装盒的溢出（显示列表一个盒、参考光栅画出；裁剪时都没有）；`box_layout_contract` 的 `wrapping_row_keeps_its_lines`；`scene_background_contract` 的 `border_image_geometry`（九块、无 fill、百分比、宽度与 auto、重叠时缩小、外扩、诊断）与 `border_images_drawn`（三种边框图片，原生 pass 与参考光栅在每块里逐像素对照，缩放 1 与 2，差不超过 2 级）。行内、溢出与 flex 三项核对过去掉修复时失败，层叠一项先在页面运行器里复现过。全量 ctest 174 项：除既有的 14 项（CRLF 换行改变了固定 fixture 的 hash）外通过。CRPG 的设置页、按钮、工具提示演示在游戏里与页面运行器里画对。
- **限制**：`border-image-repeat` 只有 `stretch`；`::before`/`::after`、`outline`、`visibility`、`text-decoration`、`overflow-wrap`/`word-break`、ruby 仍丢弃。

宽松页面丢掉画不出的 filter，`translate`/`scale` 属性（2026-10-17，`weft-dev` 分支）。CRPG 字幕的光晕是 HTML 元素上的 `filter: blur(10px)`，scene 只画 svg 元素的单个 `drop-shadow()`，显示字幕的每一帧都回退而不画（游戏里界面会停在旧帧）；Tailwind v4 的 `-translate-x-1/2`、`scale-*` 写的是单独的 `translate`、`scale` 属性。
- **实现**：宽松页面（`WEFT_PAGE_FORGIVING_CSS`）的元素上 scene 画不出的 `filter` 在构建 scene 时去掉，并发一条 `WEFT_STYLE_DROPPED: filter ... on <div> ...` 的诊断（同一消息一次），帧照常；严格页面不变。`translate`、`scale` 与原有的 `rotate` 一样并进 `transform`：先 translate、再 rotate、再 scale、最后 transform 的函数（CSS Transforms 2），`scale` 的百分比换成倍数，第三个值（z）不用。
- **验证**：`web_platform_contract` 加 `filters`（诊断、元素没有 filter、帧照常）与 `transforms`（`translate:-50% 10px;scale:200% 1` 并成 `translate(-50%, 10px) scale(2, 1)`，getBoundingClientRect 为 80,110,80,20）。全量 ctest 174 项：160 项通过，14 项与此前相同。CRPG 的字幕帧画出（数字 Cinzel、拉丁 EB Garamond、假名 Shippori、汉字 LXGW）。
- **限制**：HTML 元素的 `filter`（模糊、颜色函数、drop-shadow）仍不画；`visibility` 仍丢弃（字幕的振假名 `<rt>` 靠它切换，ruby 排版也还没有）。

scene 的滚动容器、字体的 unicode-range、假名与韩文等文字、宿主注册的 UI 着色器效果（2026-10-17，`weft-dev` 分支；CRPG 的 UI 移到 weft 的第三步）。
- **起因**：CRPG 的页面逐个在 `weft_page_runner` 里跑：作弊面板的命令列表是内容超出的 `overflow: auto`，scene 每帧报 `scene scrolling outside supported non-overflowing auto profile` 而不画；字幕的混合字体栈靠 `unicode-range`（Cinzel 只出数字，Shippori 只出假名），weft 的字体没有这一项，Cinzel 会画全部拉丁字母；日文假名（与韩文、希腊文、西里尔文）在整形时抛 `WEFT_TEXT_SCRIPT`；设置页 1536 × 1024 的背景图超过 100 万像素的上限；烟雾背景（canvas 逐像素）改用 `-weft-effect(crpg-smoke)` 后，宿主没有 C ABI 注册效果。
- **实现**：
  - 滚动（`src/scene`）：`overflow` 为 `hidden`、`auto`、`scroll` 的元素是滚动容器（`clip` 只裁剪，不再报错；Yoga 适配器把 `clip` 当 `hidden`）。scene 节点记下元素要求的滚动位置（`SceneNode::scroll`，脚本的 scrollTop/scrollLeft）；每次布局它的子项之后（`scroll_content`），由子项的外边距盒与结束内边距算出内容大小，把位置限制在内容之内，按它移动子项的盒子（`scrolled`、子项的 `scroll_shift` 记下已移的量，局部布局只重排其中一个子项时按差值再移，不重复移动）。滚动位置改变使布局重新进行；部分布局写回容器自己的结果与其他子项。元素几何（`scene_scroll_geometry`）给出 scrollWidth/Height、scrollLeft/Top 与可滚动的轴，于是脚本赋值按内容限制、滚轮的默认动作滚动最近能滚的祖先并派发 `scroll` 事件（ScriptDom 原有的逻辑），命中测试与 getBoundingClientRect 随之。
  - 字体：`FontFaceEntry::unicode_range`、`parse_unicode_range`（`U+XXXX`、`U+XXXX-YYYY`、`U+4??`）；同一族的字面带范围时是组合字体（最近的字重与样式的全部字面，后列的先试）；`shape_with_fallback` 的 `ranges`：簇的码点不全在一个字体的范围内就不用它（第一个字体也一样）；行的度量取第一个范围含 U+0020 的字体（CSS Fonts 4 的 first available font）。C ABI：`weft_font_face` 末尾加 `unicode_range`（旧大小的结构仍可用）。
  - 文字：整形接受平假名、片假名、韩文、注音、希腊文、西里尔文（与拉丁文、汉字一样不需要分段整形的从左到右文字）；阿拉伯文与双向控制字符仍是 `WEFT_TEXT_SCRIPT`。
  - 图片：不透明的 PNG 可到 400 万像素（scene pass 按字节采样）；半透明的仍是 100 万像素（按浮点采样），超过的报 `a translucent PNG image ... past the bounds`。
  - 效果：`weft_runtime_add_effect`（`weft_effect_desc`：名字、参数与类型和初始值、纹理（原始字节或 PNG，数据或 sRGB）、是否用时间、D3D11/D3D12 的 DXBC 与 Vulkan 的 SPIR-V，编码与线性两种合成空间）；运行时的效果表交给每个 View 的 scene（`View::set_effects`）。`cmake/compile_effect.cmake`：宿主用 weft 的外壳把效果编成上述四份字节码（先按 `check_effect.cmake` 检查源文件；可与 `check_effect.cmake`、`scene.hlsl` 一起随宿主发布）。
  - `weft_page_runner`：`--effects <file.json>`（名字、字节码前缀、参数、PNG 纹理）、字体文件的 `"unicodeRange"`、`--slow <ms>`（打印过慢的推进）。
- **验证**（Release）：`view_contract` 加 `scrolling`（滚轮滚动内容超出的盒子，`scroll` 事件，行在 scene 里移动，命中跟着，超出内容时停在末端）；`incremental_contract` 的随机更新加滚动（增量与全量构建逐节点相同）；`scene_cluster_platform` 的 overflow auto 由拒绝改为检查 scrollHeight、限制后的 scrollTop 与内容移动；`scene_text_style_contract` 的 overflow 折叠（`clip`、`scroll` 保留，`overlay` 仍诊断）；`font_catalog_contract` 加 unicode-range 的解析与组合字体；`font_fallback_contract` 加假名、韩文、希腊文、西里尔文与带范围的回退；`view_images_contract` 加不透明与半透明大图；`c_api_view_contract` 加字面的 unicode-range 参数与旧大小的结构。全量 ctest 174 项：160 项通过，14 项与此前相同（签出换行的散列校验）。CRPG：嵌套提示页的烟雾由 GPU 效果画出；作弊面板加载不再报滚动诊断。
- **限制**：没有滚动条（`::-webkit-scrollbar` 在宽松样式下丢弃）；滚动内容只算直接子项（子项自己溢出的后代不计）；包含块在滚动容器之外的绝对定位后代也随之移动；`scroll-behavior`、`scrollIntoView` 与 `scroll-snap` 没有；效果在运行时注册后对之后的帧生效，没有卸载。

Web 平台模块、localStorage 的宿主接口、渲染回退的原因（2026-10-17，`weft-dev` 分支；CRPG 的 UI 移到 weft 的第二步）。
- **起因**：CRPG 的页面在宽松样式下能加载，但脚本要的浏览器对象不在：`localStorage`（语言、字体、字幕与 UI 缩放的设置）、`element.dataset`、`document.head`（字体模块插 `<style>`）、手柄输入合成的 `KeyboardEvent` 与 `document.activeElement`、`console.log`（此前什么也不做，`console.error` 让页面出错）。帧画不出时渲染 C ABI 只返回 `WEFT_RENDER_UNSUPPORTED_FEATURE`，看不到原因。
- **实现**：
  - 页面标志 `WEFT_PAGE_WEB_PLATFORM`（新平台模块 `src/script/js/web_platform_bootstrap.js`，装在 ScenePlatform 与 Engine 之后）：console 的各个方法成为诊断（`WEFT_DIAGNOSTIC_CONSOLE`、`_WARNING`、`_ERROR`，`%s %d %i %f %o %O %c` 代换，对象、数组、Map、Set、Error、节点与环的描述，8 KiB 截断）；`localStorage` 与 `sessionStorage`（Storage 接口与属性访问，超过 5 MiB 抛 `QuotaExceededError`）；`dataset`、`toggleAttribute`、`append`/`prepend`/`before`/`after`/`replaceWith`/`replaceChildren`；`document.head`（第一次用时建在 body 之前，隐藏）与 `document.title`；`KeyboardEvent`、`FocusEvent`；`focus()`、`blur()`、`document.activeElement`、`tabIndex`（可聚焦按 HTML：tabindex、表单控件、带 href 的 a，不禁用），focus、blur、focusin、focusout 的顺序同浏览器，`:focus` 与 `:focus-within` 随之匹配，焦点元素离开文档后回到 body；`matchMedia` 按样式表的 `@media` 求值、随视口变；`IntersectionObserver`（对视口或 root 的盒子，每个动画帧检查，边相接算相交）；`requestIdleCallback`、`navigator`、`location`、伪元素的 `getComputedStyle`；带选项的 `toLocaleString`（千分位、最少与最多小数位、百分比，无 Intl）。没有这个标志时一切照旧。
  - ScriptDom：`install_console_host`、`install_storage`、`install_media_host`、`focused()`，原生方法 `__ceConsole`、`__ceStorage`、`__ceFocus`、`__ceFocused`、`__ceMatchMedia`；焦点状态写进 DOM 节点的 `focused`/`focus_within` 标志（变化的节点进重算）。`src/script/web_storage.*`：一个存储区（按键排序、配额、修订号）。`media_matches()`（`style.hpp`）给 matchMedia 用宽松样式的 `@media` 求值。
  - View：`ViewPage::web_platform`、三种 console 诊断、`set_local_storage`/`local_storage`（View 的 localStorage 跨页面加载保留，sessionStorage 也是）。
  - C ABI：`weft_view_set_storage_item`（NULL 值删除；不是 UTF-8 的键值无效；超过配额 `WEFT_CAPACITY_EXCEEDED`）、`weft_view_clear_storage`、`weft_view_get_storage`（修订号变了才逐项交给回调，宿主据此保存）；存储区在第一次加载页面之前就有。渲染：`weft_renderer_last_failure` 与后端表的可选 `last_failure`，D3D11、D3D12、Vulkan 的 View 后端说明目标不合要求或原生 pass 回退的原因（预备器记下最后一次回退的原因）。
  - `weft_page_runner`：`"webPlatform"`、`--storage <file.json>`（加载前恢复、变了写回）、时间线的 `"storage": true`、渲染失败时打印原因。
- **验证**（Release）：`web_platform_contract`（新，`tests/web_platform_tests.cpp`）：console 的级别与格式、没有标志时的旧行为；存储的写入、宿主恢复的项、长度与枚举、跨加载保留、配额、删除与清空；dataset、head、title 与子节点的便捷方法；焦点的事件顺序、`:focus`/`:focus-within` 的布局、键盘事件冒泡到 window、blur、移除焦点元素；matchMedia 与改变视口后、IntersectionObserver 进出视口；toLocaleString。`c_api_view_contract` 加 `web_storage`（加载前的项、修订号、参数、配额、页面写入、NULL 删除、清空、失效句柄），未知页面标志改为 8。全量 ctest 174 项：160 项通过，14 项与此前相同（签出换行的散列校验）。CRPG 的页面去掉临时垫片后加载，`[boot]` 日志成为 console 诊断，设置写回存储文件；字幕帧的回退原因是 `filter blur(10px) outside scene profile`。
- **限制**：没有宿主的键盘输入（`KeyboardEvent` 只能由页面派发）、Tab 键导航与 IME；`MutationObserver`、`structuredClone`、`Intl` 没有；matchMedia 的 `change` 事件不派发；IntersectionObserver 不看 `rootMargin` 与中间的裁剪。

宽松样式表、white-space 的 pre 系列、无头页面运行器（2026-10-17，`weft-dev` 分支；用户提出：用 weft 替换 CRPG 框架里 Ultralight 上的 React UI，需要时可以改 weft 本身）。详见 [兼容](compatibility.md)“2026-10-17：宽松样式”。
- **起因**：CRPG 的 UI（React 19、Vite 6、Tailwind v4.2 生成的 90 KB 样式表、framer-motion）在 weft 里第一步就停：严格的 scene profile 拒绝整张样式表（`@layer`、`@property`、`@supports`、嵌套、`oklch()`、`color-mix()`、profile 外的属性），页面不加载。逐条兼容一张 Tailwind 表不现实；浏览器的做法是丢掉看不懂的声明，照常加载。
- **实现**：
  - C ABI：页面标志 `WEFT_PAGE_FORGIVING_CSS`，不设时一切不变（严格 profile 照旧拒绝）；`weft_view_evaluate` 在页面里求值一段脚本并交回结果文字（调试与无头测试用）。
  - `src/style/style.cpp`（宽松时）：样式表按 CSS 语法解析：`@layer`（命名、匿名、嵌套、先声明的顺序，`!important` 反转层序）、`@media`（宽高、宽高比、方向、`hover`/`pointer`、`prefers-*`、范围语法；视口改变时重新层叠）、`@supports`（按引擎实际接受的属性与值回答）、`@property`（注册属性的初始值与继承）、`@keyframes`、CSS 嵌套；`@font-face` 跳过并警告（字体由宿主注册）。层叠的次序加上来源、层与 inline。值：CSS Color 4/5（`oklch`、`oklab`、`lab`、`lch`、`hwb`、`color-mix()` 的预乘插值）换算成 sRGB；`calc()`、`min()`、`max()`、`clamp()` 在视口与 em 确定后化简；逻辑属性映射到物理边。profile 外的属性或值丢弃，发一条 `WEFT_STYLE_DROPPED` 的 scene 诊断（同一消息一次）；没有效果的属性（`cursor`、`user-select`、`will-change` 等）静默忽略。新文件 `src/style/color.cpp`。
  - 选择器（`src/dom/selector`，宽松时）：兄弟组合子 `+`、`~`；`:is()`、`:where()`、`:not()`（含复杂选择器，权重按 Selectors 4）；`:nth-child()`、`:nth-last-child()`、`:nth-of-type()` 等（含 `of S`）与 `:first-child` 一族、`:empty`；`:focus`、`:focus-within`、`:disabled`、`:enabled`、`:checked`；属性的 `i` 标志；`::before`、`::after` 只匹配、不生成内容。严格文法不变。
  - scene：`white-space: pre`、`pre-wrap`、`pre-line`、`break-spaces`（保留的换行是强制换行，以换行结尾的空行有行盒，制表按 4 个空格）；`display: none` 的 `position: fixed` 元素不再报错；布局里的 `std::invalid_argument` 成为 scene 诊断（此前 `weft_view_advance` 返回 `WEFT_INVALID_ARGUMENT`，没有消息）；UA 表隐藏 `head`、`style`、`script` 等。
  - `samples/page-runner`：`weft_page_runner` 无头加载页面文件夹（`page.json`），按时间线发事件、改视图模型、写锚点、指针与滚轮、求值、截图（D3D11 或 WARP，PNG），打印诊断、页面发出的消息与调用。
- **验证**（Release）：`style_forgiving_contract`（新，`tests/style_forgiving_tests.cpp`）：层与 `!important`、条件规则与视口改变、注册属性、颜色、数学与逻辑属性、选择器、嵌套、丢弃的诊断、严格模式照旧拒绝。`scene_inline_contract` 加 white-space 的 6 项（不支持的值改用 `balance` 检查诊断）；`c_api_view_contract` 的未知标志改为 4。CRPG 的测试菜单页面（Tailwind 全表）用 `weft_page_runner` 加载、渲染，布局与颜色和浏览器一致。全量 ctest 173 项：159 项通过，14 项与此前相同（夹具与浏览器输入的散列校验，签出换行）。
- **限制**：宽松模式还没有 Chrome 的逐值对照；`::before`/`::after` 不生成内容；`@font-face` 与 `unicode-range` 未接；常见但被丢弃的属性（`outline`、`visibility`、`translate`/`scale`、`text-decoration`、`border-image`、`overflow-wrap`、`word-break`、ruby）待做；CSS `zoom` 不支持（UI 缩放用设备缩放）。

weft 的 logo（2026-10-17，main；用户要求：动态 logo，深色、浅色、静态、彩色四个版本，用 weft 支持的内容实现）。见 [examples/logo](../examples/logo/README.md)。
- **内容**：`examples/logo/` 的四个页面文件夹（`dark`、`light`、`color`、`static`）共用 `weft-logo.js`、`weft-logo.css`、`weft-logo-keyframes.css`。标志是三根经线与三根纬线的平纹，压在上面的线两侧留底色断开下面的线；字标 `weft` 用同样粗细的线写，f 与 t 共用的横笔从 f 上、t 下穿过。全部是 SVG 的线与路径描边（圆头、圆角连接），动画是 `stroke-dashoffset` 的关键帧：经线画出，纬线依次拉过，写字母，横笔拉过，约 2.4 秒，停在最后一帧。不用字体与图片。
- **工具**：`tools/browser_logo_reference.mjs`（Chrome 无头、`probes/page_reference_clock.js`，`reference-cases.json` 的 10 帧 × 4 个版本，960 × 400）写 `tests/fixtures/logo-reference`；`tools/verify_logo_fixture.py` 检查截图与来源文件的散列。
- **验证**（Release，RTX 5090 机器）：
  - `logo_browser_reference`（新，`tests/logo_reference_tests.cpp`，View 加原生 D3D11 pass，WARP）：40 帧对 Chrome 155.0.8059.80 都通过，一帧里超过 32 级差的像素最多 0.040%（限 0.5%），平均差至多 0.07 级，最大差 90 级（曲线边缘的抗锯齿）。`logo_fixture_integrity`（新）：50 个 SHA256。
  - `weft_page_runner`（硬件 D3D11）画四个版本 200 帧：没有诊断，与 Chrome 的截图同样一致。
  - 变异检查：纬线在经线上的规则取反，40 帧中 31 帧失败、来源散列检查失败；纬线晚 0.1 秒拉过，三个动画版本的第 60 帧失败。
- **未验证**：D3D12、Vulkan 后端与 UE、Unity 插件里画 logo（同一个 scene，没有逐帧对照）；Android。
- **README**：改为对外的介绍（weft 是什么、能做什么、怎么接进引擎、构建、目录与文档），顶部是 weft 画的 logo（`docs/images` 的深色、浅色动画，按 GitHub 的主题切换）；原来的立项内容与阶段摘要原样移到 [立项与阶段摘要](charter.md)。

scene 的 transition 简写与列表配对；样式表的加载错误（2026-10-17，main；Unity 插件写示例页面时发现：样式表里的 `transition` 简写让 `weft_view_load` 返回 `WEFT_INVALID_ARGUMENT`，没有任何诊断；时长列表 `transition-duration: 0.15s, 0.1s` 能加载，但一开始过渡就是 scene 诊断 `finite numeric value 0.15s, 0.1`，整帧被拒）。详见 [兼容](compatibility.md)“2026-10-17：scene 的 transition”。
- **原因**：scene 的 profile 没有 `transition` 简写，`CompiledStyle` 在 `SceneSolver` 的构造里抛出 `WEFT_STYLE_PROFILE: unsupported property transition`；它在 `View::load` 捕获脚本错误的 try 之外，`failure()` 把它当作 `std::invalid_argument` 映射成 `WEFT_INVALID_ARGUMENT`，消息丢失。scene 的过渡对每个列表只取整串文字：`duration("0.15s, 0.1s")` 不是一个时间。
- **实现**：
  - `src/style/style.cpp`：`transition_parts` 按 CSS Transitions 1 拆简写（属性名或单独的 `none`、时长与其后的延迟、缓动函数、`transition-behavior`，顺序任意，缺的取初始值，转小写）；简写在层叠中展开为四个分量（同 `background`），没有 `var()` 的值加载时检查，有 `var()` 的在代换后拆，拆不出时分量在计算值时无效。
  - `src/scene/scene.cpp`：`transition_items` 按 `transition-property` 配对各列表（短的循环、长的截去），`transition_item` 取最后一个点到属性（或 `all`）的项；组合时长 max(时长, 0) + 延迟大于 0 才开始，`0s` 加延迟在延迟后跳变；元素样式改变时（包括第一次）读列表，不支持的缓动、负时长、列表里的 `none` 在此诊断；进行中的过渡在属性不再被点到、再变而不开始新过渡、离开目标样式时结束。scene profile 的四个分量有了初始值（`all`、`0s`、`ease`、`0s`）。
  - `src/view/view.cpp`：样式表与关键帧超出 profile 时页面不加载，一条 scene 诊断说明原因，`weft_view_load` 返回 `WEFT_SCRIPT_ERROR`（同脚本抛出）；`include/weft/weft.h` 的说明随之改，C ABI 不变。
  - JS：`'transition' in element.style` 为真。
- **验证**（Release，RTX 5090 机器）：
  - `scene_transition_browser_contract`（新，`tests/scene_transition_tests.cpp`）：
    - 与 Chrome 155.0.8059.80 暂停、定位到各时刻的过渡对照（`tools/browser_scene_transition_reference.mjs` 写 `tests/fixtures/scene_transition_reference.json`）：15 个用例（单个、Unity 按钮的两项列表、时间顺序与负延迟、短列表循环、长列表截去、`all` 后再点名、同一属性两项取后者、带逗号的缓动函数、`var()`、`var()` 代换出的不是列表、`none`、只有时间、`0s` 加延迟、改变之后的列表、大小写）× 9 个时刻：改变之后的分量列表逐项相同，不透明度、平移、背景色最大差 4.5e-7、4.8e-5 px、0.46 级（限 1e-4、0.01 px、0.75 级）；重新录制两次数据相同。
    - 300 组随机列表（各列表长 1–4，属性含 `all` 与重复，时长含 0s，延迟含负值），写成长度各异的分量与配对后的简写（各部分顺序随机）各一份，与按 CSS 配对的线性模型逐帧对照（2,440 个过渡中的采样）。
    - 简写的分量（样式表与 style 属性、后写的分量、`!important`、`inherit`、`initial`），12 种加载时的诊断、`normal` 与含 `var()` 的值能加载，4 种样式改变时的诊断；过渡的四种结束（不再被点到、再变而不开始、属性离开目标、到达终点后反向）。
  - `c_api_view_contract` 加 `stylesheets`：带简写的页面 `WEFT_OK`、无诊断；不支持的属性、简写的两种错误、at-rule、关键帧里不支持的属性都是 `WEFT_SCRIPT_ERROR` 与一条说明原因的 `WEFT_DIAGNOSTIC_SCENE`，没有脚本运行、没有元素，之后的页面照常加载。
  - 变异检查（改回一处、重建、两个测试至少一个失败）：7 处都失败：每项都取第一个时长、延迟、缓动（`unity-list` 30ms 平移 29.5 vs Chrome 51.3）；取第一个点名的项（`all-then-override`）；`0s` 不开始过渡的旧规则（`zero-duration-delay`）；进行中的过渡从不提前结束；没有简写（两个测试都失败）；不在计算值时拆简写；样式表错误越过 `View::load`（`c_api_view_contract`）。
  - 全量 ctest（170 项，Release）：156 项通过；14 项与上次相同（夹具与浏览器输入的散列校验，这台机器的签出换行）。
  - Debug：`scene_transition_browser_contract`、`c_api_view_contract`、`scene_contract`、`incremental_contract` 通过。
  - Unity 插件（weft-unity 的 `Native~` 用这个签出重建在临时目录，`weft_unity_tests --page`）：简写 `transition: background-color 0.15s, transform 0.1s` 的页面 `load: ok`，悬停与按下无诊断；时长列表的页面按下时不再有 `finite numeric value` 诊断；写了 `outline` 的页面 `load: page script error` 与 `diagnostic: WEFT_STYLE_PROFILE: unsupported property outline`；示例页面改回简写与时长列表后加载、点击、消息照常。
- **限制**：从没有声明的值（初始值）开始或到它结束的过渡（改变前后有一侧没有该属性时不过渡，如 `:active` 才写 `transform`）；`transition-property` 里的简写名；反向过渡的缩短；不能插值的属性在一半处切换；没有 transition 事件；加载失败沿用 `WEFT_SCRIPT_ERROR` 这个名字（字符串仍是 "page script error"）。
- **插件**：Unity 与 UE 插件用这个提交重建 weft 即可，ABI 不变；Unity 示例页面可以改回 `transition` 简写与时长列表。

`unreal` 分支合入 main（2026-10-17，用户检查后同意）：D3D12 后端、10 位渲染目标、不持有宿主目标（交换链可改大小）、页面文件里的图片（`url()`）、大面板的阴影、d3-sankey，共 6 个提交，main 是它的祖先，快进合并。合并前在分支上重跑全量 ctest（169 项）：155 项通过，14 项与此前相同（夹具与浏览器输入的散列校验，这台机器签出的换行）。之后在 main 上开发；下面各条标的 `unreal` 分支是它们做的时候的位置。

D3 的桑基图：d3-sankey 0.12.3（2026-10-16，`unreal` 分支，用户提出：游戏的经济面板要用桑基图画资源的流向）。详见 [前端库](frontend-libraries.md#d3-sankey2026-10-16)。
- **依赖**：`third_party/d3-sankey`（npm 发布包原样，tarball 与 UMD 的 SHA256、npm 的 SHA512 integrity 与 shasum 已核对，BSD-3-Clause），`third_party/manifest.json` 加一项；只是测试与样例依赖。
- **验证**（Release）：`d3_sankey_browser_contract`（新）：同一个脚本在 weft 与 headless Chrome 155 里的布局与 svg 逐值相同（12 个节点、12 条连线、7 项检查；改参考里一个坐标即失败）。`scene_paint_reference` 加 `sankey` 用例：原生 0.98 / 1.6% / 32，参考光栅 1.01 / 1.8% / 47（限 1.5 / 3% / 44、56）。重录截图时 Chrome 从 155.0.8059.40 升到 .80，旧用例只有 `repeating` 变了（2.7% 的像素差 1 级）。全量 ctest 169 项：155 项通过，14 项与上次相同（夹具与浏览器输入散列，签出换行）。UE 的经济面板用它画流向图：游戏的冒烟视角 `EconomyFlowsHan` 0 失败。
- **工具**：`tools/browser_d3_sankey_reference.mjs` 记录 Chrome 的报告并把 svg 写成绘制用例，再用 `tools/browser_scene_paint_reference.mjs` 录截图。

大面板的阴影（2026-10-16，`unreal` 分支）：UE 里的经济面板（900 × 640，`box-shadow: 0 4px 14px`）在 150% 缩放下整帧不画，D3D12 后端报 `WEFT_RENDER_UNSUPPORTED_FEATURE`；原因是 `shadow bitmap bound`：阴影图像在设备缩放下要 1,418 × 1,028 像素，超过 1,048,576 的上限。
- **实现**：`render/shadow_image.cpp` 的 `bitmap_scale`：超过上限时按 √(上限 ÷ 像素数) × 0.98 降低做图像的缩放（至多 8 次，内阴影连同模糊读取的外圈一起算），`shadow_geometry` 与 `shadow_image` 用同一个缩放，矩形仍是同一块，pass 线性采样拉伸；放得下的阴影不变。常量 `shadow_scale_tries`、`shadow_scale_margin`。
- **验证**（Release）：`paint_contract`：4000 × 4000、模糊 64 的阴影在 2× 下做成上限以内的图像且覆盖模糊的范围（此前是拒绝的反例）；700 × 500 的窗口阴影在 2× 下做成 1× 与 2× 之间的图像，6 个采样点与 1× 的差 ≤ 10 级；内阴影同样。`scene_native_contract`：900 × 640 的面板在 1.5× 下准备成功、不回退、阴影图像在上限以内。去掉降缩放后两项都失败（变异检查）。全量 ctest 168 项：154 项通过，14 项与上次相同（夹具与浏览器输入散列，签出换行）。UE 的经济页面（D3 横条、散点、坐标轴、22 个 `url()` 图标、150 座城）在 D3D11 WARP 原生 pass 上 1×、1.25×、1.5×、2× 都画出（此前 1.5×、2× 回退）。
- **限制**：渲染 C ABI 不报回退原因（`SceneNativeWork.reasons` 只在内部），宿主只看到 `WEFT_RENDER_UNSUPPORTED_FEATURE`；这次靠一个临时的诊断程序找到原因。

页面文件里的图片：scene 页面的 `background-image: url()`（2026-10-16，`unreal` 分支，用户提出：UE 里的游戏要在界面上用一张共享的灰度噪声纹理做旧图表，纹理放在页面的文件夹里而不是引擎资产；用户要求把这个能力做进 weft 而不是插件里绕开）。详见 [与引擎通信](engine-communication.md#8-对外-view133)“页面的文件”与 [兼容](compatibility.md)。
- **接口**：`weft_page_desc` 末尾加 `base`、`base_size`（样式表的路径；`struct_size` 等于 `offsetof(weft_page_desc, base)` 的旧调用方照常加载）；`weft_view_set_resource_reader`、`weft_resource_fn`、`weft_resource_writer_set`：View 求解场景时在 owner 线程上同步读文件，每次页面加载每个路径一次，回调里不能重入 View。
- **依赖**：lodepng（commit e584a84，`20261006`，zlib 许可），全部 26 个文件未改，固定在 `third_party/manifest.json`（逐文件 SHA256 与 archive SHA256），只编 `lodepng.cpp` 为私有静态库 `weft_lodepng`，关掉编码器、文件读写、辅助块与 C++ 接口（[记录](../third_party/lodepng/README.md)）。选它而不是 stb_image：只做 PNG、读文件头就能先查尺寸、单文件，用户选定。
- **实现**：
  - `src/view/view_images.*`：`view_resolve_url`（RFC 3986 的合并与去点段；相对基准开头没得退的 `..` 保留，UE 的页面路径是 `../../../Game/Content/...` 这种相对路径）、`view_decode_png`（先查头再解压，至多 1024 × 1024 像素、边长 4096）、`ViewImages`（每次加载的缓存、诊断与总量上限）；
  - scene：`scene_values::image_url`（引号、转义与被分词拆开的不带引号的 URL）、`SceneImages` 接口与 `SceneSolver::set_images`，`SceneNode::background_image`（参与相等、来源键与 damage 比较）；
  - 显示列表：`paint::Image::fitted`，`url()` 的图片是列表自己的资产，像宿主纹理那样带 `TextureFit`；`paint::check` 与线性 pass 拒绝不合规的；
  - pass：fitted 图片用宿主纹理的铺排模式（`image_mode_texture`）、准备时用效果纹理的路径做成整张纹理（不透明 sRGB、半透明线性预乘浮点）；Vulkan 与 D3D12 设备不能过滤浮点纹理时整帧回退（同效果纹理）；D2D 参考光栅同样画。
- **验证**（Release，RTX 5090 机器）：
  - `view_images_contract`（新）：16 种基准与 URL 的解析；RGBA8、2 位调色板加透明、16 位灰度的 PNG 逐字节，超界的头不解压，坏数据与截断；每个路径读一次、同一路径不同写法同一份资产、失败诊断一次；View 里三种写法的 `url()` 得到同一张图、同一次加载不再读、重新加载重读、缺文件是 scene 诊断、没有回调时无图。
  - `c_api_view_contract` 加 `resources`：基准、回调、回调里重入得 `WEFT_BUSY`、写入的参数检查（空指针、超过 16 MiB 得 `WEFT_CAPACITY_EXCEEDED`）、缺文件/回调失败/非 PNG 各一条诊断、旧的结构大小、落在 `base` 中间的大小被拒、去掉回调、销毁后 `WEFT_STALE_HANDLE`。
  - `scene_texture_contract`：`url()` 的语法与诊断、节点的图、显示列表（色、图、边框；一张资产；圆角裁剪；不做缓存图层）、`paint::check` 与线性 pass 的拒绝。
  - `scene_background_contract`：九种铺排的半透明 `url()` 图片，D3D11 WARP 原生 pass 与参考光栅在 1×、2× 下逐像素至多差 2 级（与宿主纹理相同）。
  - `scene_paint_reference`：四个纹理用例再用 `url()` 画一遍，与 Chrome 的截图（Chrome 画的就是同样的 PNG）误差与 `-weft-texture()` 完全相同（如 texture-size 原生 0.63 / 0.0073 / 13），Vulkan 与 D3D11 WARP 至多差 2 级。
  - 全量 ctest（168 项）：154 项通过；14 项是与上次相同的夹具与浏览器输入散列校验（这台机器的签出换行，与本次无关）。
  - UE（CaseKargaroo，weft-unreal 插件经 `FFileHelper` 读文件、传页面基准）：城市面板的环图用 `url(../Textures/pigment.png)`，冒烟 0 失败、无页面诊断，截图上纹理与边缘暗化可见；自动化测试 40/40；两进程联机 0 不同步。
- **限制**：只有 PNG；没有 `data:` URL、`<img>`、`image-set()`、多层背景、文档页面的背景图；没有 mip（缩得很小时会闪烁）；回调是同步的（本地小文件，第一帧就有图，不为异步到达重做场景）；半透明图在 pass 里每像素 16 字节，所以单图限 1,048,576 像素。
- **下一步**：需要时加 JPEG/WebP、mip 与 `<img>`。（2026-10-17 已合入 main。）

D3D12 后端（2026-10-15，`unreal` 分支，为 UE 5 的 D3D12 RHI；用户要求同时保留 Metal 等 API 的能力：后端仍是渲染器 C ABI 下的一个适配器，核心与其他后端不变）。详见 [渲染后端 API](render-backend-api.md#view-帧2026-10-14)。
- **接口**：`include/weft/render_backend_d3d12.h`：`weft_render_d3d12_create`、`_external`、`_destroy`，`weft_render_d3d12_pass`（打开的命令列表与目标资源）、`weft_render_d3d12_texture`；`WEFT_RENDER_API_D3D12`（4）。
- **实现**：`src/render/backends/d3d12`（`weft::scene_d3d12`）：照 Vulkan 的场景 pass 移植（保留的 pass、补丁、槽位、待提交检查、背景滤镜链、粒子与效果），着色器复用 D3D11 的 DXBC，根签名、描述符堆、上传与资源状态是自己的。
- **验证**（Release，RTX 5090 机器）：
  - `render_d3d12_view_contract`：D3D12 WARP 与 D3D11 WARP 画同一帧逐字节相同：游戏画面之上、宿主清屏、保留的 pass、锚点移动、RGBA8 目标、typeless 目标、丢弃的录制、宿主纹理；另一设备的目标、格式不符、pass 大于目标、空命令列表、非预乘的清屏色被拒，帧绑定的纹理作目标是 `INVALID_STATE`；录制已提交、尚未报告完成时目标的引用计数不变（适配器不引用目标，交换链能改大小）；D3D12 调试层无错误；`WEFT_D3D12_GPU_VALIDATION=1` 时 GPU 校验也无错误。
  - `engine_host_d3d12_warp`：样例的 D3D12 模式（每帧写的小地图纹理、锚点、事件与调用）与 D3D11 WARP 的末帧逐字节相同；本机 GPU 上与 WARP 至多差 3 级。
  - 全量 ctest（167 项）：153 项通过；其余 14 项是 Python 夹具与浏览器输入的散列校验（如 `react_fixture_integrity`、`scene_fixture_integrity`），它们核对的文件这次都没改，有的在仓库之外（CaseCheetah 的 `UI/src`），在这台机器的新克隆上无论 CRLF 还是 LF 签出都不符，与 D3D12 后端无关。
- **UE 里的整合**：在 CaseKargaroo 里经 weft-unreal 插件验证（城市标签、地块与城市面板、经济面板、改窗口大小；冒烟与两进程联机）。
- **未验证**：多 GPU；设备移除。

重构第 13 步之五（完）：增量管线，13.5c owner 侧每个元素的开销（2026-10-15）。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14) 与 [架构](architecture.md#增量更新的正确性)。
- **结果**：
  - 级联：缓存淘汰按使用序；子元素只在它读的父值变了时重算；只改内联样式里不继承的属性时，只把这些属性对缓存的规则胜者重新级联；元素上次的内联样式按元素记下；
  - 场景构建：重建的元素不再先复制旧节点；绘制值只重新解析文本变了的槽；变换的参数不再经字符串流；
  - 局部布局：绝对定位、两个方向都有插入值、父即包含块的盒子是布局边界，改动只重排所在的最外层边界（在取包含块样式与尺寸的替身里排），其余子树共享；
  - View 与画廊会话关掉求解器的元素几何（读几何时自己算）；
  - 渲染侧补丁分三段（先做不必列出的检查，形状都对再规划），被拒的帧少做白工；浮动 UI 基准打印补丁与拒绝原因；
  - HUD 基准新增“只移动”一档（400 个单位每帧改 transform，不改血量）。
- **测量**（`--hud-benchmark-hardware`，RTX 5090 + 9950X3D，固定 CCD0、高优先级，Release，3840 × 2160，每档 240 帧，三轮中位数）：400 个单位，owner p50 / p99，13.5b → 13.5c：
  - 页面自算改 1 个：0.16 / 6.8 → 0.111 / 0.194 ms（最慢 0.24 ms；渲染 CPU 0.043 / 0.078 ms）；改 4 个 0.24 / 6.9 → 0.125 / 0.32 ms；改 40 个 1.3 / 8.8 → 0.42 / 1.2 ms；改 400 个 17.7 / 20.7 → 4.1 / 4.5 ms；400 个只移动 3.3 / 3.7 ms（最慢 4.5 ms）；推 JSON 400 个 17.5 / 19.3 → 3.8 / 4.3 ms；
  - 锚点加 20 个血条改宽度：6.6 / 7.1 → 0.91 / 1.09 ms（Vulkan 0.93 / 1.17 ms）；改 `scaleX` 0.35 / 0.53 → 0.32 / 0.47 ms；
  - 13.5 的验收：每帧改 1 个元素 owner 与渲染 CPU 都不到 0.2 ms（p99 也是）；改 400 个 transform owner p99 3.7 ms（≤ 4 ms）；随机变更下增量与全量逐值相同。
  - CRPG 原样的浮动 UI（`weft_floating_benchmark --full`，三轮中位数）：锚点方式的推进 p50 / p99，13.4 → 13.5c：200 个单位 29.3 / 63.4 → 1.63 / 27.0 ms，50 个 8.53 / 23.5 → 1.62 / 12.8 ms，12 个 4.75 / 13.6 → 1.93 / 11.0 ms；推 JSON 200 个 191 → 148 ms；渲染 CPU 200 个 14.6 → 13.2 ms。
- **验证**：
  - `incremental_contract`：级联新增 1,200 次随机更新（规则不读父值、内联样式在各类属性与 `!important` 规则之间变化）；新增 layout 段：900 次随机更新与整页布局逐值相同（849 次布局里 348 次局部），另 26 个确定性用例各恰好一次局部布局；场景对照 367 次布局里 300 次局部；
  - 变异：13.5c 26 个，24 个被发现，2 个等价（只有一个方向插入值的边界盒：Yoga 按父的对齐放置，结果不变；缓存淘汰的次序只影响命中）。第一轮存活的补了 content-box 包含块、max-height 与“包含块比父宽且内边距是百分比”的用例，删去了快速路径里多余的属性比较；
  - 渲染侧补丁分三段之后，13.5b 的 34 个渲染变异重跑，全部被发现；
  - `ctest`：Release 165/165，Debug 165/165；产物与 12.3 基线相比与 13.5b 相同（同样的 24 个已知差异套件；`cheetah-flow-window-vulkan` 的 `peak_in_flight` 在并行测试时变了一次，单独重跑两次与基线相同）；Android 零警告。
  - 命令：同 13.5b；基准结果在 `build/perf/hud-13-5c-final/round-*/`。
- **限制**：
  - 变化超过一半的帧（如 400 个都在动）渲染 CPU 仍约 4.3 ms：pass 整份重新准备（13.5b 的限制）；
  - 动画多的页面（浮动 UI）补丁约一半被拒，渲染 CPU 比不记区间多 2–5%（交替运行的对照）；
  - 布局边界外的结构变化、视口变化、fixed 盒、隐藏祖先下的变化仍整页布局；
  - 级联的快速路径只覆盖不继承的属性；改继承属性（如颜色、字号）的内联样式仍走完整级联；
  - 每个重建元素的样式与绘制值仍按字符串解析（约 8 µs，400 个约 3 ms）。
- **下一步**：第 13 步的五段都已完成；下一步待定。候选：变化多的帧的渲染侧准备（每个元素生成显示条目时解析样式字符串）、动画多的页面上补丁的命中率、Android 真机（待办）、D3D12 与 UE 的 RHI 后端（M5）。

重构第 13 步之五（续）：增量管线，13.5b 渲染侧只重做变化的子树（2026-10-15）。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14) 与 [架构](architecture.md#锚点)。
- **结果**：
  - 显示列表记下每个元素的访问上下文与它在列表各部分的区间，可以只在原上下文里列一个子树；
  - 保留的 pass 在内容版本变了时就地补丁：按存储找出变了的子树，形状相同的部分原地换掉条目、记录、圆角裁剪与图像；顺序、z-index、锚点或背景模糊的变化、效果、形状不同、或变化超过一半时重新准备；
  - D3D11 与 Vulkan 的槽位只上传之后的补丁写过的记录与圆角裁剪（最近 8 次补丁）。
- **测量**（`--hud-benchmark-hardware`，RTX 5090 + 9950X3D，固定 CCD0、高优先级，Release，3840 × 2160，每档 240 帧，三轮中位数）：400 个单位（约 1,600 个节点），渲染 CPU p50 / p99：
  - 页面自算改 1 个：4.1 → 0.044 / 0.084 ms（最慢 0.20 ms）；改 4 个 0.078 / 0.14 ms；改 40 个 0.52 / 0.72 ms；改 400 个 4.4 / 5.2 ms（整份重新准备，比 13.5a 多约 5%）；
  - 锚点加 20 个血条改宽度：4.4 → 0.15 / 0.19 ms（Vulkan 0.18 / 0.24 ms）；改 `scaleX`：4.5 → 0.13 / 0.21 ms（Vulkan 0.16 / 0.25 ms）；
  - owner 侧不变：改 1 个 p50 0.16 ms，p99 6.8 ms（改宽度的帧整页布局，13.5c）。
- **验证**：
  - `incremental_contract` 新增 pass 与 WARP 两段：
    - 两个种子各 200 帧随机变化，保留的列表与区间、补丁后的 pass 与新准备的逐字节相同（221 帧补丁、179 帧重新准备）；
    - 五个单项：svg、透明组换了子元素、换批次、锚定元素里的透明组、大部分变化；
    - WARP 两个槽位交替，144 帧像素与每帧重新准备的相同（93 帧补丁，20 帧按新深度重排，22 帧槽位只写补丁写过的部分）；
  - `render_vulkan_scene_contract`：三帧在途的补丁录制与每帧重新准备的相同（60 帧里 34 次补丁），验证层零告警；
  - 变异：渲染侧 34 个全部被发现。第一轮存活 7 个，补了场景与计数断言；第二轮存活 1 个：圆角半径的变化没有碰到像素，补了铺满裁剪框的盒子。测试页变了，13.5a 的求解器 27 个重跑，全部被发现；
  - `ctest`：Release 165/165；Debug 164/165，`incremental_contract` 的测试辅助函数对只有一条声明的内联样式取了空串的 `back()`（13.5b 新加的元素才有），修正后重跑通过；产物与 12.3 基线相比与 13.4 相同（同样的 24 个已知差异套件）；Android 零警告。
  - 命令：同 13.5a；三轮固定核心的运行用 `Start-Process` 后设 `ProcessorAffinity = 0xFFFF`、`PriorityClass = High`，结果在 `build/perf/hud-13-5b-final/round-*/`。
- **限制**：变化超过一半的帧整份重新准备；效果、粒子、背景模糊、透视平面、宿主纹理所在的子树，以及锚点或背景模糊的增减、换键，只能重新准备；每个元素生成显示条目的开销（每帧解析样式字符串）不变。
- **下一步**：13.5c owner 侧每个元素的开销：级联的内联样式快速路径与缓存淘汰、只排受影响部分的布局、跨帧的行内排版缓存。

重构第 13 步之五：增量管线，13.5a owner 侧只处理变化的节点（2026-10-15）。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14) 与 [架构](architecture.md#增量更新的正确性)。
- **剖析**：新入口 `weft_d3d11_probe --hud-benchmark-sampled <dir>`（需要 windows-profile 的 PDB）。400 个单位（约 1,600 个节点）每帧改 1 个时，owner 3.7 ms 都花在逐帧走整份文档的级联、几何列表、场景树构建与布局上；渲染 4.3 ms 里 75% 是每帧重建整份显示列表。
- **结果**：
  - 场景树写时复制（`SceneChildren`）：没变的子树在帧之间共享存储，发布与给出帧只复制根；改子节点要经 `edit`；
  - 级联从上一次读的文档更新：没变、上下文也没变的子树不访问，计入的上限照记录，与全量相同；只改内联样式不让后代重算；
  - 展开与场景构建只走变化的部分：元素只在输入变了（DOM、文字、计算样式、继承的值、显示与否）或跟随时间时重建，其余子树共享；
  - 布局的输出先写旁表，只把变了的盒子写回；只改 transform、opacity 等的帧不布局；
  - `View` 的元素几何只在 JS 读几何时才算；内容相同的帧对象得到同一修订号。
- **测量**（`--hud-benchmark-hardware`，RTX 5090 + 9950X3D，固定 CCD0，Release，每档 240 帧）：400 个单位页面自算，owner 更新 p50 / p99：
  - 改 1 个：5.3 / 12.1 → 0.21 / 7.1 ms；改 4 个：6.1 / 14.9 → 0.31 / 7.4 ms；改 40 个：7.0 / 18.2 → 1.4 / 9.3 ms；改 400 个：22.6 / 25.3 → 18.6 / 26.2 ms；
  - 锚点只移动 0.008 ms；锚点加 20 个血条改 `scaleX`：7.1 → 0.42 ms；改宽度：10.7 → 7.1 ms；
  - p99 来自每 20 帧一次的宽度变化：仍要整页布局（约 7 ms）；改很多元素时仍是每个元素约 25 µs 的级联（13.5c）；
  - 渲染 CPU 不变（约 4.1–4.5 ms，13.5b）。
- **验证**：
  - 新测试 `incremental_contract`：
    - 级联 1,200 次随机更新与全量级联逐值相同，报告的变化节点、stamp、计入的上限都对；
    - 场景 900 次随机更新（含过渡、关键帧、移动、移除与放回）与全量求解器的帧相同；不受影响的子树 1,890 个里 1,889 个共享原来的存储；
    - 粒子（隐藏又显示的祖先、流、爆发）240 步、行内格式化（子元素的显示改父元素的文字分法）60 步与全量相同；
  - 变异：级联 11 个、求解器 27 个，除 1 个实际等价的外都被发现（第一轮存活的 5 个补了场景或删去多余的记账）；
  - `ctest`：Release 165/165，Debug 165/165（新增 1 个）；产物与 12.3 基线相比与 13.4 相同（已知按时序变化的套件、图层复用数、VM 峰值，以及 13.4a 起 glass-page 与 css-03 各 2–7 个像素差 1 级），现有画面不变；Android 零警告。
  - 命令：`ctest --preset windows-release --parallel 6 --output-on-failure`（与 `windows-debug`）；`python -I tools/compare_artifacts.py build/step12-3-baseline build/windows-msvc/artifacts`；`weft_d3d11_probe --hud-benchmark-hardware <dir>`（固定 CCD0、高优先级；结果在 `<dir>/hud.json`）与 `--hud-benchmark-sampled <dir>`。
- **限制**：改宽度、改结构的帧整页布局；改很多元素时每个元素的级联开销；渲染侧每帧重建显示列表与整个 pass。
- **下一步**：13.5b 渲染侧（显示列表按子树就地更新，pass 只重算、只上传变化的记录），然后 13.5c。

引擎纹理的五项限制（2026-10-15，13.3c 之后，用户定为中优先级）。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14)、[与引擎通信](engine-communication.md#8-对外-view133) 与 [兼容表](compatibility.md)。
- **结果**：
  - 铺排：`-weft-texture()` 像 CSS 的背景图那样铺排：`background-size`（长度、百分比、`auto`、`cover`、`contain`；自然大小是纹理的像素）、`background-position`（一到四个值）、`background-repeat`（`repeat`、`no-repeat`、`space`、`round`，两轴各自）、`background-origin`、`background-clip`，以及 `background` 简写的一层。瓦片由着色器按绑定纹理的大小算，宿主换了大小不同的纹理也画对。13.3c 时纹理总是拉伸铺满边框盒，现在要铺满写 `background-size: 100% 100%`（HUD 样例已改）；
  - `background-clip` 也裁剪背景色（任何元素）；`background` 简写拆成全部分量（此前只分颜色与图像）；
  - sRGB 与 BGRA：外部纹理接受 sRGB 的 RGBA8、BGRA8 视图与线性 BGRA8；
  - mip：`weft_render_external_texture::mip_levels`（原来的保留字段，0 与 1 都是一层），缩小时在层间采样，字节预算按每层算；
  - D2D 参考光栅画宿主给的像素（`SceneRaster::bind_textures`），铺排与采样同着色器；
  - Vulkan 的目标别名：纹理结构末尾可选的 `image`，给出时拒绝画进帧绑定的纹理（同 D3D11）；
  - 跨设备、跨 API：D3D11 接受别的设备或 API（如 D3D12）共享、宿主在后端设备上打开的纹理，写与读由宿主在立即上下文上用共享 fence 排（encode 之前 Wait，之后 Signal），不需要新的 C ABI。
- **验证**：
  - 新测试 `scene_background_contract`：
    - 解析、铺排（含 round 的比例与 space）、简写、一个盒子、关键帧里的简写、诊断；
    - 原生 D3D11 WARP 与 D2D 参考在九种铺排、缩放 1 与 2 下，各绘制区内部至多差 2 级；
  - `scene_paint_reference` 新增 4 个 Chrome 155 用例（尺寸、位置、重复、盒子，Chrome 里纹理是 `url()` 的 PNG）：平均差 0.2–0.6 级，差 32 级以上的像素 ≤ 0.8%，5 × 5 平均后最大 13 级；
  - `render_view_contract`：
    - 带铺排的纹理在 Vulkan（本机设备）与 D3D11 WARP 上一致：高对比纹素之间至多差 8 级（硬件的子纹素权重），超过 2 级的通道不到 0.1%；
    - sRGB 视图的编码值往返不变，BGRA 的通道正确，缩到四分之一取第三层，D3D11 与 Vulkan 都测；
    - 拒绝声明为线性的 sRGB 视图与层数不符的视图；
    - Vulkan 拒绝画进帧绑定的纹理，接受第一版结构；
    - D3D12 在同一适配器上写、D3D11 画，两帧各是 D3D12 为它写的内容；Vulkan 验证层零告警；
  - `render_backend_contract`：未知的格式位、超过一整条链的层数、按层计的字节；
  - 27 个变异全部被发现。第一轮存活 1 个：关键帧简写里不带空格的斜杠（测试原来只写了带空格的），补了反例；
  - `ctest`：Release 164/164，Debug 164/164（第一次并行运行时 `d3d11_warp_gallery_preview` 段错误一次，单独、负载下 3 次与再跑全套都通过，未能复现，原因未查明）（新增 1 个）；产物与 12.3 基线相比与 13.4 相同，现有画面不变；Android 零警告。
  - 命令与产物：同 13.4；`build/windows-msvc/artifacts/scene-background-<配置>/`（原生与参考并排）与 `scene-paint-reference-<配置>/texture-*.png`；Chrome 截图：`node tools/browser_scene_paint_reference.mjs`。
- **限制**：
  - 文档页面不支持（文档页面整体没有背景图片）；
  - Vulkan 的跨 API 导入未测（宿主可用外部内存扩展自己导入）；
  - D3D12 与 UE 的 RHI 后端仍在 M5；
  - 渐变与效果仍铺满边框盒并重复，非初始的重复与盒子是诊断；
  - D2D 参考光栅只用纹理的第一层；
  - `background-attachment: fixed` 与 `local`、多层背景不支持。
- **下一步**：13.5 增量管线。

重构第 13 步之四：CRPG 浮动 UI（2026-10-15），13.4 完成。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14)、[与引擎通信](engine-communication.md#13-性能目标与测量方法) 与 [兼容表](compatibility.md)。
- **13.4a：原样组件所需的 CSS**：CRPG 的 `FloatingUIOverlay`、`HealthBar`、`DamageNumber`、`AbnormalFlagFloating` 不改一行跑在 scene 上，补齐它们用到而 scene 不支持的：
  - `text-overflow: ellipsis`（按 Blink 截断行，省略号用块的字体与颜色，几何不变；flex 容器直接包含的文字不截断，同 Chrome）；
  - `text-shadow`（多层，σ = 模糊半径的一半，每行文字的覆盖率模糊后着色，按内容缓存）；
  - svg 根上的 `filter: drop-shadow()`（svg 的路径与形状合成一张覆盖率，偏移、模糊、着色，画在下面；锚点元素只准备一次）；
  - `box-shadow` 的 `inset`（背景之上、边框之下）；
  - `radial-gradient()` 的形状、尺寸与位置（默认形状由圆改为标准的椭圆），`repeating-linear-gradient()` 与 `repeating-radial-gradient()`，`to <边>`；原生 pass 与 D2D 参考共用一份解析；
  - em 与 rem（计算值阶段换成 px）、`inset` 一到四个值、绝对定位盒子的 `calc(百分比 ± 长度)` inset、`transform-origin` 的关键字、零深度的 `translate3d()` 与 `translateZ()`；
  - 范围外的写法（HTML 元素的 `filter`、其他滤镜函数、椭圆内圆角里的内阴影、非绝对定位的 calc inset 等）给诊断或整帧回退，见兼容表。
- **13.4b：锚点对比推 JSON**：新基准 `weft_floating_benchmark`。同一场模拟战斗（单位移动、离开屏幕或可见范围；每 12 帧两个伤害数字，每 45 帧一个状态标记；减血、躺下与复活，危急时脉动），两种方式驱动原样的组件：
  - 推 JSON：原样的 `FloatingUIOverlay`，引擎每帧经 CRPG 兼容层推整批实例；
  - 锚点：同样的实例组件与外层包装（`examples/crpg-gallery/src/floating/AnchoredFloatingUIOverlay.tsx`），跟随引擎每帧写的锚点，实例的出现、消失走事件，字段走视图模型；
  - 结果（RTX 5090 + 9950X3D，固定 CCD0，Release，2560 × 1440 的游戏上缩放 2 的 UI，每档 600 帧，三次运行的中位数）：

    | 单位 | View 推进 p50 / p99（推 JSON → 锚点） | 引擎的工作 p50 | 渲染 CPU p50 | VM 峰值 |
    | --- | --- | --- | --- | --- |
    | 12 | 23.8 / 41.0 → 4.75 / 13.6 ms | 0.091 → 0.004 ms | 2.25 → 1.64 ms | 17.5 → 15.2 MiB |
    | 50 | 55.8 / 103 → 8.53 / 23.5 ms | 0.175 → 0.008 ms | 4.98 → 3.98 ms | 30.6 → 28.0 MiB |
    | 200 | 191 / 336 → 29.3 / 63.4 ms | 0.479 → 0.021 ms | 14.4 → 14.6 ms | 79.1 → 64.1 MiB |

  - 锚点方式剩下的开销：伤害数字、状态标记与危急血条的 framer-motion 动画每帧改 DOM，样式、布局与场景按整个文档重算，渲染线程每帧重新准备整个 pass。这是 13.5 增量管线的事。GPU 两种方式相同（200 个单位 p50 约 1.6 ms）；
  - 测出来、已处理的问题：
    - framer-motion 12.38 的 AnimatePresence 要等全部退场结束才一起移除离开的实例。每 0.1 s 一次命中时退场一直重叠，伤害数字一个也离不开文档，节点与内存随战斗增长。这是库的规则，与运行时无关，浏览器里一样；基准改为成批命中。CRPG 页面在密集战斗里会这样，需要时让实例各自退场；
    - 200 个单位超出 VM 的 64 MiB 与文档的 2,048 个节点：上限改为 256 MiB 与 16,384 个。View 的故障消息原来只说 `interrupted`，现在写明是 VM 内存上限还是执行预算；
    - 交付引擎的输入时页面的时钟还停在上一帧，处理函数里开始的动画比任务、rAF 里开始的早一帧。`View::advance` 现在先把时钟推到这一帧（`ScriptDom::advance_clock`）；
    - 锚点页面的写法：一次交付的事件合成一次发布（`setTimeout(0)`），实例用 `memo`，AnimatePresence 关掉 `presenceAffectsLayout`。不这样做时，200 个单位有事件的帧约 600 ms，p99 约 190 ms。
- **验证**：
  - 新测试 `scene_paint_reference`：Chrome 155 截图对照 11 个用例（省略号的几种排版、文字阴影、名牌、发光、四种 svg 图标与形状的投影、血条与危急框的内阴影、几种内阴影、各种径向渐变、重复渐变、calc 与 transform-origin），原生 D3D11 WARP 与 D2D 参考光栅各自比较：
    - 形状、阴影、渐变与布局 5 × 5 平均后最大差 0–12 级；
    - 文字的截断位置、省略号颜色与阴影层序与 Chrome 相同，平均差不超过 2.3 级（差异来自字形光栅）；
    - Vulkan（本机设备）与 D3D11 WARP 至多差 2 级；
  - 新测试 `scene_floating_css_contract`：渐变、阴影、滤镜的解析与诊断，内阴影的像素，变换，em、rem 与 calc 的布局，省略号的行，路径阴影与文字阴影的条目，列表校验；
  - 新测试 `floating_benchmark_quick`（WARP，12 个单位，150 帧战斗加 240 帧安静期）：
    - 两种方式画面一致，差异只在锚点方式由 GPU 重采样的 svg 图标上（图标大小的区域，墨迹质心偏移不超过半个像素，各通道墨量差不超过覆盖率的 10%）；
    - 锚点方式在安静期不发布帧；
    - 离开的实例在战斗中至多 5 个，安静期后为零；
    - 没有页面诊断；
  - `view_contract`：交付时的时钟；`script_dom_lifecycle`：故障消息里的上限（捕获后继续运行、超过上限的重新分配、重置后不带旧原因）；
  - 变异：13.4a 的 40 个与 13.4b 的 7 个全部被发现。第一轮存活 3 个：
    - 内阴影洞的圆角没有缩小：补了契约反例与 Chrome 用例；
    - 内阴影画在边框之上：这个变异写错了，改正后被发现；
    - 超过上限的重新分配：测试的缓冲在 QuickJS 的 arena 里，增长不走重新分配，改为从 1 MiB 起增长；
  - `ctest`：Release 163/163，Debug 163/163（新增 3 个）；产物与 12.3 基线相比：已知按时序变化的套件、13.1b 的图层复用数与 13.2 的 VM 峰值之外，径向渐变换了着色器算法，glass-page 的 9 张与 css-03 的 3 张图各有 2–7 个像素差 1 级，其他画面不变；Android 零警告。
  - 命令与产物：
    - 测试：`ctest --preset windows-release --parallel 6 --output-on-failure`（与 `windows-debug`）；
    - 产物对比：`python -I tools/compare_artifacts.py build/step12-3-baseline build/windows-msvc/artifacts`；
    - 基准：`build/windows-msvc/bin/Release/weft_floating_benchmark.exe --full <dir>`，结果在 `<dir>/floating.json` 与两种方式的截图；
    - 页面：`node tools/build_floating_page.mjs` 与 `python -I tools/compile_gallery_cluster_styles.py`；
    - Chrome 截图：`node tools/browser_scene_paint_reference.mjs`。
- **限制**：
  - 200 个单位时锚点方式的 View 推进 p50 仍约 29 ms，超过 60 Hz 的预算：动画每帧改 DOM 后按整个文档计算（13.5）；
  - 锚点方式的 p99 有 QuickJS 的循环回收（约 60 MiB 的堆，一次约 100 ms）；
  - 锚点元素里的 svg 图标由 GPU 重采样，与就地栅格化相比边缘差几级；
  - 伤害数字没有用元素池（实例组件原样）；
  - 文字阴影只做 CPU 模糊；HTML 元素的 `filter` 不支持；
  - 只在本机 RTX 5090 上测过，没有 Android 真机数据。
- **下一步**：13.3c 的五项纹理限制（用户 2026-10-15 定为中优先级，接在 13.4 之后），再做 13.5 增量管线。

重构第 13 步之三（c）：页面显示引擎的纹理（2026-10-15），13.3 完成。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14) 与 [与引擎通信](engine-communication.md#8-对外-view133)。
- **结果**：
  - CSS `background-image: -weft-texture(name)`（Weft 扩展，也可写在 `background` 简写里）：引擎每帧在 GPU 上画的纹理（如 UE 的 render target）直接画进页面，不经 CPU。像 CSS 的背景图那样在背景色之上、边框之下，拉伸铺满边框盒，圆角裁剪，对齐设备像素；页面只写名字，不合法的名字是 scene 的诊断；
  - 宿主每帧在 `weft_render_scene_frame::textures` 里把名字绑到导入的纹理（外部纹理扩展的租约：D3D11 的着色器资源视图、Vulkan 的图像视图，线性预乘的 RGBA8、RGBA16F 或 RGBA32F）。帧准备好后租约就可以释放，帧的使用完成后才退休；没绑定的名字什么也不画；
  - 新的同步方式 `WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE`：引擎每帧在同一队列上重写同一张纹理，导入一次、一直绑定（原来的冻结方式要轮换多张纹理，每张一个新版本）；
  - scene pass 的新图像方式：采样之后把线性预乘的颜色转成 sRGB 编码值，与页面的其他内容同样合成；行从底部开始的纹理翻转；只换纹理内容的帧仍重画保留的 pass，平面里有纹理的 pass 不保留；
  - 内置 View 后端的扩展入口 `weft_render_d3d11_external`、`weft_render_vulkan_external`；
  - 样例：引擎每帧更新一张小地图纹理（单位的点随血量从绿变红），以 live 方式导入一次，HUD 左下角用 `-weft-texture(minimap)` 显示。样例每帧的 CPU 耗时（同 13.3b 的条件，三次运行的中位数）：D3D11 WARP 的 encode p50 0.28 / p99 0.39 ms（第一帧 25.5 ms），View 推进 p50 0.30 / p99 1.03 / 最慢 1.38 ms；Vulkan 的 encode p50 0.16 / p99 0.26 ms（第一帧 23.0 ms），View 推进 p50 0.16 / p99 0.81 / 最慢 1.33 ms。
- **验证**：
  - 新测试 `scene_texture_contract`：名字的规则与诊断；显示列表里背景色、纹理、边框的顺序，名字只列一次，圆角裁剪，对齐设备像素（旋转的不对齐），显示纹理的元素不进缓存的图层；
  - `paint_contract`：带名字的图片的校验与 `append` 的重新编号，scene pass 的纹理方式，线性 pass 拒绝名字；`render_backend_contract`：显示列表帧拒绝带名字的列表，live 导入；
  - `render_view_contract` 新增：
    - D3D11（WARP）：带纹理的帧与原生 pass 直接画的逐字节相同；四个象限（红、绿、线性灰、半透明的蓝）按页面的编码合成在游戏画面上，圆角外是游戏画面；小地图在红色背景色之上、白边框之下；没绑定的名字什么也不画；准备之后释放租约，提交后不退休，完成后退休一次；行从底部开始的纹理与原生 pass 逐字节相同，与从顶部开始的至多差 4 级（采样器的亚纹素精度，暗处被 sRGB 编码放大）；live 纹理的新内容在保留的 pass 上出现；平面里的纹理每帧取新内容；后端拒绝的帧放开它钉住的租约；拒绝没装扩展、重复或不合法的名字（点、空、65 字节）、保留字段、只有数量没有数组、第一版结构带纹理、sRGB 视图、重复导入、别的设备的纹理、画进帧绑定的纹理；
    - Vulkan（本机设备，验证层零告警）：宿主在帧的命令缓冲里先写纹理再录 UI；与原生 pass 逐字节相同；象限、边框、没绑定的名字、live、行从底部开始、平面；重复导入、别的设备；
  - `engine_host_d3d11_warp`、`engine_host_vulkan`：小地图每帧更新；
  - 44 个变异全部被发现（显示列表的名字、校验与重新编号；名字的规则、简写与诊断；背景色、边框、圆角、对齐与缓存图层；scene pass 的纹理方式与线性 pass；条目的名字、图集合批与平面的保留；两个渲染器的翻转、保留的 pass 与平面；两个后端的导入、方向与别名；核心的名字、保留字段、扩展、第一版结构、失败时的钉住与 live；着色器的编码转换）。第一轮存活 1 个：关键帧里的 `background` 简写由求解器自己展开（元素样式里的由样式模块展开），测试只写了元素样式，补了关键帧的反例。
  - `ctest`：Release 160/160，Debug 160/160（新增 1 个）；产物与 12.3 基线相比与 13.3b 相同（已知按时序变化的套件、13.1b 的图层复用数、13.2 的 VM 峰值），现有画面都不变；Android 零警告。
  - 命令与产物：同 13.3b；`build/windows-msvc/bin/Release/weft_scene_texture_tests.exe`。
- **限制**：
  - 纹理总是拉伸铺满边框盒：不支持 `background-size`、`background-position` 与 `background-repeat`；
  - 只接受线性预乘的 UNORM 或浮点视图；sRGB 视图与 mipmap 不支持；
  - 同设备、同队列：跨设备、跨 API（D3D12 的资源给 D3D11）不支持；UE 插件（M5）走 RHI 后端；
  - D2D 参考光栅不画纹理（报错），文档页面不支持；
  - Vulkan 后端无法检查纹理是否就是目标（D3D11 会拒绝），由宿主保证。
- **下一步**：13.4：按 CRPG 的血条、伤害数字与状态标记做的浮动 UI，改用锚点与事件，与推 JSON 的同一页面对比耗时与画面。

重构第 13 步之三（b）：渲染 C ABI 接 View 的帧（2026-10-14）。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14) 与 [渲染后端 API](render-backend-api.md#view-帧2026-10-14)。
- **结果**：
  - `weft_renderer_prepare_scene`：把 `weft_view_acquire_frame` 取到的帧交给渲染器（能力位 `WEFT_RENDER_FEATURE_SCENE`），之后的录制、提交与完成协议与其他帧相同；准备好的帧自己持有引用，View 销毁后仍可画；
  - 内置后端的 C 创建入口，用宿主的设备：D3D11 `weft_render_d3d11_create`（encode 时画在设备的立即上下文上），Vulkan `weft_render_vulkan_create`（录进宿主的命令缓冲；`weft_render_vulkan_pass` 末尾新增可选的 `target_image`，`backdrop-filter` 据此读游戏画面）；
  - 画在宿主目标的内容之上：不清屏，根元素的背景照常画（不折进清屏），宿主可以要求先清成某个颜色；锚点在 encode 时取最新值；每个 View 一个保留的 pass，只动锚点的帧重画保留的记录，View 销毁后下一次准备时丢弃；
  - 与计划不同：D3D11 不录进宿主的延迟上下文，改为 encode 时画在立即上下文上（scene pass 的上传、平面与图层、背景拷贝依赖立即上下文的顺序；引擎的 D3D11 后端本来也在立即上下文上画），理由见重构计划；
  - 模拟引擎的宿主样例 `samples/engine-host`（`weft_engine_host`，只用公开头文件）：1280 × 720，12 个单位绕圈移动、每帧写锚点，血量走视图模型、伤害走事件，同步函数 `loc.get`，第 30 帧命中查询并点击“结束回合”、回答页面的调用；View 的帧画在每帧变色的游戏画面之上，D3D11 或 Vulkan，最后一帧写成 BMP；
  - 样例每帧的 CPU 耗时（Ryzen 9 9950X3D，固定 CCD0、高优先级，Release，90 帧，三次运行的中位数；GPU 时间没有测）：D3D11 WARP 的 encode p50 0.26 / p99 0.39 ms（第一帧 23.7 ms：建图集、上传字形、建管线状态），View 推进 p50 0.29 / p99 1.06 / 最慢 1.45 ms；Vulkan（RTX 5090）的 encode p50 0.16 / p99 0.23 ms（第一帧 22.8 ms），View 推进 p50 0.15 / p99 0.81 / 最慢 1.50 ms。推进的 p99 是有伤害事件、改了 DOM 的帧（整页的固定开销，13.5 处理）；WARP 的光栅线程与 owner 在同一个 CCD 上，所以 D3D11 那次运行的推进慢一些。
- **验证**：
  - 新测试 `render_view_contract`：
    - D3D11（WARP）：经 C 接口画出的帧与直接用原生 scene pass 画在同样游戏画面上的逐字节相同；页面以外是游戏画面，毛玻璃面板把它下面的游戏画面调暗；宿主的清屏（第一个 pass 与保留的 pass 都是）；锚点移动、场景不变时画在新位置；宿主给的缩放；半透明的根背景混合在游戏画面或清屏颜色之上，不透明的根背景盖住游戏画面；同一个渲染器先替换后叠加时与只叠加相同；保留的 pass（只动锚点时重画、内容变了重新准备）；同一 Runtime 的第二个 View 有自己的 pass，View 销毁后被丢弃；
    - D3D11 的拒绝：延迟上下文、sRGB 目标、与视图不符的格式、别的设备的目标（`WRONG_BACKEND`）、第二级 mip、与 pass 大小不同的目标、不是预乘的清屏颜色、四边形帧、别的 Runtime 的帧、负数与无穷大的缩放、保留字段、短的结构、别的版本、空的帧；声明了能力却没有 `prepare_scene` 的表；没有该能力的后端；View 销毁之后画准备好的帧；有准备好的帧时销毁渲染器返回 `BUSY`；创建参数的错误；
    - Vulkan（本机设备，验证层，零告警）：与直接录制的原生 pass 逐字节相同（包括读目标图像的毛玻璃）；游戏画面、清屏、锚点移动；同一个渲染器先替换后叠加；丢弃的录制归还槽位；两帧在途时第一帧完成不动还没提交的录制（之后又录两帧，都不拿它的槽位）；宿主给的缩放；第一版结构（在 `target_image` 之前结束）照常画，毛玻璃看不到下面；别的设备、不是预乘的清屏颜色、两种过短的结构；第二个 View 被丢弃；
  - 新测试 `engine_host_d3d11_warp`、`engine_host_vulkan`（没有 Vulkan 设备时跳过）：样例在两个后端上跑完，页面就绪、调用得到回答、没有诊断；`render_backend_c_abi` 检查新的结构布局与入口；`render_vulkan_backend_contract` 的显示列表后端接受第一版结构；
  - 57 个变异全部被发现（pass 的起始方式、根背景的折叠、清屏、保留的 pass 与它的起始方式、忘记 View；`prepare_scene` 的校验与表的规则；两个后端的目标检查、清屏、缩放、锚点、View 的编号与丢弃、命令列表；Vulkan 的提交、完成、丢弃、目标图像与借来的设备）。第一轮 54 个里存活 4 个：不透明的根背景在叠加时被折进清屏（两处，测试只有半透明的根）、View 第一个 pass 上的宿主清屏、Vulkan 上宿主给的缩放，补了反例；之后为第一版 Vulkan 结构加了 3 个，也都被发现。
  - `ctest`：Release 159/159，Debug 159/159（新增 3 个）；产物与 12.3 基线相比与 13.2 相同（已知按时序变化的套件、13.1b 的图层复用数、13.2 的 VM 峰值），新的 `engine-host` 只在本次产物里；Android 零警告。
  - 命令与产物：`ctest --preset windows-release --parallel 6 --output-on-failure`（与 `windows-debug`）；`python -I tools/compare_artifacts.py build/step12-3-baseline build/windows-msvc/artifacts`；样例 `build/windows-msvc/bin/Release/weft_engine_host.exe --d3d11 --warp --frames 90 --out <file.bmp>`（或 `--vulkan`，或不带 `--warp` 用硬件 D3D11），测试的输出在 `build/windows-msvc/artifacts/engine-host-<配置>/`；`cmake --build --preset android-arm64`（样例与这些测试只在 Windows 上）。
- **限制**：
  - D3D11 只在立即上下文上画 View 的帧（延迟上下文返回 `UNSUPPORTED_FEATURE`）；目标只支持 UNORM 的 BGRA8（D3D11 另有 RGBA8），不支持 sRGB 视图；
  - 一个后端只画一个 Runtime 的 View；
  - 页面还不能显示引擎的纹理（13.3c）；D3D12 与 UE 的 RHI 后端在 M5；
  - 样例只在 Windows 上；没有窗口与交换链（离屏，最后一帧写成文件），硬件 D3D11 路径没有自动测试；
  - 发布帧仍复制整棵场景树（13.5）。
- **下一步**：13.3c：页面显示引擎的纹理（`background-image: -weft-texture(name)`，宿主每帧给出名字对应的外部纹理租约）。

重构第 13 步之三（a）：对外 View 与它的 C ABI（2026-10-14）。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14) 与 [技术路线](engine-communication.md#8-对外-view133)。
- **结果**：
  - `weft::View`（`src/view`）把探针的画廊会话收拢成通用的部件：加载页面（脚本、样式表与关键帧、场景根）、推进（引擎的输入、任务、rAF、观察者、场景）、指针输入（合成 pointer 与 mouse 事件、over/out、enter/leave 与 click）、命中查询（元素与它继承来的 `cursor`，宿主据此决定点击交给 UI 还是游戏，同 CRPG 现在的做法）、发布不可变的帧（场景的版本号、View 的大小或缩放变了才复制一份场景树）。页面的异常记成诊断，VM 资源耗尽使 View 进入 Faulted；
  - C ABI（`include/weft/weft.h`）：
    - 字体：系统字体、字体文件、通用族与回退族，第一个页面加载时固定；
    - owner 线程：`weft_view_load`、`advance`、`resize`、`pointer`、`hit_test`、`drain_messages`、`register_function`（结果写进 `weft_value_writer`）、`drain_diagnostics`；
    - 任意线程：`weft_view_acquire_frame`（帧比 View 活得久）、`weft_view_write_anchors`、`weft_view_emit`、`complete_call`、`set_model`（MessagePack 或 JSON）；与销毁并发时返回过期句柄；
    - 新结果 `WEFT_SCRIPT_ERROR`、`WEFT_FAULTED`；宿主回调里对同一 View 的 owner 操作返回 `WEFT_BUSY`；
  - 由桥驱动的 HUD 示例页面 `examples/engine-host`：名牌跟锚点，血量来自视图模型，伤害数字来自事件，按钮调用引擎；
  - View 每帧的开销（`weft_view_benchmark --full`，Ryzen 9 9950X3D，固定 CCD0，Release，1920 × 1080，每档预热 30 帧、240 帧）：画廊的锚点 HUD 只移动时推进 0.003 ms（没有新帧）；400 个名牌（1,603 个节点）每帧 20 处掉血时推进 p50 6.3 ms、p99 7.2 ms（与 13.1a 测的 7.1 ms 相同，整页的固定开销留给 13.5），其中发布帧复制场景树 0.53 ms。
- **验证**：
  - 新测试 `view_contract`：
    - HUD 页面：加载与同步函数、事件与视图模型（视图模型先于事件交付，所以新名牌从模型取血量）、伤害数字一秒后消失、按钮的命中与光标、点击调用引擎与它的完成、按下与抬起不在同一元素上时没有 click；
    - 锚点决定命中与 JS 读到的几何，继承的光标，enter、滚轮（默认动作被阻止）与 leave；
    - 诊断：加载失败后 View 为空，处理函数、rAF 与任务的异常，样式做不出场景时保留上一帧；
    - 帧：不变时不发布，只改缩放也发布，另一个线程读帧时帧不变，ResizeObserver 与 resize 事件，时间不倒退；
    - 同一页面在 View 与画廊会话里的场景逐项相同（节点身份除外，两个 VM 各自编号）；
  - 新测试 `c_api_view_contract`：结构体大小与版本；字体（第一个页面之后 `WEFT_BUSY`）；没有页面时；经 C 接口跑 HUD（JSON 与 MessagePack 的事件与完成、latest、视图模型、点击与完成、过期与不合法的完成、命中）；回调里的重入（owner 操作 `WEFT_BUSY`，发事件照常）；锚点（隐藏、非有限数、超量）；owner 操作在别的线程上返回 `WEFT_WRONG_THREAD`，任意线程的操作可用；销毁后与槽位被重用后的过期句柄；4 个线程与 owner 反复创建销毁 View 并发；加载失败丢弃消息与调用；Faulted（加载时与之后的推进，重新加载后恢复）；
  - `c_api_smoke` 用 C11 编译并调用全部新入口；`view_benchmark_quick` 是基准的短跑；
  - 37 个变异全部被发现（View 的发布、交付、观察者、锚点、命中与光标、click、enter/leave、滚轮、加载与诊断、Faulted、时间与 resize；C 接口的重入、代际、销毁、字体、函数、时间、latest、完成、光标、prevented、诊断种类、帧时间、锚点、错误映射、JSON 结果与消息种类）。第一轮 38 个里存活 3 个：失败的加载留下消息与调用、MessagePack 完成的成功标志，补了反例；`weft_view_advance` 里对 Faulted 的提前返回与 View 自己的检查等价，删去（`weft_view_pointer` 里同样的检查一并删去）；
  - `ctest`：Release 156/156，Debug 156/156（新增 3 个）；产物与 12.3 基线相比与 13.2 相同；Android 零警告。
  - 命令与产物：`build/windows-msvc/bin/Release/weft_view_benchmark.exe --full <dir>`（固定 CCD0、高优先级，结果在 `<dir>/view.json`；测试的短跑写到 `build/windows-msvc/artifacts/view-benchmark-<配置>/view.json`）。
- **限制**：
  - 渲染 View 的帧还没有 C 接口（13.3b）；
  - 输入只有指针与滚轮；键盘、IME 与手柄在之后（手柄可以走桥的事件）；
  - 没有页面效果与粒子的注册接口、从内存加载字体、图片等资源的宿主回调；页面脚本由宿主读好交进来，没有模块加载器；
  - 发布帧时复制整棵场景树（1,600 个节点约 0.5 ms），13.5 改成共享不变的子树。
- **下一步**：13.3b：渲染 C ABI 接 View 的帧，内置 D3D11、Vulkan 后端的 C 创建入口（用宿主的设备），模拟引擎的宿主样例。

重构第 13 步之二：桥接（2026-10-14），13.2 完成。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14) 与 [技术路线](engine-communication.md#6-桥接)。
- **结果**：
  - 带类型的值：MessagePack 的子集（null、布尔、int64/uint64、有限浮点、UTF-8 字符串、字节、数组、键为字符串的对象），编解码自己写（`src/bridge`，不引入第三方库），单个值至多 1 MiB、嵌套至多 64 层；也接受 JSON 文本（严格的 RFC 8259），在给出的线程上转换；
  - 引擎一侧（`bridge::EngineChannel`）：任意线程发事件（可选替换同名未交付的那个）、完成调用、合并视图模型的字段；有界，满了返回 Full、不丢；页面打开的调用登记在这里，已完成、超时关闭、重置之前的或从未打开的调用的完成立即返回 Stale；
  - 页面一侧 `weft.engine`（平台模块 `Engine`）：`on`、`once`、`onAny`（都返回取消函数）、`send`、`call`（Promise，可带超时）、`invoke`（宿主的同步函数）、`model(name)`（`{name, value, subscribe}`，可直接给 React 的 `useSyncExternalStore`）；
  - owner 每次交付（`ScriptDom::deliver_engine`）先交视图模型的批次，再按投递顺序交事件与完成，每项之后一次微任务检查点，共用一个执行预算；处理函数的异常记下，不影响其他处理函数；页面发出的消息由宿主在 owner 上取走（`take_engine_messages`）；
  - 值在原生侧直接建成 JS 对象与数组，不拼脚本、不求值；超过 2^53 的整数是 BigInt，字节是 Uint8Array；从 JS 转出时 toJSON 与 Proxy 包着的数组同 JSON.stringify，函数、symbol、循环引用与非有限数被拒绝；
  - 同步函数在 JS 执行当中被调用，期间进入 ScriptDom 的任何接口都被拒绝（`WEFT_ENGINE_REENTRY`）；
  - CRPG 兼容层（平台模块 `CrpgBridge`）：`window.ue.sendEvent` 照 CRPG 宿主的规则发消息，引擎的事件转给页面定义的 `window.__ue_dispatch`，视图模型 `vm` 每一批变化的字段转给 `window.__vm_update`；CRPG 的页面代码不用改；
  - 对比基准（`weft_bridge_benchmark --full`，Ryzen 9 9950X3D，固定 CCD0，Release，每种 600 帧、预热 60 帧，三次运行的中位数；同一个按 CRPG ue-bridge 写的页面，处理函数读出全部字段）。UI 线程每帧 p50 / p99 / 最慢：

| 每帧 | 负载 | 拼进脚本求值（CRPG 现在的做法） | 桥接，引擎给 JSON | 桥接，引擎写 MessagePack |
| --- | --- | --- | --- | --- |
| 1 个事件 | 血量（7 字节） | 3.5 / 5.5 / 9.6 µs | 1.1 / 1.2 / 2.2 µs | 1.1 / 1.8 / 1.9 µs |
| 17 个事件 | 血量 | 59 / 89 / 109 µs | 14 / 24 / 38 µs | 14 / 23 / 38 µs |
| 1 个事件 | 市场（1 KB，8 项） | 32 / 56 / 71 µs | 6.2 / 9.8 / 31 µs | 6.1 / 11 / 40 µs |
| 17 个事件 | 市场 1 KB | 0.56 / 0.68 / 0.90 ms | 0.10 / 0.14 / 0.18 ms | 0.10 / 0.13 / 0.17 ms |
| 1 个事件 | 市场（10 KB，77 项） | 265 / 327 / 396 µs | 47 / 59 / 80 µs | 47 / 59 / 73 µs |
| 17 个事件 | 市场 10 KB | 4.49 / 4.82 / 6.07 ms | 0.79 / 0.90 / 1.12 ms | 0.78 / 0.88 / 1.07 ms |

  - 每秒约 1,000 个 10 KB 的事件（每帧 17 个）时，UI 线程每帧从 4.49 ms 降到 0.79 ms（p99 4.82 → 0.90 ms），约 5.7 倍；剩下的几乎全是建 JS 对象。引擎写 MessagePack 与给 JSON 在 UI 线程上一样快：JSON 在给出的线程上转换，10 KB 约 18 µs（p99 30 µs）；
  - 调用的往返（宿主取到就完成）：页面在一帧里调用，下一帧兑现。
- **验证**：
  - 新测试 `bridge_contract`：
    - 随机值往返：3,000 个随机值，两种容器头写法，JSON 两次往返不变；每个截短的前缀都被拒绝；2 万个随机改了字节的值要么被拒绝，要么能转换；
    - 最短编码的各个边界；读的一侧接受较长的形式；
    - 拒绝的输入：类型外、扩展类型、非有限浮点、不是 UTF-8、截短、个数超过剩余字节、键不是字符串、多余字节、深度与大小的上限（恰在上限上的被接受）；
    - JSON：空白、转义、代理对、整数与浮点的界、−0，以及 37 种不合法的文本与超过深度、大小上限的文本；
    - 通道：顺序、替换、名字、上限（满时完成不关闭调用）、过期的完成、视图模型的合并（同一个键出现两次只算一次字节）；
    - 多线程：6 个线程各发 2 万个事件、1 个线程 2 万次替换、2 个线程抢着完成 2,000 个调用，owner 同时取：每个线程的事件按序且不丢，替换的值不回退，每个调用恰好完成一次；
  - 新测试 `engine_bridge_contract`：
    - 两个方向的转换（整数与 BigInt 的界、字节与视图的偏移、`__proto__` 键不改原型、−0、孤立代理项、Proxy、toJSON、拒绝的值与名字、上限）；300 轮随机值经页面原样发回，引擎 → JS → 引擎不变；
    - 事件：处理函数的顺序、once、交付中途取消与新增、异常隔离、每个事件之后的检查点；
    - 调用：兑现、三种拒绝、超时、完成在超时之前给出而在之后交付（stale）、重置之后的完成被拒、id 不重用；同步函数的结果、异常、坏结果与四种重入；
    - 视图模型：合并、冻结、先于事件、批次中途取消订阅；没有通道时；
    - CRPG：按 CRPG `ue-bridge.ts` 编译出的代码与 MVVM 测试页，跑引擎侧的 `pushUI`、`loadMarket`、`setLocale` 与视图模型样例，`sendEvent` 的缺省值与 JSON.stringify 规则；
  - `bridge_benchmark_quick`：基准的短跑，三种方式交给页面的值的校验和必须相同；
  - 66 个变异全部被发现（值的读写与 JSON、通道、ScriptDom 的转换、交付与同步函数、两个 JS 模块）。第一轮存活 1 个：`sendEvent` 不给缺省值，因为 CRPG 的页面自己先补了 `{}`；对照 CRPG 宿主（`UEBridgeSendEvent`）补了反例，并照它处理不是字符串的名字与 JSON.stringify 会省略的值；
  - `ctest`：Release 153/153，Debug 153/153（新增 3 个）；产物与 12.3 基线相比，不同的只有已知按时序变化的套件、13.1b 已说明的图层复用数，以及 gallery 与 scene-gallery 报告的 VM 峰值多 1,000 字节（节点原型上新增的四个原生方法；去掉它们时与基线相同），新的 `bridge-benchmark` 只在本次产物里；Android 零警告。
  - 命令与产物：`ctest --preset windows-release --parallel 6 --output-on-failure`（与 `windows-debug`）；`python -I tools/compare_artifacts.py build/step12-3-baseline build/windows-msvc/artifacts`；基准 `build/windows-msvc/bin/Release/weft_bridge_benchmark.exe --full <dir>`（固定 CCD0、高优先级，结果在 `<dir>/bridge.json`；测试的短跑写到 `build/windows-msvc/artifacts/bridge-benchmark-<配置>/bridge.json`）；`cmake --build --preset android-arm64`（Android 不构建探针，基准未在 Android 上编译）。
- **限制**：
  - 只有内部 C++ 接口与 JS 模块；C ABI（`weft_view_emit`、`weft_view_complete_call`、`weft_view_set_model`、取消息、注册函数）与对外 View 在 13.3，交付现在要宿主调用（13.3 在推进开始时做）；View 处于 Faulted 时的投递也在 13.3；
  - 交给 JS 的开销主要是建对象：10 KB 的市场数据约 47 µs；字节总是复制（零拷贝的 ArrayBuffer 留给技术路线 6.5）；
  - 从 JS 转出时函数与 symbol 成员被拒绝，不像 JSON.stringify 那样省略（CRPG 兼容层的 `sendEvent` 例外，照 CRPG 宿主）；
  - 画廊与 HUD 探针还没接桥接（13.3、13.4）。
- **下一步**：13.3 C ABI 与对外 View（加载、推进、输入、命中、取帧；锚点与桥接的 C 接口；模拟引擎的宿主样例）。

重构第 13 步之一（b）：锚点的 GPU 路径与保留的 pass（2026-10-14），13.1 完成。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14) 与 [技术路线](engine-communication.md#53-渲染线程取值与应用)。
- **结果**：
  - 场景帧带版本号（`SceneFrame::revision`）：求解器只在布局或绘制的内容变了（或帧有动画）时换新的版本号。渲染器为每个视图保留上一份准备好的 pass，版本号相同的帧不再重建显示列表、规划与上传记录，只把这一帧的锚点（每个锚点元素 16 字节）和记录顺序传给 GPU 再画一遍；
  - 锚点子树在显示列表里留在元素自己的原点，记录里记下它属于哪个锚点元素；顶点着色器按锚点平移、缩放四个角，像素着色器把像素位置换回元素自己的坐标再照常求覆盖、裁剪与采样；
  - 同一父元素下按深度排序：每帧算出新的顺序，在一个批次里的就写顺序缓冲，跨批次（例如中间有透明度组）时重新准备这一帧；
  - 锚点子树里有背景模糊、粒子、透视平面或着色器效果时不能由 GPU 移动，这样的帧仍走 CPU 参考路径（每帧准备）；
  - HUD 基准（4K，RTX 5090 + 9950X3D，固定 CCD0，Release，每档 240 帧）：

| 400 个名牌只移动（锚点） | UI 线程 p50 / p99 | 渲染 CPU p50 / p99 | GPU p50 |
| --- | --- | --- | --- |
| D3D11，CPU 参考路径（13.1a） | 0.025 / 0.034 ms | 4.16 / 5.17 ms | 0.08 ms |
| D3D11，GPU 路径 | 0.004 / 0.019 ms | 0.021 / 0.114 ms | 0.06 ms |
| Vulkan，GPU 路径 | 0.006 / 0.029 ms | 0.042 / 0.111 ms | 0.23 ms |

  - 达到 13.1 的目标（UI 线程 ≤ 0.1 ms，渲染 CPU p50 ≤ 0.1 ms、p99 ≤ 0.3 ms）。推 JSON 的同一画面每帧约 23 ms（UI 线程）加 4 ms（渲染）；
  - 有掉血时（任何 DOM 变化）帧换了版本号，仍要整页更新与重建，UI 线程 7–10 ms、渲染 CPU 约 4 ms，留给 13.5；
  - 没有锚点的页面也受益：版本号不变的帧直接重画保留的 pass。
- **验证**：
  - `render_scene_anchor_contract` 新增 GPU 路径：同一批名牌带版本号画，与 CPU 参考路径对照：Vulkan（RTX 5090）逐像素相同，D3D11 WARP 至多差 2 级（约 1,300 个像素，采样坐标末位舍入不同）；保留的 pass 移动并重排、stay、透明名牌（重排时重新准备，计数 `kept_rebuilds`）、带背景模糊的名牌（每帧准备，与 CPU 路径相同）、换缩放或图层时重新准备，以及伸到元素原点左边的 SVG 路径（锚点在整像素上，差 0）；
  - `scene_anchor_contract` 新增：延后的显示列表（锚点元素、记录范围、元素空间的变换与裁剪、按 z-index 分开的排序组、组不随锚点移动、背景模糊不能延后）；每帧放置（映射、隐藏、缩到 0、屏幕外、stay、深度相同的顺序、重排的记录顺序、跨批次时要求重新准备、记录的锚点序号越界被拒绝）；求解器的版本号（不变的帧保持、重画或移动后换新、整帧重建与重置后换新）；
  - 33 个变异全部被发现（规划器的锚点序号与遮罩，着色器的移动、隐藏、换回与抗锯齿，延后列表的绘制项、组、排序组与不能延后的项，每帧放置的映射、隐藏、stay、深度相同与重排，两个渲染器的保留与上传，帧版本号）。第一轮存活 3 个：缩小时的抗锯齿外延、Vulkan 换版本号时重新准备（补了反例），以及重置后清零版本号（重置后必然整帧重建，与之等价，删去这行）；Debug 第一遍还发现 `place()` 先换掉 stay 记忆、再用它的深度排序（释放后读取，Release 下碰巧不错），已修正，新反例在 Debug 下能发现它；
  - `ctest`：Release 150/150，Debug 150/150；产物与 12.3 基线相比图像全部相同，scene-native 与 scene-vulkan 的报告里图层复用数变少（保留的 pass 不再查缓存），其余只有已知按时序变化的套件；Android 零警告。
  - 发现一处约定问题：`scene_particle_page` 复制会话的帧、手工删掉粒子后用同一个渲染器再画，画出的是保留的 pass。约定写进 `SceneFrame::revision`：在求解器之外改了帧内容的代码要把版本号清零；测试已改。
- **限制**：锚点平移不在整像素上时，GPU 路径里 SVG 路径的覆盖率图按双线性采样，边缘略软（盒子与文字不受影响）；锚点缩放时路径与阴影图按原尺寸采样；虚线边框按元素原点处的像素网格布置；Vulkan 每帧仍重新分配描述符集（已计入上表的 0.042 ms）；保留的 pass 里的透视平面在 Vulkan 上每帧重画内容。
- **下一步**：13.2 桥接（带类型的值、双向事件、调用与 Promise、视图模型、CRPG 兼容层）。

重构第 13 步之一（a）：锚点的 CPU 参考路径（2026-10-14）。详见 [重构计划](refactoring-plan.md#第-13-步记录2026-10-14) 与 [技术路线](engine-communication.md#5-锚点)。
- **结果**：
  - 页面在元素上写 `data-anchor="<key>"`（可选 `data-anchor-pivot`、`-scale`、`-offscreen`、`-margin`、`-missing`），元素就跟着引擎写的锚点走：布局仍决定它的大小，位置由锚点决定；
  - 引擎每帧整份写一张锚点表（每个锚点 32 字节：key、位置、缩放、深度、标志），任意线程、不加锁；渲染线程与 owner 各有一个三缓冲，各自取最新的整张表，互不等待；
  - 显示列表、owner 的命中与几何读取用同一套规则：pivot 对准锚点、绕它缩放；裁剪与 `position: fixed` 相同（祖先的变换与 overflow 裁剪不作用）；同一父元素下按深度由远到近；锚点在 View 外或相机后时隐藏、照画或贴边（贴边时整个元素留在 View 内）；表里没有它的 key 时隐藏或停在上次的位置；
  - HUD 基准新增锚点模式（RTX 5090 + 9950X3D，固定 CCD0，Release，4K，每档 240 帧，约 1,600 个节点）：

| 400 个名牌，每帧 | 引擎写锚点与 JSON | 脚本 | UI 线程 p50 / p99 | 渲染 CPU p50 |
| --- | --- | --- | --- | --- |
| 只移动（锚点） | 0.012 ms | 0 | 0.025 / 0.034 ms | 4.2 ms |
| 移动（锚点）+ 20 个血条改宽度 | 0.017 ms | 0.16 ms | 10.7 / 13.2 ms | 4.0 ms |
| 移动（锚点）+ 20 个血条改 `scaleX` | 0.018 ms | 0.33 ms | 7.1 / 10.1 ms | 4.1 ms |
| 对照：推 JSON，每帧改 400 个 transform 与 20 个宽度 | 0.17 ms | 3.9 ms | 23.3 / 32.1 ms | 4.2 ms |

  - 只移动时 UI 线程每帧从 23 ms 降到 0.03 ms。渲染 CPU 没变：CPU 参考路径每帧重建整份显示列表，这是 GPU 路径要解决的；
  - 只要有一处 DOM 变化（掉血），UI 线程就按整个文档重新求样式与场景，改宽度时还要整页重新布局；改 `scaleX` 省去布局。剩下的是 13.5 增量管线要去掉的开销。
- **验证**：
  - 新测试 `scene_anchor_contract`：
    - 属性语法与诊断：求解器拒绝不合法的值与 svg 内部元素上的锚点，改 pivot 时报告几何变化；
    - 表按 key 排序、同一 key 取最后一条、非法值与超量不发布；一个写线程与两个读线程并发（至少 2 万次写，每个读者至少看到 1,000 张表）：每张表都完整、序号不回退、最后取到最后一次写；
    - 定位：pivot、缩放、隐藏标志、View 外（边界在内、相机后）、贴边（含自身变换与边距、放不下时居中）；深度只在同一 z-index 的锚点元素之间排序；stay 只认同一元素、同一 key；
    - 显示列表与“同样的盒子用 CSS 放在那里”逐项相同（祖先的位移与圆角裁剪不作用）；透明度组的合成范围、图层（有锚点子孙的不做成图层，锚点元素本身做图层时图像跟着走）、行内盒与透视平面的回退；
    - 命中（深度决定谁在上、祖先裁剪不作用、隐藏的不命中、stay）与几何（矩形、offsetParent 与偏移、隐藏的为空盒）；
  - 新测试 `render_scene_anchor_contract`：10 个名牌（小数坐标与缩放、深度重叠、贴边、隐藏、View 外、缺失），放在一个会移动、有圆角裁剪的盒子里；D3D11 WARP 与 Vulkan（验证层）上与 CSS 把它们摆在同样位置的画面逐像素相同（差 0），换一张表、stay 时也相同；Vulkan 与 D3D11 至多差 2 级；
  - 51 个变异全部被发现（表与三缓冲、语法、定位与贴边、深度顺序、stay、显示列表的裁剪与变换、图层与组、命中与几何、参考光栅、预处理器的记忆、求解器、节点相等、两个渲染器传表）。第一轮存活 2 个，补了反例：测试节点写的 `border-radius` 简写对手工构造的节点不起作用（祖先没有圆角裁剪链），命中的 stay 记忆被前一次几何读取掩盖；
  - `ctest`：Release 150/150，Debug 150/150（新增 2 个；Debug 第一遍 `vulkan_scene_window` 在并行负载下 5 秒内只比到 33/34 个场景，单独重跑 4 次与整套重跑均通过）；产物与 12.3 基线相比场景像素全部相同，不同的只有已知按时序变化的套件（实时窗口的截图与滚轮事件、画廊预览窗口的动画截图：同一二进制两次运行也不同、trace 时间戳、异步合并与退休计数）；Android 零警告。
- **限制**：
  - CPU 参考路径每帧重建显示列表，只移动时渲染 CPU 与改 DOM 时相同；
  - D2D 参考光栅不画锚点元素（与没有锚点表时相同），退回参考光栅的帧里看不到它们；
  - 锚点元素不能是行内盒或在透视平面里（整帧回退，计入原因）；svg 内部的元素不能写锚点（诊断）；不合法的锚点属性按其他语法错误处理，整帧诊断（技术路线原写“元素隐藏”）；
  - 引擎写锚点的公开 C 接口在 13.3；现在由内部 C++ 接口与探针驱动；画廊会话（`GallerySceneSession`）的命中已接锚点，几何读取每次重算（13.3 的 View 再缓存）。
- **下一步**：13.1 的 GPU 路径：页面没变时显示列表与绘制记录留在显存里，每帧只上传锚点元素的变换与顺序，目标 400 个名牌只移动时渲染 CPU ≤ 0.1 ms。

重构第 12 步之三：图集合批、1 字节覆盖率与 Vulkan 描述符集（2026-10-14）。详见 [重构计划](refactoring-plan.md#第-12-步记录2026-10-14)。
- **结果**：
  - 隔离基准（同一串 4K 帧，固定 CCD，6 轮交替中位数）：D3D11 K 线渲染 CPU p50 0.907 → 0.828 ms、p99 1.73 → 1.34 ms，散点图 p50 0.777 → 0.655 ms、p99 1.26 → 0.86 ms；Vulkan K 线 p50 0.844 → 0.806 ms、散点图 0.764 → 0.716 ms；D3D11 K 线首帧 10.2 → 5.9 ms；峰值工作集 288 → 244 MiB；画廊窗口 K 线重绘 p50 0.99 → 0.83 ms、散点图重绘 p99 1.11 → 0.91 ms；
  - 路径覆盖率与单色文字每像素 1 字节（原 4 字节）；小的文字、阴影与路径图像放进按格式分页的图集，相邻的盒子与同页图像合成一次绘制：K 线每帧绘制 58 → 9 次，散点图 30 → 3 次；
  - Vulkan 每个 pass 按采样去重描述符集；图集之后绘制已很少，这一项在 RTX 5090 上测不出差别；
  - 新基准 `--scene-interaction-benchmark-hardware`：同一串 K 线、散点图帧在 D3D11 与 Vulkan 上各画一遍。
- **验证**：新测试 `render_scene_atlas_contract`（开关图集逐像素对比、区域重用、Vulkan 多录制与丢弃，验证层零告警），`scene_native_contract` 与 `paint_contract` 新增图集用例；20 个变异发现 19 个（剩下的“边框填 0”与现有内容等价）；`ctest`：Release 147/147、Debug 147/147（新增 `render_scene_atlas_contract` 与 `d3d11_warp_scene_interaction_benchmark`）；Android 零警告。
- **限制**：图集里的图像与独立纹理采样坐标的末位舍入不同，边缘像素差 1–3 级（计划原写“像素不变”，产物差异见计划记录）；WARP 上旋转的单色文字因单通道过滤精度差 1 级；Vulkan 描述符的收益在本机测不出，移动端未测。
- **下一步**：与用户一起看剩下的热点（虚线十字线、字形轮廓的光栅化、属性变了的元素重新解析）。

重构第 12 步之二：SVG 基本形状画成盒子（2026-10-14）。详见 [重构计划](refactoring-plan.md#第-12-步记录2026-10-14)。
- **结果**：
  - 散点图拖动（4K，各 3 次中位数）：卡片重绘 p50 5.19 → 0.65 ms；渲染 CPU p50 6.62 → 1.56 ms、p99 8.06 → 2.77 ms；FPS 151 → 618；每帧路径光栅化 108 → 0.4 张；
  - SVG 的矩形（直角或圆角半径相同）、圆的填充与实线描边、线段（平头、方头、圆头）画成盒子，不再光栅化，按批绘制；椭圆、不等半径圆角、切角连接、虚线等仍是路径；
  - K 线的柱与影线也成了盒子，绘制调用 547 → 62 次，重绘时间不变（瓶颈在 SVG 值缓存的校验）；
  - 修正盒子着色器：旋转或圆角的带边框盒子外缘原来取不到边框色、是锯齿，现在与 Chrome 一致（CSS 元素同样受益）。
- **验证**：
  - 新测试 `render_scene_shape_boxes_contract`：16 种形状画成盒子与同轮廓的路径逐像素比较，水平与竖直边差 0–2 级，曲边与斜边只差在抗锯齿；不能精确表达的形状仍是路径；Vulkan 与 D3D11 最大差 1；
  - `scene_cluster_platform` 改为断言盒子；画廊各项 Chrome 对照全部通过；
  - 15 个变异全部被发现；`ctest`：Release 145/145，Debug 145/145（新增 1 个）；产物：含 SVG 形状与圆角、旋转边框的截图在边缘像素上变化（最大差 38–116 级，每张至多几千个像素，以上 Chrome 对照均通过），其余只有已知按时序变化的套件；Android 零警告。
- **限制**：盒子按距离抗锯齿，曲边与斜边上个别像素与面积覆盖差到 50 级左右；K 线的瓶颈转到 SVG 值缓存的逐项校验。
- **下一步**：12.3 图集合批、1 字节覆盖率、Vulkan 描述符集。

重构第 12 步之一：K 线的 SVG 与文字缓存（2026-10-14）。详见 [重构计划](refactoring-plan.md#第-12-步记录2026-10-14)。
- **结果**：
  - K 线悬停扫动（4K，`kline-sweep`，各 3 次中位数）：每次图表更新的重绘 p50 3.42 → 1.48 ms、p95 3.99 → 2.18 ms；渲染 CPU p95 5.29 → 3.16 ms；FPS 406 → 618，图表更新 148 → 208 次/s；
  - SVG 元素的派生值（变换、透明度、文字位置与轮廓、路径、填充与描边）按元素缓存，元素自己的值不变就不再解析；
  - 文字图像按内容缓存：tooltip 每次 `innerHTML` 生成的新节点，文字相同就复用，每次更新新建的文字图像从约 12 张降到 0.6 张；
  - 路径图像的键改为每个路径对象只算一次的内容摘要，命中时再核对路径；
  - 画廊窗口在渲染线程之外释放旧的场景帧（回收线程），单独约省 0.4 ms。
- **验证**：
  - 新测试 `scene_display_cache_contract`：1200 个随机编辑帧带缓存与不带缓存的显示列表逐字节相同（含 45 帧回退）；复用与重取的计数；视图隔离；源键与相等性严格对应；路径键的复用与重画反例；
  - `scene_native_contract`：同文字的其他节点复用，文字或缩放改变时另建；
  - 19 个变异全部被发现；`ctest`：Release 144/144，Debug 144/144（新增 1 个）；产物与 11.5c 基线相比场景像素全部相同，render-stages 的图像淘汰数 35 → 31（文字按内容缓存），其余只有已知按时序变化的套件；Android 零警告。
- **限制**：SVG 值缓存每个元素仍要逐项比较属性与样式（K 线重绘采样约 14%）；变化的路径仍要光栅化并新建纹理。
- **下一步**：12.2 基本形状画成盒子（散点图）。

重构第 11 步之五（c）：画廊的半透明窗口组，11.5 完成（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - 画廊窗口新增 “Translucent Windows” 一组三例（75 例、13 组，原生绘制 71 例）：毛玻璃（模糊、饱和度、半透明底色与高光边）、普通半透明（只有底色）、只模糊背后（无底色）。每例 4 个叠放的窗口，按标题栏拖动并置顶，− / + 在 1–32 之间增减；背景（漂移的光、转动的圆环、滚动的色块）每帧都在动；
  - 与 Chrome 155 逐帧对照三例各 4 帧（初始、背景移动后、拖动后、增减后）：窗口顺序与位置相同，整帧平均差 < 1 级，窗口空白玻璃平均差 0.6–1.0 级、最大 11 级；
  - 4K（3840×2160，RTX 5090，Release）32 个毛玻璃窗口：GPU p50 0.94 ms（D3D11）/ 1.32 ms（Vulkan），p99 1.6 / 1.9 ms；渲染 CPU p50 2.4 / 2.9 ms；owner 约 2 ms。半透明本身在 GPU 上便宜（比普通半透明多 0.5–0.8 ms）；CPU 随窗口里的元素数增长，三种窗口相近，已记入待办的交互性能优化；
  - 修正：Vulkan 背景遍的常量缓冲与临时图像每帧重建（32 个窗口时渲染 CPU 15 ms，现 2.9 ms）；粒子与玻璃基准的 Vulkan 计时原来开着验证层、目标不可读，已改正，11.4a/11.4c 的 Vulkan CPU 数字在计划中更正（实际约为原记录的 1/3）。
- **验证**：
  - 新测试 `scene_glass_page`：行为反例（拖动置顶、限在页面内、离开与取消结束拖动、内容不拖动、1–32 的边界、背景每帧变化）、12 帧与 Chrome 对照、D3D11 WARP 与 Vulkan 一致；
  - 窗口自测新增这一组：真实排队的拖动与点击 +，owner 审计窗口位置与数量，截图 `group-12.png`、`glass-drag.png`；`d3d11_warp_glass_page_benchmark` 在 WARP 上跑通基准；
  - 13 个变异全部被发现（页面的置顶、拖动边界、离开结束拖动、数量上下限、删最新的、散布；三种窗口的样式；scene 选项；owner 审计；Vulkan 背景常量的偏移）；
  - `ctest`：Release 143/143，Debug 143/143（新增 2 个）；产物与 11.5b 基线相比，窗口的报告与截图（新分区，75/71 例，卡片几何 300 项，静态包重建 30 次）和 surface 退休探针的提交数（221，多了 3 个 scene）按预期变化，其余只有已知按时序变化的套件；Android 零警告。
- **人工验证**：运行 `tools\gallery.bat`，选 1 打开画廊窗口，点侧栏的 “Translucent Windows”；拖动窗口的标题栏，按底部的 + / − 增减窗口，比较三种窗口。
- **限制**：有背景拷贝的帧任何变化都整幅损伤；性能未隔离测量，同一档两次运行的 owner p50 可差约 2 倍；Chrome 对照只在本机 Chrome 155 与 RTX 5090；移动端未验证。
- **下一步**：第 11 步到此完成。与用户一起看待办里的交互性能优化（含这一组的 CPU 数据）。

重构第 11 步之五（a、b）：背景拷贝与 `backdrop-filter`，与 Chrome 截图对照（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - scene 页面支持 `backdrop-filter`：`blur()` 与 8 个颜色函数（如 `saturate()`、`brightness()`），按 Chrome（Skia）的做法模糊：大半径先按面积缩小、边缘镜像，按圆角与裁剪画在元素内容之下；
  - 语义：透明度组里的背景只含组内已画的内容；元素自身有透明度或子孙也过滤背景时，它成为隔离组，背景画在组内容之下；Vulkan 宿主目标不可读时背景透明并计入原因；
  - D3D11 与 Vulkan 的 scene pass 都实现；D2D 参考光栅、线性 pass 与 C ABI 拒绝；元素成为固定定位子孙的包含块；
  - 与 Chrome 155 的截图逐像素对照 11 个用例（模糊半径、设备缩放、圆角、模糊前后的颜色函数、页面边缘、透明度、组、嵌套、毛玻璃）：玻璃之外完全相同，玻璃之内平均差 0.03–2.81 级、最大 1–12 级（毛玻璃圆角边框的抗锯齿最大 25）；
  - 对照发现宽模糊偏暗（`blur(20px)` 平均差 9.16）：逐级 2× 双线性降采样在奇数尺寸上丢失平均亮度，改为按面积缩到 Skia 选的尺寸后为 2.81。
- **验证**：
  - 新测试：两个后端对 CPU 参考（同一算法）在 9 种情形下相差不超过 2 级，Vulkan 对 D3D11 最大差 2；组与透明度的反例；不可读目标；CSS 值与诊断、显示列表位置、图层、损伤、包含块；
  - 新测试 `scene_backdrop_reference`：对 Chrome 截图按用例的上限失败；截图清单记录用例文件的 SHA-256，用例改了没重新截图时失败；`node tools/browser_backdrop_reference.mjs` 重新截图逐位相同，清单记下 GPU（RTX 5090）；
  - 32 个变异全部被发现（滤镜解析、显示列表与组、损伤、规划器的区域、缩小尺寸与 σ、三个着色器变体、合成、两个渲染器的拷贝、混合与缩小），其中 16 个 Chrome 对照测试单独也能发现；
  - `ctest`：Release 141/141，Debug 141/141（新增 3 个）；产物与 11.4c 基线相比只有已知按时序变化的套件不同（窗口截图与计数、trace 时间戳、owner 合并请求数、surface 退休数）；Android 零警告。
- **限制**：有背景的帧任何变化都整幅损伤；读背景的 UI 着色器效果未做；与 Chrome 的剩余差异来自 Skia 的缩放方式与圆角抗锯齿，没有逐位复刻。
- **下一步**：11.5c（见上）。

重构第 11 步之四（c）：画廊的 UI 粒子页面与 4K 性能，11.4 完成（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - 画廊窗口新增 “UI Particles” 一组（72 例、12 组，原生绘制 68 例）：篝火的火焰、余烬与烟，宝箱每 2.5 s 或点击时打开、金币沿曲线飞到顶栏计数，悬停符文时光环变密，全页微粒的密度可用按钮调；全部用 `-weft-particles` 与 `weft.emitParticles`；
  - 4K（3840×2160，RTX 5090，Release）：24 万个粒子 GPU p50 0.22 ms（D3D11）/ 0.20 ms（Vulkan），p99 约 0.6 ms；owner 每帧约 0.1 ms，不随粒子数增长；
  - 取舍：粒子的效果外观推迟（需要第二条参数记录与每个效果的离线粒子变体，没有页面需要），记在计划里。
- **验证**：
  - 新测试 `scene_particle_page`：流、自动与点击开箱、金币的目标、计数、悬停、密度分层；D3D11 WARP 与 Vulkan 按画廊 scene 规则一致，同一时钟两次运行像素相同；
  - 窗口自测新增这一组：真实排队的点击打开宝箱、帧里有飞行的金币、篝火有亮像素；`d3d11_warp_particle_page_benchmark` 在 WARP 上跑通基准；
  - 11 个变异全部被发现（页面选项、参考光栅判断、窗口的点击与审计、页面脚本、CSS、金币精灵）；`ctest`：Release 138/138，Debug 138/138（新增 2 个）；Android 零警告；产物：窗口截图与计数按时序变化（同一构建连跑两次也不同），新增 `group-11.png`、`particles-loot.png`，surface 退休探针的提交数随 scene 数变为 212。
- **更正**：11.4b 记录里“产物像素全部相同”有误：当时的比较只看了 alpha 通道。重新按 RGB 比较，窗口截图与上一阶段不同，但同一构建连跑两次同样不同，属于按时序变化，与该阶段无关。
- **人工验证**：运行 `tools\gallery.bat`，选 1 打开画廊窗口，点侧栏的 “UI Particles”；看篝火与烟，点宝箱看金币飞到右上角计数，悬停符文，按底部的 + 加密微粒。
- **限制**：粒子的效果外观未做；只有 scene 页面；带粒子的元素不整棵复用。
- **下一步**：11.5：背景拷贝与 `backdrop-filter: blur()`，以画廊的半透明窗口组验收。

重构第 11 步之四（b）：UI 粒子的 CSS 与 JS 挂接（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - 宿主按名字注册粒子；元素写 `-weft-particles: name` 在显示时持续发射，`--particles-*` 自定义属性覆盖速率、寿命、速度、方向、扩散、重力、尺寸与颜色；改变时旧流播完、新流开始，隐藏或移除时粒子立即消失；
  - JS `weft.emitParticles(element, name, {count, duration, target})` 在下一帧爆发，可飞向另一元素的中心或视口点（开始时取两者几何，含变换）；
  - owner 维护每个元素的发射记录，粒子死后删除，有粒子时帧持续推进；同一时钟、同样的输入得到同样的像素；
  - 上限与诊断：每帧 262144 个粒子、帧前 1024 次爆发；损伤从整幅改为粒子可达的范围。
- **验证**：
  - 新契约测试 `scene_particles_contract`：生命周期、覆盖值、隐藏、静态子树、爆发与飞行、上限、脚本 API、损伤；可达范围的性质测试中约 27 万个粒子位置全部在内；
  - 求解器给出的流与飞行（固定时钟）在 D3D11 WARP 与 Vulkan 上对 CPU 参考光栅相差不超过 3 级，Vulkan 对 D3D11 最大差 2；
  - 44 个变异全部被发现（首轮“参考帧带着旧粒子时整棵复用”存活，补反例后发现）；`ctest`：Release 136/136，Debug 136/136（新增 1 个）；Android 零警告；产物的像素与上一阶段全部相同（窗口截图有字节差、像素相同），只有按时序变化的计数，以及一个 cluster 用例的 VM 峰值内存多 600 字节（平台脚本变长）。
- **限制**：画廊还没有用粒子的页面（11.4c）；粒子的效果外观未做；只有 scene 页面。
- **下一步**：11.4c：画廊的粒子演示页与它的 4K 性能。

重构第 11 步之四（a）：UI 粒子的绘制项与渲染器（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。用户决定第 11 步继续做到 11.5，11.5 以画廊里一组叠放的半透明窗口（毛玻璃、普通半透明、只模糊背后）验收；交互性能的三项优化仍在待办。
- **结果**：
  - 无状态粒子：每个粒子由发射的已过时间、序号与种子解析得出（形状、速度与方向、重力与阻力、角度与自转、尺寸与颜色曲线、飞向目标），CPU 参考模型与着色器用同一组公式；
  - D3D11 与 Vulkan 的 scene pass 各加一种批次：一次发射一条记录、每个粒子 6 个顶点，柔和或硬边圆点、精灵图集帧；
  - 粒子随所属元素移动、变换，参与透明度与祖先裁剪；线性 pass、C ABI 与 D2D 参考光栅拒绝粒子。
- **验证**：
  - 两个后端对 CPU 参考光栅：柔和圆点、流与飞行在缩放 1、2 下相差不超过 3 级，硬边圆点在旋转、缩放、透明度与圆角裁剪下（除抗锯齿像素）同样，精灵帧、覆盖率精灵与旋转；Vulkan 对 D3D11 最大差 2；
  - 模型：随机数均匀，一天长的流时序仍精确，运动对数值积分相差约 0.001 px；
  - 4K（RTX 5090，Release）：40 万个粒子 GPU p50 0.71 ms（D3D11）/ 0.66 ms（Vulkan），p99 约 1.3 ms，CPU p50 0.03 / 0.25 ms；1 万个 0.02 ms；
  - 32 个变异全部被发现；`ctest`：Release 135/135，Debug 135/135（新增 3 个）；Android 零警告；产物与上一阶段相同（只有按时序变化的窗口与计数类产物不同）。
- **限制**：还没有 CSS 与 JS 挂接（11.4b）；顶点阶段每顶点算一次整个粒子（每粒子 6 次），按测量暂不另设计算 pass；带粒子的元素一变就整幅损伤。
- **下一步**：11.4b：发射器注册、CSS `-weft-particles`、JS `weft.emitParticles`、发射记录的生命周期与上限。

重构第 11 步之三（e）之二：嵌套提示页面进画廊窗口，11.3 完成（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - 画廊窗口去掉两例 Three.js（webgl-02/03，WebGL 不是产品目标），最后新增一组 “CRPG Tooltip”，运行嵌套提示页面：三张卡的烟雾由 UI 着色器效果绘制，悬停、嵌套浮层、按住 3 秒钉住、拖动钉住的窗口都能在窗口里操作；
  - 窗口现为 71 例、11 组：原生绘制 67 例（64 个 scene、2 例 Canvas、页面），4 例 Lottie 仍是诊断卡片。原始目录（72 例）及其清单、观察审计与 Chrome 参照不变。
- **验证**：
  - 窗口自测新增页面一组的像素 oracle 与截图；真实排队的鼠标移动悬停第一个关键词，页面在 body 上打开浮层；窗口截图里第一张卡有烟雾；每个 scene 的尺寸等于其槽位；
  - 6 个变异全部被发现；`ctest`：Release 132/132，Debug 132/132；Android 零警告；产物：窗口的分组截图因少了两张卡而变（新增 `group-10.png`、`tooltip-hover.png`），其余与上一阶段相同（只有按时序变化的窗口与计数类产物不同）。
- **人工验证**：运行 `tools\gallery.bat`，选 1 打开画廊窗口，点侧栏的 “CRPG Tooltip”；悬停关键词出浮层，移入浮层再悬停其中的关键词，停住 3 秒钉住，按标题拖动钉住的窗口。
- **限制**：Chrome 逐帧对照在 1596×800 的 scene 测试中（11.3e 之一）；窗口只查页面能画、能响应真实输入。
- **下一步**：与用户一起看性能待办（K 线悬停与散点图平移，见上方待办）。

重构第 11 步之三（e）之一：嵌套提示页面与 Chrome 逐帧对照（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - 页面有了 Chrome 参照（原版画布烟雾，CSS 动画从出现时计时）：9 个截帧覆盖悬停、嵌套浮层、钉住、拖动，以及带自身烟雾的装备浮层；
  - 字体回退按 emoji 呈现选字，与 Chrome 相同：🗡、☠ 这类默认按文字呈现的取单色字体，📜 取彩色字体，VS15/VS16 可改；`font-family` 列表里的字体仍优先；
  - scene 能以 html 为根运行整页；页面样式表带上 preflight 的 html 规则（行高 1.5）；
  - 修正样式编译：页面代码运行时拼出的类名也作为候选（Chrome 用的 gallery.css 有 `bg-amber-400`），第一张卡标题下缺的分隔线补上；
  - UI 着色器效果像 Chrome 画图像与画布那样画进对齐到设备像素的矩形（此前小数位置的浮层烟雾整体偏 0.375 px）。
- **验证**：
  - 回放在 9 个截帧上逐一对照 Chrome：每个元素的 rect（99–142 个，最大差 0 px）；绘制用的计算样式（颜色、透明度、边框、字重，0 处不同）；整帧像素（均差 ≤ 0.85，IoU ≥ 0.98）；每块烟雾只显示烟雾的区域（三张卡 1.9–4.2 万像素、均差 0.09–0.18，装备浮层 7.2 千像素、均差 0.13；与 Chrome 画布相差超过 1 级的像素至多 0.044%，阈值 1%；烟雾时钟提前一个 66 ms tick 时为 34%–57%）；
  - 新单测：emoji 呈现选字（VS15、VS16、列表优先、无该类字体时的回退）、字体列表给出的字体数、效果对齐到设备像素；
  - 16 个变异全部被发现（首轮“烟雾提前一个 tick”存活，烟雾阈值由均差改为逐像素份额后发现）；`ctest`：Release 132/132，Debug 132/132；Android 零警告；产物与上一阶段相同（彩色文字 oracle 里默认按文字呈现的 ⚔、☠ 加上 VS16 后逐字节相同；只有按时序变化的窗口与计数类产物不同）。
- **限制**：页面还没有进画廊窗口（11.3e 之二）；盒子的背景与边框仍按小数位置的覆盖率绘制，Chrome 先对齐到整像素，小数位置的 1px 边框比 Chrome 略虚（像素容差之内）。
- **下一步**：11.3e 之二：页面进画廊替换 webgl-02/03（画廊清单、计数与预览）。

重构第 11 步之三（d）之二：嵌套提示页面在 scene 中运行（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - CRPG UI 的嵌套提示页面（去掉调参面板，烟雾为 UI 着色器效果，组件本身不改）作为一个画廊组在原生 scene 中运行；
  - scene 根可以是 body，页面门户到 body 的浮层能画、能命中；页面有自己的样式表，模板字符串与数据里的 Tailwind 类名都在；
  - 修正一处层叠问题：样式表里的 `background` 简写曾盖过内联的 `background-color`。
- **验证**：
  - 新测试在原生 scene 中走完验收动作：悬停出浮层（位置与 Chrome 的规则一致：关键词右侧 8px、上移 4px）、嵌套浮层、停留 3 秒钉住、拖动窗口；原生 pass 画出三张卡的烟雾、浮层与底色；
  - 6 个变异全部被发现；`ctest`：Release 132/132，Debug 132/132；Android 零警告；画廊产物不变（只有按时序变化的窗口与计数类产物不同）。
- **限制**：页面还没有进画廊窗口，也还没有与 Chrome 逐帧对照（11.3e）。
- **下一步**：11.3e：页面进画廊替换 webgl-02/03；Chrome 参照（原版画布烟雾，从出现时计时的参考时钟）；逐帧对照烟雾与悬停、钉住、拖动。

重构第 11 步之三（d）之一：`Element.closest` 与固定盒子的裁剪检查（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：JS 支持 `Element.closest`；拖到视口外一半的固定窗口不再误报诊断（逃出的裁剪只需容纳视口内的部分）。
- **验证**：`ctest` Release 131/131、Debug 131/131；Android 零警告；变异 4 个中 3 个被发现，剩下 1 个与原实现等价。
- **另记**：参照清单锁定的是 CRLF 工作副本的哈希（`probes/gallery_reference_clock.js`），全新检出会使 `scene_fixture_integrity` 失败；已另开任务。
- **下一步**：11.3d 其余：scene 根改为 body（`#root` 之外的 portal）、页面样式表、模板字符串与数据里的 Tailwind 类名提取。

重构第 11 步之三（c）之三：scene `position: fixed`（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - scene 页面支持 `position: fixed`：相对视口定位，或相对最近的带 `transform`、`will-change: transform` 的祖先，与 Chrome 相同；CRPG 页面的悬停浮层与钉住的窗口由此可放；
  - `will-change: transform` 的盒子现在也是绝对定位后代的包含块。
- **验证**：
  - 一页 18 个元素的 client rect 与 Chrome 155 全部相同；命中与原生 pass 的像素都在固定盒子所在处；
  - 10 个变异全部被发现（首轮 1 个存活，已补用例）；
  - `ctest`：Release 131/131，Debug 131/131；Android 零警告；产物与上一阶段相同（只有按时序变化的窗口与计数类产物不同）。
- **限制**（都是诊断，不会悄悄画错）：
  - 固定盒子仍在树中的位置绘制与命中，所以它在 CSS 中逃出的裁剪必须完整容纳它；
  - 要用静态位置（某轴 inset 全为 auto）时，父元素必须是包含块；
  - 不能在没有变换祖先的 inline-block 里。
  - z-index 仍只在兄弟之间排序。
- **下一步**：11.3d 运行时：scene 根改为 body（`#root` 之外的 portal）、`Element.closest`、页面样式表、模板字符串与数据里的 Tailwind 类名提取。

重构第 11 步之三（c）之二：scene 文字样式与相关 CSS（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - scene 页面支持 `text-transform: uppercase/lowercase`（Unicode 完整映射，ß → SS 等）、`font-style: italic`（字体没有斜体时像 Chrome 一样合成倾斜）、`font-variant-numeric`（`tabular-nums` 等，作为 OpenType 特性）；
  - 支持 `vw`/`vh`/`vmin`/`vmax`（含 `dvh` 等形式），视口变化时跟随；`overflow-x`/`overflow-y`；`-webkit-user-select`（无效果）；
  - 斜体文字的图像四周多留空间，倾出盒子的笔画不再被裁掉（真斜体字面此前也会被裁）。
- **验证**：
  - Chrome 行几何新增 6 个用例（大写、斜体、合成倾斜、数字变体），29 个用例全部与 Chrome 相同（最大差 0 px）；
  - 合成倾斜的斜率：Chrome −0.266、参考光栅 −0.254、原生 −0.248 px/行；
  - 20 个变异全部被发现；
  - `ctest`：Release 130/130，Debug 130/130；Android 零警告；产物与上一阶段相同（只有按时序变化的窗口与计数类产物不同）。
- **限制**：`capitalize` 等其他 `text-transform` 值是诊断；没有按 `lang` 的大小写规则；关键帧里的视口长度是诊断；scene 只能两轴一起裁剪。
- **下一步**：11.3c 最后一项 `position: fixed`。

重构第 11 步之三（c）之一：scene 点线与虚线边框（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - scene 页面可以画 `dotted`、`dashed` 边框：点与段的位置、长度、两端补点都按 Chrome 的画法，在设备像素上排列。CRPG 页面关键词下的 `border-b border-dotted` 由此可画。
  - 写法补齐：`border-style` 一到四个值（Tailwind 的 `border-dotted`）此前在 scene 里被静默忽略，现在展开到四边；新增 `border-top/right/bottom/left` 简写；`border` 简写缺样式时为 none（此前当作 solid，与浏览器不同）。
  - 原生 scene pass 中直角、未旋转的盒子改按像素面积算覆盖率：整像素上的小盒子不再发虚（1 像素的点此前只有 75% 覆盖、向四邻渗出）；离格带边框盒子外缘一圈像素此前取背景色，现取边框色。
- **验证**：
  - 17 个用例在缩放 1、2 下与 Chrome 155 截图逐像素对照（参考光栅与 D3D11 原生共 68 次）：方点与虚线完全相同；圆点墨量差 3% 以内，48 个点的质心与 Chrome 都差不到 0.05 px；119 条边的逐像素图案与 Chrome 相同；
  - 离格带边框盒子每个像素与按面积的精确值相差不超过 1 级；
  - 19 个变异全部被发现（首轮 2 个存活，已补用例）；
  - `ctest`：Release 129/129，Debug 129/129；Android 零警告；
  - 产物：原生画面变化的像素全部更接近参考光栅（D3D11 170 个、Vulkan 154 个）；画廊 cluster 变化的 220 个像素中 216 个更接近 Chrome。
- **限制**：
  - 带圆角的点线、虚线边框，以及 `double` 等其他线型是诊断；
  - 原生圆点外有约 9% 覆盖的淡晕；
  - 实线边框宽度不按设备像素取整，离格盒子不像 Chrome 那样对齐像素；
  - 旋转的带边框盒子外缘仍取背景色。原生的抗锯齿斜坡比 D2D 宽，D2D 的边缘又落在精确几何之内，改为取边框色时两者差得更多，需单独处理。
- **下一步**：11.3c 其余：`position: fixed`、`text-transform`、`font-style`、`tabular-nums`、`vh`/`vw`、`overflow-x/y`、`-webkit-user-select`。

重构第 11 步之三（b）：scene 内联排版（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - scene 页面的文字会换行；块里混排的文字、`span`、`<br>`、`inline-block` 与 svg 按浏览器规则排行，行内元素跨行时按片画背景和边框、按片命中，`getClientRects` 返回各片；
  - 支持空白折叠、`nowrap`、`text-align`（左、右、居中），`text-align`、`white-space` 改为继承，常见块元素有了 UA 的 block 显示。
- **验证**：
  - 23 个排版用例与 Chrome 的元素片段和每行文字、位置逐一相同（最大差 0 px）；
  - 21 个变异全部被发现（首轮 3 个存活，已补用例）；
  - 新增契约：布局复用与重排、按片命中与 client rects、分片边框、逐行绘制、原生 pass 与参考光栅一致、四种诊断；
  - 画廊：anime-13 与 Chrome 的差距从 0.56 降到 0.19，其余为亚像素变化。
- **限制**：其他 `white-space` 值、两端对齐、非 baseline 的 `vertical-align`、行内元素里的块与定位元素都是诊断；没有 float、bidi。
- **下一步**：11.3c，`position: fixed`、点线与虚线边框、`text-transform`、`font-style`、`tabular-nums`、`vh`、`overflow-x/y`。

重构第 11 步之三（a）：CRPG 烟雾效果（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：CRPG UI 的 `SmokeBg` 写成了 UI 着色器效果 `crpg-smoke`：原版每秒约 15 次在 CPU 上算一张 48×64 的画布，这里由像素着色器每帧直接算出，画布拉伸的双线性也在着色器里做。
- **验证**：
  - 与 CPU 上照搬的原版 tick 逐像素对照（D3D11 WARP、Vulkan RTX 5090）：8 种情形，包括时钟晚开始、宽扁 chip 的右侧渐隐与旋转、无噪声、整数与非整数倍拉伸；最大差都是 1 级；
  - 8 个变异全部被发现；`ctest`：Release 125/125，Debug 125/125；产物与 11.2 相同。
- **限制**：画廊会话还没有注册这个效果；页面接入在 11.3e。
- **下一步**：11.3b，scene 内联排版（文字换行、混合内容、内联框分片）。

重构第 11 步之二：效果的 CSS 挂接与 scene 渲染器（2026-10-13）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - scene 页面可以写 `background-image: -weft-effect(名字)`，参数来自元素的自定义属性（如 `--amount: 0.75`），按效果清单里的名字、类型和初始值取值；
  - 宿主用 `SceneEffects` 注册效果和它的纹理；未注册的名字、类型不符的参数给出 scene 诊断；
  - 读时间的效果让帧持续推进；参数变化只重绘、不重排；
  - D3D11 与 Vulkan 的 scene 渲染器都能画效果（编码空间），纹理支持覆盖率数据、不透明 sRGB 与半透明颜色。
- **验证**：
  - 效果与画法等价的盒子在两个后端上最大差 1；纹理与帧时间的期望值都在 2 级以内；Vulkan 对 D3D11 WARP 最大差 1；
  - `ctest`：Release 124/124，Debug 124/124（319.9 s）；Android 零警告；产物没有新的差异；
  - 28 个变异全部被发现；
  - 加强测试时发现并修复一个缺陷：求解器的参照帧不复制效果，带读时间效果的子树会被整体复用而停帧。
- **限制**：文档页面与 D2D 参考光栅不画效果；C ABI 仍拒绝效果项；画廊还没有注册任何效果；Android 设备上未运行。
- **下一步**：11.3，用 CRPG nested tooltip 页面验收烟雾效果，并补齐页面用到而尚未支持的能力，分小段完成（11.3a–e，见计划）。

修复图表悬停掉帧（2026-10-13）。详见 [交互剖析](gallery-interaction-profile.md#悬停扫动与-scene-图像缓存2026-10-13)。
- **问题**：用户手测时，鼠标在 D3 交互的 sunburst 和 K 线上快速扫动，画廊掉到 60–80 FPS。
- **原因**：
  - scene 预处理器的图像缓存上限（128 条、16 MiB）小于一帧要用的图像（K 线 546 张，sunburst 22.3 MiB），超出就整个清空，复用为 0。
  - 所以每帧重新光栅化全部 SVG 路径，D3D11 再为每张新建纹理。
  - 另外，画廊窗口一直开着 D3D11 调试层，绘制密集的帧多花约一半时间。
- **修复**：
  - 缓存改为按最近使用淘汰，当前帧用到的图像一律保留；上限 1024 条、32 MiB；
  - D3D11 scene pass 的逐次绘制常量改为一次写入、按偏移绑定；
  - `tools/gallery.bat` 的交互入口默认关闭 API 验证（D 键切换），自测和 CTest 照常开启。
- **结果**（4K、RTX 5090、垂直同步关闭，场景更新帧的 present P95 / P99）：
  - sunburst：17.1 / 19.5 ms → 2.5 / 3.1 ms（调试层开），关闭调试层 1.9 / 2.5 ms；
  - K 线：23.9 / 25.7 ms → 8.2 / 9.6 ms（调试层开），关闭调试层 5.8 / 6.9 ms；
  - 峰值工作集基本不变：sunburst 705 → 666 MiB，K 线 697 → 716 MiB。
- **验证**：
  - 新增缓存合同（淘汰顺序、当前帧保留、条目与字节上限）；
  - 新增预处理器反例：一帧图像多于上限时，下一帧全部复用；
  - 原生 scene 测试在 WARP 与 RTX 5090 上对比两种常量路径，逐字节相同；
  - `ctest`：Release 122/122，Debug 122/122（329.0 s）；Android 零警告；
  - 产物图像与 10.6d 基线逐字节相同；报告里的变化：render-stages 的淘汰计数 1 → 35（含义从“清空次数”改为“淘汰的图像数”）、scene-native 的帧数多 34（新增的常量路径对照）、Vulkan scene 的上传 379 → 319（复用变多）；
  - 8 个变异全部被发现。
- **仍然存在**：
  - K 线更新帧仍约 5.7 ms：SVG 属性每帧重新解析，tooltip 文字每次都重建，每张路径图像各一次绘制；
  - 散点图平移每帧约 9 ms，平移让每个圆都重新光栅化；
  - 可做：小图像进图集合批，文字按内容缓存，基本形状画成圆角盒子而不经光栅化。

重构第 11 步之一：UI 着色器效果基础（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-11-步记录2026-10-12)。
- **结果**：
  - paint IR 有了效果绘制项：一个 HLSL 像素函数，读最多 16 个 float4 参数、4 张纹理和时间，返回线性预乘颜色；
  - D3D11 与 Vulkan 的显示列表渲染器都能画效果，遵守裁剪、圆角、透明度与组合；同一效果的相邻实例合为一次绘制；
  - CMake 的 `weft_add_effect` 离线编译效果：D3D11 DXBC 与 Vulkan SPIR-V，线性与编码两种空间；编译前拒绝声明资源或不带 `static` 全局量的源文件。
- **验证**：
  - 测试效果在 D3D11（WARP、RTX 5090）与 Vulkan（RTX 5090、AMD 核显）上逐像素对 CPU 期望，不超过 1 级：盒内 uv、全部 16 个参数与时间、数据与颜色纹理、重复采样、效果之后的盒子、效果纹理不取图集；
  - 效果与等价的盒子在组、圆角裁剪、旋转与圆角下最大差 0；Vulkan 对 D3D11 WARP 最大差 1；调试层与验证层零告警；
  - 原有着色器字节码不变；
  - `ctest`：Release 121/121，Debug 121/121（328.2 s）；Android 零警告；
  - 确定性产物与 10.6d 基线逐字节相同（实时窗口套件除外）；
  - 33 个变异全部被发现。
- **限制**：
  - CSS 挂接、scene 渲染器中的效果、C ABI 的效果特性位都在 11.2；C ABI 目前拒绝效果项；
  - 效果编进宿主程序，还没有效果包与清单；参数只有个数，没有名字与类型；
  - 编码空间的变体已编译，尚无绘制路径使用；
  - Android 设备上未运行。
- **另外**：CRPG UI 的两张烟雾纹理已只读捕获到 `tests/fixtures/crpg-gallery-assets`（带 sha256 清单），供 11.3 使用。
- **下一步**：
  - 11.2：CSS `background-image: -weft-effect(name)`，自定义属性作参数，效果注册与诊断，动画效果的帧调度，scene 渲染器画效果；
  - 11.3：烟雾效果与 CRPG nested tooltip 页面验收，并补齐页面用到而尚未支持的能力（scene 文字换行、`position: fixed`、portal、点线边框、`text-transform` 等），分成小段完成。

重构第 10 步之六（f）：Vulkan 适配器支持宿主纹理（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - Vulkan 的 C ABI 可以导入宿主的 `VkImageView`（外部纹理扩展），显示列表帧与四边形帧都能采样它们，与 D3D11 适配器的能力对齐；
  - 格式：线性 RGBA8、RGBA16F，以及设备能线性过滤时的 RGBA32F；
  - 支持行从底部开始的纹理；租约随帧完成退休。
- **验证**：
  - 新合同在 RTX 5090 与 AMD 核显上通过：每个像素与宿主写入的纹素相差不超过 1 级，拒绝路径与租约退休都覆盖，同步验证零告警；
  - `ctest` Release 114/114，Debug 114/114（240.9 s）；Android 零警告；
  - 8 个变异全部被发现。
- **风险**：渲染器把半透明 CPU 资产上传成 32 位浮点纹理并线性过滤，部分移动 GPU 不支持；Android 运行前要改。
- **下一步**：
  - 第 11 步：UI 着色器效果、粒子与背景拷贝，设计见 [架构](architecture.md)，从 11.1（效果基础）开始；
  - Vulkan 版的 4K gallery 窗口顺延；
- **范围调整（用户 2026-10-12 决定）**：WebGL/WebGL2 与两例 Three.js 移出需求，3D 交给宿主引擎、经外部纹理合成；烟雾等小型着色器效果改为在 UI 内做，作为新目标设计。见 [决策](decisions.md)。
- **待办（用户 2026-10-12 决定暂缓）**：Android 示例应用与模拟器、真机运行。需要下载 Android SDK 命令行工具、模拟器与系统镜像；NDK r30 已安装，交叉编译已验证。

重构第 10 步之六（e）：Vulkan 小图像共享设备内存块（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - 不超过 2 MiB 的 Vulkan 图像放进 16 MiB 的共享内存块，不再每张单独分配和释放设备内存；
  - Vulkan scene 窗口自测（关闭验证层）的其余帧 P99 从 22.7 ms 降到 1.10 ms，最慢帧从 25.3 ms 降到 1.37 ms，设备内存分配从 807 次降到 27 次。
- **另外修复**：gallery 预览自测的像素 oracle 偶发失败。
  - 原因：oracle 对核心 scene 用 D2D 参考光栅，而 GPU 画的是原生 pass，两种光栅器的抗锯齿边缘相差超过容差；d3-04 的内容每次运行都不同，网格点偶尔落在边缘上；
  - 修复：oracle 改为对所有可见 scene 采样 GPU 画出的像素；
  - 验证：修复后并行压力运行 60 次无失败，修复前 6 次中有 1 次失败。
- **验证**：
  - 新测试：区间堆的确定性与随机合同；设备上 500 张随机图像各自保持自己的像素；
  - Vulkan scene 输出与 10.6d 逐字节相同；
  - `ctest`：Release 113/113，Debug 113/113（242.8 s）；Android 零警告；
  - 10 个变异全部被发现。
- **限制**：
  - 缓冲仍然每个单独分配（槽位与渲染器已复用它们）；
  - 共享块的大小与上限是固定常量，没有按设备内存预算调整；
  - Android 设备上未运行。
- **下一步**：
  - 让宿主可以固定 `Math.random` 的种子，使 gallery 的自测内容可重复；
  - Android 示例应用需要安装 Android SDK 与模拟器，等用户确认。

重构第 10 步之六（d）：Vulkan 的 scene 窗口与槽位复用（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - 新的 Vulkan 窗口用交换链实时显示 gallery 的 26 CSS + 8 D3 scene：原生 Vulkan scene pass 记录进窗口的命令缓冲，两帧在途；
  - 操作：点击、空格或方向键切换，Esc 关闭；入口是 `tools/gallery.bat` 第 7 项（第 8 项是自测）；
  - 槽位在之后的记录中复用自己的缓冲、暂存图像与描述符池。
- **验证**：
  - 窗口自测：34 个 scene 各显示 12 帧，每个 scene 的最后一帧从交换链读回，与 `draw()` 逐字节相同；验证层（含同步验证）零告警（RTX 5090）；
  - 合同新增：每个槽位第一次记录之后不再新建对象；
  - Vulkan scene 的产物在 AMD 核显上与上一段逐字节相同，RTX 5090 上的统计不变；
  - `ctest`：Release 110/111（`d3d11_warp_gallery_preview` 偶发失败，单独重跑通过，见观察），Debug 111/111（246.0 s）；Android 零警告；
  - 4 个变异全部被发现。
- **观察**：
  - 窗口自测中 D3 的 d3-04 至 d3-06 有 17–24 ms 的慢帧，时间都花在 `encode` 里：
    - 回收过期的缓存图像时，每张图像单独释放设备内存，约 0.18 ms 一张，一次回收一百多张；
    - 新建图像约 0.06 ms 一张；
    - D3D11 驱动自己做子分配，没有这个问题。
  - `d3d11_warp_gallery_preview` 的像素 oracle 在全量并行运行中第二次失败：
    - 失败点在 d3-04 一根柱子的圆角边缘；
    - d3-04 的数据来自 `Math.random`，QuickJS-ng 以当前时间做种子，每次运行都不同；
    - 偏差原因尚未查清。
- **下一步**：
  - Vulkan 设备内存子分配（小图像共享内存块），消除这些慢帧；
  - 查清 gallery 预览 oracle 的偶发失败；
  - Android 示例应用需要安装 Android SDK 与模拟器，等用户确认。

重构第 10 步之六（c）：Vulkan scene pass 接入宿主提交（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - Vulkan scene pass 可以记录进宿主的命令缓冲，用槽位管理多帧在途；
  - 宿主报告提交与完成，图像只在使用它的帧都完成后回收；
  - 采样尚未提交的上传会得到 `Busy`。
- **验证**：
  - 新合同：三帧在途时的结果与同步绘制逐字节相同，并覆盖 `Busy` 与丢弃，RTX 5090 与 AMD 核显都通过，同步验证零告警；
  - `ctest` Release 110/110、Debug 110/110（240.7 s），Android 零警告；
  - 4 个变异全部被发现。
- **限制**：
  - 还没有在窗口（交换链）中实时显示 Vulkan 的 scene；
  - Android 设备上未运行。
- **下一步**：Vulkan 的 gallery 窗口，用交换链实时显示 scene，供人工查看；Android 示例应用需要安装 Android SDK 与模拟器，等用户确认。

重构第 10 步之六（b）：Vulkan 的 scene pass（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - scene 的原生 pass 有了 Vulkan 实现：复用可移植准备层，编码 SPIR-V 着色器，平面、图层、组、文字、阴影、SVG 路径齐全；
  - gallery 的 26 CSS + 8 D3 在 RTX 5090 与 AMD 核显上都通过 102 张 Chromium 参考；
  - 编码 pass 改用 fine 导数，Vulkan 与 D3D11 的差异大幅缩小，D3D11 产物与浏览器的距离基本不变。
- **验证**：
  - `ctest` Release 109/109、Debug 109/109（240.1 s），Android 零警告；
  - 与 D3D11 WARP 逐像素比较：RTX 5090 超过 2 级的像素 162 个，最多 19 级（硬渐变停止点）；AMD 核显只有 1 个像素，最多 3 级；
  - 验证层（含同步验证）零告警；7 个变异全部被发现。
- **限制**：
  - Vulkan scene pass 目前是同步的（自己提交、等待、读回），还没有接入宿主命令缓冲和完成 serial；
  - Android 设备上未运行；
  - 字形是 PNG/SVG 图像的文字在 Vulkan 上没有参考光栅，计为回退。
- **下一步**：Vulkan scene pass 接入宿主提交（异步、两帧在途、按完成 serial 退休），之后是 Android 的示例应用（需要安装 Android SDK 与模拟器，等用户确认）。

重构第 10 步之六（a）：原生 scene pass 的可移植准备层（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - 新库 `weft::scene_native` 承担原生 scene pass 中与图形 API 无关的一半：显示列表、规划、按内容缓存的文字/阴影/路径图像，以及平面与图层的嵌套 pass；
  - D3D11 渲染器只剩 GPU 部分，按图像 id 保存纹理。
- **验证**：
  - `ctest` Release 108/108、Debug 108/108（240.0 s），Android 零警告；
  - 全部确定性产物与上一段逐字节相同；
  - 新增准备层合同；3 个变异全部被发现。
- **观察**：一次全量运行中 4K 实时预览失败，没有留下信息，此后六次运行都未复现；之后全量运行都会输出失败信息。
- **下一步**：Vulkan 的 scene pass，复用这一准备层。

重构第 10 步之五（j）：可移植的彩色字形（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - COLRv1/COLRv0 彩色字形（Segoe UI Emoji 等）由 HarfBuzz 的 paint 接口加新的软件画布绘制，包括渐变与合成模式，不再依赖 DirectWrite；
  - 原生路径在所有测试中都不再使用 D2D 光栅。
- **验证**：
  - `ctest` Release 107/107、Debug 107/107（239.9 s），Android 零警告；
  - 与 DirectWrite 对照，文字图像的均差在 1.6 以内，GPU 帧均差 0.44；
  - cluster 含 emoji 的案例与浏览器的 IoU 升高；
  - 12 个变异全部被发现。
- **限制**：
  - PNG/SVG 图像字形（CBDT、sbix、SVG 表，如部分 Android 的 Noto Color Emoji）还画不了，只有这种文字仍走 D2D；
  - SVG 文字中的彩色字形仍回退。
- **下一步**：Vulkan 的 scene pass（编码空间），把规划与图像准备从 D3D11 渲染器中抽成可移植部分。

重构第 10 步之五（i）：显式图层原生绘制（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - 显式图层的图像由原生嵌套 pass 绘制，替代 D2D 参考光栅；原生路径中只剩彩色字形文字依赖 D2D；
  - 图层里的文字改用与其他原生文字相同的 FreeType 覆盖率，含图层的画面与浏览器参考的均差略升（css-09 0.168 → 0.180），仍远低于门限。
- **验证**：
  - `ctest` Release 106/106、Debug 106/106（305.6 s），Android 零警告；
  - 新增图层对照：图层与其内容内联绘制逐字节相同；
  - 5 个变异全部被发现。
- **限制**：
  - 彩色字形（emoji）仍经 D2D 参考光栅；
  - 图层内的 SVG、投影平面与溢出内容仍整帧回退。
- **下一步**：彩色字形改用 FreeType，之后原生路径不再依赖 D2D；然后做 Vulkan 的 scene pass。

重构第 10 步之五（h）：投影平面（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - perspective/rotateY 平面由 D3D11 原生绘制：嵌套 pass 先把平面内容画进它自己的图像，外层再经单应性投影采样；
  - paint IR 增加嵌套列表的 `Plane` 项；
  - scene 不再有回退：scene-native 121 → 0，预览 8 → 0，cluster GPU 0；
  - 翻牌卡片与浏览器参考的误差不变，均差约 0.13。
- **验证**：
  - `ctest` Release 106/106、Debug 106/106（310.4 s），Android 零警告；
  - 新增平面对照：与 D2D 参考均差 0.67，实心区域一致；
  - 新增平面 scratch 的退休检查；
  - 15 个变异全部被发现。
- **限制**：
  - 平面内的溢出内容不画（参考端相同，Chrome 会画）；
  - 平面内容每帧重画；
  - 只支持 `perspective(px) rotateY()` 子集；
  - Vulkan 还没有 scene pass。
- **下一步**：显式图层改走嵌套列表，去掉原生路径对 D2D 光栅的依赖；之后做 Vulkan 的 scene pass。

重构第 10 步之五（g）：裁剪在自身空间（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - 裁剪带上“目标像素 → 自身空间”的映射，旋转或斜切盒子的溢出裁剪、旋转下的 svg 视口与 clip-path 都原生绘制；
  - cluster GPU 的回退降为 0，预览只剩投影变换 8 次；
  - 进度环案例改为原生后与浏览器参考的误差 1.09 → 0.31。
- **验证**：
  - `ctest` Release 106/106、Debug 106/106（316.9 s），Android 零警告；
  - 恒等映射下产物与上一段逐字节相同；8 个变异全部被发现；
  - 修正了 D2D 参考把直角描边画圆的问题。
- **剩余回退**：投影变换（scene-native 121、预览 8）。
- **下一步**：投影平面（嵌套列表渲染到纹理再投影采样，同时取代显式图层对 D2D 的依赖）；之后 Vulkan 的 scene pass。

重构第 10 步之五（f）：SVG 进入原生显示列表（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - SVG 形状、文字、组、视口与矩形 clip-path 由 D3D11 原生绘制，路径经可移植矢量光栅；SVG 文字以字形轮廓绘制，旋转标签不再重采样；
  - scene-native 的回退 1089 → 121（只剩投影变换），cluster GPU 189 → 21，预览 41 → 12；
  - 顺带修正着色器量化（半级进位，与 D2D、Chromium 一致）和 D2D 参考的 SVG 斜接（改为 SVG 的 bevel 语义）。
- **验证**：
  - `ctest` Release 106/106、Debug 106/106（310.4 s），Android 交叉编译零警告；
  - 8 个 D3 场景中 6 个比原 D2D 更接近浏览器参考；cluster 的 SVG 案例除图标外都不比原来差；
  - 原生与 D2D 的 SVG 对照均差 0.40 级；12 个变异全部被发现。
- **限制**：
  - 剩余回退：投影变换（scene-native 121、预览 8）与旋转盒子的溢出裁剪（cluster 21、预览 4）；
  - SVG 只支持参考端同样支持的子集（无渐变填充、marker、pattern、mask，clip-path 只有一个矩形）；
  - Vulkan 还没有 scene（编码空间）pass。
- **下一步**：旋转盒子的溢出裁剪与投影变换，之后 Vulkan 的 scene pass。

重构第 10 步之五（e）：路径进入 paint IR，可移植的矢量光栅（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - paint IR 有了路径（三次曲线）与描边样式；
  - 新模块 `src/vector` 用 FreeType 的光栅器与描边器把填充、描边和虚线画成覆盖率遮罩，在路径自身坐标里描边，支持非均匀变换；
  - 这是原生 SVG 的基础，生产者与渲染器尚未接入。
- **验证**：
  - `ctest` Release 106/106、Debug 106/106（297.0 s），产物与上一段逐字节相同；
  - 新测试覆盖面积精度、填充规则、线帽、连接、斜接上限、虚线、边界与裁剪，并跑 1500 条随机路径与 200 个凸多边形面积差分；14 个变异全部被发现。Android 交叉编译零警告。
- **下一步**：10.5f，SVG 元素进入 scene 显示列表，D3D11 渲染器绘制路径遮罩，去掉 SVG 回退。

重构第 10 步之五（d）：原生阴影（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - 外阴影不再让 scene 回退 Direct2D：显示列表带阴影条目，阴影图由可移植的 CPU 代码生成（高斯模糊，只保留盒外部分），D2D 参考与原生共用；
  - cluster GPU 剩下的回退全是 SVG（189 次），预览 41 次（SVG 33、投影变换 8）。
- **验证**：
  - `ctest` Release 105/105、Debug 105/105（315.3 s）；D2D 改用共享代码后产物逐字节不变；
  - 原生阴影与 D2D CPU 栅格逐像素对照：3522 个阴影像素，最大差 2；11 个变异全部被发现。
- **限制**：内阴影仍报错（与 D2D 参考相同）；scene-native 仍有 SVG 968 次、投影变换 121 次整帧回退。
- **下一步**：SVG 的原生绘制（D3 是产品必需目标，也是剩余回退的大头）；之后投影变换。

重构第 10 步之五（c）：原生显示列表补齐 `#rgb` 颜色与长度渐变色标（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：这两种值 D2D 参考早已支持，原生却整帧回退；现在按参考同样的方式解析。cluster GPU 回退 231 → 210，预览 47 → 44。
- **验证**：`ctest` Release 105/105、Debug 105/105（305.6 s）；改为原生的 anime-09 比原来更接近浏览器参考；5 个变异全部被发现。
- **剩余回退**（逐项）：scene-native 为 SVG 968、投影变换 121；cluster GPU 为 SVG 189、阴影 21；预览为 SVG 33、投影变换 8、阴影 3。
- **下一步**：10.5d 原生阴影（D2D 参考与原生共用一份 CPU 阴影图）；之后投影变换与 SVG。

重构第 10 步之五（b）：溢出裁剪按 CSS 取内边距盒（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - scene 的溢出裁剪改为内边距盒与内圆角；原生显示列表、D2D 参考（逐帧与保留绘制程序）、可移植命中与参考命中四处共用一个函数；
  - 三个 scene GPU 探针的报告都逐项列出剩余回退：SVG、投影变换、颜色串、阴影。
- **验证**：
  - `ctest` Release 105/105、Debug 105/105（308.1 s）；
  - 新合同覆盖裁剪值、命中与两种 D2D 绘制模式，9 个变异全部被发现；新测试还发现并修正了 D2D 换 factory 时残留的裁剪几何缓存；
  - 打字机光标（css-19）的文字不再画到边框上，与 Chromium 一致；spring-02 更接近参考。
- **限制**：
  - 子元素圆角与父元素内圆角重合时，两层抗锯齿相乘，比 Chrome 在 css-08、css-20 上的结果暗一些（Chrome 在几何相同的 spring-02 上也相乘），在容差内；
  - 旋转或斜切盒子的溢出裁剪仍整帧回退。
- **下一步**：依次去掉阴影、颜色串、投影变换、SVG 的回退。

重构第 10 步之五（a）：scene 的圆角溢出裁剪（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：带圆角的 `overflow: hidden/auto` 不再让 scene 整帧回退 Direct2D。显示列表给内容带上圆角裁剪链，D3D11 scene 渲染器上传裁剪缓冲，由共享着色器裁剪。回退次数：gallery 预览 210 → 47，scene-native 1452 → 1089，cluster GPU 252 → 231。
- **验证**：
  - `ctest` Release 105/105、Debug 105/105（233.1 s）；
  - 新合同覆盖嵌套链、镜像与缩放的圆角、组与文字取哪条链、旋转回退；8 个变异全部被发现；
  - 与 Chromium 参考的误差基本不变。spring-02 的均差 0.153 → 0.246，全部来自最右的半像素列：原生按像素中心裁剪，与 Chrome 对齐到像素的裁剪一致。
- **限制**：
  - scene 的溢出裁剪按边框盒，CSS 要求内边距盒与内圆角，带边框时不同，原有实现与 D2D 参考也是如此，10.5b 修正；
  - 旋转或斜切盒子的溢出裁剪仍整帧回退；Vulkan 还没有 scene（编码空间）pass。
- **下一步**：10.5b 按 CSS 修正溢出裁剪；之后依次处理阴影、颜色串、投影变换、SVG 路径与彩色字形的回退。

重构第 10 步之六（a）：Android arm64 交叉编译（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - NDK r30 下 `android-arm64` preset 构建全部可移植模块、Vulkan 后端与 36 个测试可执行文件，零错误零警告；
  - Android 平台层：NDK 系统字体迭代器 + FreeType 族名组成字体目录；Vulkan 的 Android 窗口 surface。
- **验证**：配置、编译、链接通过（arm64-v8a，API 29）；Windows `ctest` Release 105/105、Debug 105/105（282.8 s）。
- **未验证**：Android 模拟器与真机上的运行、Activity 与输入、Vulkan 呈现与性能；iOS（需要 macOS）。在模拟器上运行需要另行下载 Android SDK 命令行工具、模拟器与系统镜像，尚未获批准。
- **下一步**：10.5 可移植文字与路径（scene 不再回退 Direct2D）；之后 10.6b，Android 示例应用与模拟器验证。

重构第 10 步之四（d）：Vulkan 交换链窗口与性能（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - Vulkan 设备可向窗口呈现，`vulkan::Swapchain` 管理交换链；示例宿主经 C ABI 两帧在途地画 CaseCheetah 4K 流式页面；
  - 渲染器复用完成帧的缓冲与描述符池，Vulkan（关验证层）每帧编码 P50 0.062 ms，与 D3D11 的 0.072 ms 相当；
  - `tools/cheetah-flow.bat` 打开 D3D11 或 Vulkan 窗口，或运行对比基准。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 105/105，`windows-debug --parallel 6` 105/105（290.0 s）；
  - 4K Vulkan 窗口自测：输入、滚动、缩放、像素对照（最大差 1）与退休，同步验证零告警；
  - 4K 滚轮基准（RTX 5090，120 帧，三次中位数）：Vulkan 关验证层 228.8 fps，每帧 P50/P99/最大 4.34/4.83/5.66 ms；D3D11 211.8 fps，4.54/5.30/5.90 ms；5 个变异全部被发现。
- **未验证**：Vulkan 冷启动（4K 交换链创建约 130 ms）与峰值内存（227 MiB，D3D11 110 MiB）的优化、AMD 核显上的窗口与性能、Vulkan 外部纹理、Android 与 iOS。
- **人工验证**：双击 `tools/cheetah-flow.bat`，选 2 打开 Vulkan 窗口（标题栏显示帧率与 GPU），滚轮滚动列表、点“仓”展开、拖动窗口大小；选 1 打开同一页面的 D3D11 窗口对照；选 3 运行对比基准（约 2 分钟）。
- **下一步**：10.5 可移植文字与路径；10.6 平台层与 Android（NDK）。

重构第 10 步之四（c）：可移植的纹理缓存与 Vulkan 字形 atlas（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - 缓存策略（资产身份、LRU 纹理索引与淘汰规则、atlas 放置）移到 `render/texture_cache.hpp`，D3D11 与 Vulkan 共用；
  - Vulkan 缓存渲染器把字形放进 atlas 页面，字形串一次绘制；页面的首次写入不与另一个未提交的录制重叠（返回 BUSY）。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 104/104，`windows-debug --parallel 6` 104/104（290.6 s）；产物与 10.4b 逐字节相同；
  - Vulkan atlas 与独立纹理逐像素相同；淘汰不取在途纹理，区域复用后像素正确；同步验证零告警。
- **未验证**：Vulkan 的外部纹理、交换链窗口与性能、Android 与 iOS。
- **下一步**：10.4d，Windows 上的 Vulkan 交换链窗口，跑真实工作负载并与 D3D11 对比性能；之后 10.5、10.6。

重构第 10 步之四（b）：Vulkan 的 C ABI 适配器（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - 新 payload `weft_render_vulkan_pass`：宿主交出命令缓冲与目标视图，自己提交并报告完成 serial；
  - 渲染器拆成 prepare/encode/collect，纹理与每次录制的资源按 `ResourceLifetime` 只在完成后退休，失败时什么都不录；
  - 两帧在途；目标格式 sRGB RGBA8/BGRA8 与线性 RGBA16F；验证层加开同步验证。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 104/104，`windows-debug --parallel 6` 104/104（235.5 s）；
  - RTX 5090 与 AMD Radeon 核显上宿主驱动的合同测试通过，同步验证零告警；
  - 8 个变异全部被发现，其中一个只有同步验证能发现。
- **未验证**：Vulkan 的外部纹理、纹理缓存与字形 atlas、交换链窗口与性能、Android 与 iOS。
- **下一步**：10.4c，Vulkan 的纹理缓存与字形 atlas（与 D3D11 共用可移植部分）和 Windows 交换链窗口，并测性能；之后 10.5、10.6。

重构第 10 步之四（a）：Vulkan 渲染器直接绘制显示列表（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - Vulkan-Headers 固定版本 vendored，DXC 把共享着色器编成 SPIR-V；
  - `weft::render_vulkan` 运行时加载驱动，绘制显示列表与隔离组并回读；
  - 线性变体改用细导数和按映射计算的距离边缘宽度，使后端之间一致。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 103/103，`windows-debug --parallel 6` 103/103（293.5 s）；
  - RTX 5090 与 AMD Radeon 核显上 Vulkan 与 D3D11 WARP 最多差 2 级，验证层零告警；
  - D3D11 的 SDF 夹具像素变化，见计划；4 个变异全部被发现。
- **未验证**：Vulkan 的 C ABI 适配器、两帧在途、交换链窗口、Android 与 iOS。
- **下一步**：10.4b，Vulkan 的 C ABI 适配器与宿主提交、完成合同。

重构第 10 步之三（d）：字形 atlas 与按纹理合批（2026-10-12）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：缓存渲染器把小覆盖率资产放进共享的 R8 atlas 页面，相邻的盒子与同页字形合成一次绘制；4K 文档页每帧约 23 次绘制。
- **验证**：`ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（310.2 s）；产物与 10.3c 逐字节相同；RTX 5090 硬件探针通过；6 个变异全部被发现。
- **性能**：4K 文档窗口录制 P50 0.39 → 0.13 ms，吞吐 190.8 → 198.7 帧/秒（对第 9 步）；GPU 每帧仍多约 0.05 ms。
- **下一步**：10.4 Vulkan 后端。

重构第 10 步之三（c）：文档与四边形帧都走显示列表（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - C ABI 增加不透明的显示列表帧；D3D11 渲染器只剩显示列表一条绘制路径，四边形帧在适配器里转成显示列表；
  - 逐四边形绘制、`quad.hlsl` 与 CPU 圆角遮罩删除；文档探针、窗口 HUD 与外部纹理生产者都发送显示列表。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（235.6 s）；RTX 5090 上 16 个硬件探针通过；
  - 产物：19 套逐字节不变；10 套像素变化，都在圆角边缘、SDF 抗锯齿带暗端或 1 级以内，逐套说明见计划；
  - 变异检查 12 个：首轮 9 个（组与裁剪诊断失效、底部起始纹理不翻转、组图层不清空、嵌套组取错图层、线性裁剪与图片范围失效、追加时外部纹理不后移、能力位不查）全部被发现；补充的 3 个里两个裁剪链上限被发现，“scene pass 也去掉外扩”起初未被发现，补规划器范围测试后被发现。
- **性能**：4K 文档窗口吞吐 186.4 → 178.5 帧/秒，录制 P50 +0.10 ms，GPU P50 +0.06 ms；冷启动首帧约 41 → 23 ms。
- **下一步**：10.3d 字形 atlas 与记录压缩，然后 10.4 Vulkan。Vulkan SDK 1.4.363.0 已安装（验证层、带 SPIR-V 后端的 DXC）。

重构第 10 步之三（b）：共享着色器直接绘制文档显示列表（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：共享的 scene 着色器能画图片条目、着色器内的圆角裁剪链和线性合成；D3D11 渲染器可以直接绘制文档显示列表。文档探针此时仍走四边形，像素与产物不变。
- **验证**：`ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（390.6 s）；1306 个产物逐字节相同；直接绘制与四边形降级在 WARP 上最差差 8 级，只在圆角裁剪边缘。
- **下一步**：10.3c，文档探针切换到直接绘制，退役 CPU 圆角遮罩与逐四边形绘制。

重构第 10 步之三（a）：文档版面产出 `DisplayList`（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - IR 增加图片条目、资产与圆角裁剪区间；
  - 文档版面经 `page_display_list()` 产出显示列表，再由可移植的 `lower_to_quads()` 降成原来的四边形包（CPU 圆角遮罩从探针移入 `src/render`）。像素与产物不变。
- **验证**：`ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（334.1 s）；1306 个产物逐字节相同（含全部文档路径套件）；`paint_contract` 增加降级检查。
- **下一步**：10.3b，统一渲染器直接绘制文档显示列表：着色器内的圆角裁剪链、图片条目与线性合成。

重构第 10 步之二：可移植的 scene pass 规划（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：scene pass 的实例记录与绘制步骤由可移植代码规划，D3D11 只执行；Vulkan 后端将复用同一规划。像素与产物不变。
- **验证**：`ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（227.3 s）；1306 个产物逐字节相同；`paint_contract` 增加规划检查。
- **下一步**：10.3，文档版面产出 `DisplayList`，先经可移植的四边形降级保持旧输出不变，再让统一渲染器直接绘制。

重构第 10 步之一：`src/paint` 与 scene 显示列表（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-10-步记录2026-10-11)。
- **结果**：
  - 新模块 `src/paint`：可移植的绘制 IR `DisplayList` 及其结构校验；
  - scene 改为产出 `DisplayList`，原生 D3D11 scene pass 改读它。像素与产物不变。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 102/102，`windows-debug --parallel 6` 102/102（228.3 s）；
  - 1306 个产物逐字节相同；新增 `paint_contract`。
- **下一步**：10.2，文档版面也产出 `DisplayList`，统一渲染器补齐圆角裁剪链、字形 atlas、图片与线性合成，退役逐四边形绘制与 CPU 圆角遮罩。Vulkan 头文件、SPIR-V 编译器与 Android NDK 的下载待用户确认。

继续压 owner CPU，第 4 步：SVG 形状按节点保留，几何遍历不再逐节点建表（2026-10-11）。详见 [交互性能](gallery-interaction-profile.md#重构之后继续压-owner-cpu2026-10-11第五轮)。
- **结果**：
  - 几何与命中图里，属性没变的 SVG 元素直接复用自己的形状，解析过的变换按文本复用；
  - 元素几何的包围盒按树序一次算出，不再逐节点建哈希表、重复解析变换或整份复制结果。行为与产物不变。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 101/101，`windows-debug --parallel 6` 101/101（276.4 s）；
  - 1306 个产物逐字节相同；
  - `scene_contract` 新增几何缓存随机差分（300 步：长期存活与每步新建的 `SceneGeometry` 对照元素几何和网格命中，含改值、增删属性、改标签、填充规则、变换、身份互换和换父节点）以及五角星中心随填充规则变化的检查；9 个变异全部被发现：6 个由新差分发现，测量阶段的 3 个（变换与序号）由画廊、集群测试里对照 D2D 参考几何的检查发现；
  - owner CPU（高优先级交替 5 轮）：K 线 P50 1.969 → 1.747 ms（−11%）、P99 2.865 → 2.327 ms，5 轮都更低；散点图 1.434 → 1.402 ms；anime 持平。
- **下一步**：层叠每帧全量遍历（约占 K 线 owner 的四分之一），以及静态复用判断里按子树的整份比较。

继续压 owner CPU，第 3 步：样式输入与上一帧只跟着变化走（2026-10-11）。详见 [交互性能](gallery-interaction-profile.md#重构之后继续压-owner-cpu2026-10-11第五轮)。
- **结果**：
  - 场景求解器的样式输入按 DOM 子树版本号原地维护，不再每帧整份重建；
  - 上一帧的场景节点与计算样式只复制本次没有整棵复用的部分。行为与产物不变。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 101/101，`windows-debug --parallel 6` 101/101（304.6 s）；
  - 1306 个产物逐字节相同；
  - `style_cascade_contract` 新增样式输入的随机差分（4000 步，两种 profile，对照整份读取）、版本号信任与换根检查、跳过子树的节点与深度上限；`scene_contract` 新增场景求解器随机差分（600 步，对照非增量求解和独立拥有的帧）；
  - 17 个变异全部被发现，其中 2 个（上一帧自定义属性不同步，输出不受影响）只由 Debug 自检发现；
  - owner CPU（高优先级交替 5 轮）：K 线 P50 2.367 → 1.914 ms（−19%），5 轮都更快；散点图 1.470 → 1.396 ms（−5%）；anime 持平。
- **下一步**：静态复用判断里仍有按子树的整份比较（计算样式、动画状态、绘制源），以及每帧全量的层叠遍历。

继续压 owner CPU，第 2 步：属性表按视图查找，样式键不产生临时串（2026-10-11）。详见 [交互性能](gallery-interaction-profile.md#重构之后继续压-owner-cpu2026-10-11第五轮)。
- **结果**：属性表可以直接用 `string_view` 查找；样式缓存键字节不变，但拼写时不再产生临时字符串、不再整串转小写。行为与产物不变。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 101/101，`windows-debug --parallel 6` 101/101（304.4 s）；
  - 1306 个产物逐字节相同；
  - owner CPU（高优先级交替 5 轮）：K 线 P50 2.542 → 2.353 ms（−7%），5 轮都更快；散点图与 anime 持平。
- **下一步**：样式输入键和上一帧的场景节点、计算样式拷贝按 DOM 版本号只跟着变化走。

继续压 owner CPU，第 1 步：几何与命中的只读访问（2026-10-11）。详见 [交互性能](gallery-interaction-profile.md#重构之后继续压-owner-cpu2026-10-11第五轮)。
- **结果**：形状缓存键一遍拼出；几何与命中遍历不再复制样式字符串；没有变换的元素不再解析变换。行为与产物不变。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 101/101，`windows-debug --parallel 6` 101/101（324.3 s）；
  - 1306 个产物逐字节相同；
  - owner CPU（高优先级交替 5 轮）：K 线 P50 2.927 → 2.588 ms（−12%），散点图 1.558 → 1.483 ms（−5%），两者每轮都更快；anime 持平。
- **下一步**：场景求解器里与 DOM 版本号相关的整份工作：样式输入键、上一帧场景节点与计算样式的拷贝。

重构第 9 步之五：增量快照，第 9 步完成（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-9-步记录2026-10-11)。
- **结果**：
  - 快照按子树版本号增量刷新：没变的子树原样保留，并报告内容是否真的变化；
  - 宿主几何/样式读、gallery scene 会话与场景求解器都改用增量副本，不再每次变更后整份复制、深比较和拷贝。行为与产物不变。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 101/101，`windows-debug --parallel 6` 101/101（325.7 s）；
  - 1306 个产物与改动前逐字节相同；强制 GC 下 6/6（63.7 s，日志 `artifacts/local/step9/gc-stress-9.5.log`）。
  - 原生与脚本层两组随机差分对照全量快照，18 个变异全部被发现；
  - owner CPU（交替 5 轮，每轮都更快）：K 线 P50 4.003 → 3.209 ms（9.5a）；9.5b 再到 2.915 ms，合计 −27%；散点图 −7%，其余持平。
- **下一步**：第 9 步完成，计划中只剩暂缓的第 10 步。

重构第 9 步之四（d）：原生 DOM 树移入 `src/dom`（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-9-步记录2026-10-11)。
- **结果**：
  - 节点、树边、插入与移除的校验、变更日志、快照与选择器查询都在 `dom::Tree`，可以不经脚本引擎使用和测试；
  - 脚本层只剩包装对象、脚本状态和 GC 所有权：每条树边在邻居包装上持有一个引用，节点存活仍由 QuickJS 收集器决定。行为不变。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 101/101，`windows-debug --parallel 6` 101/101（336.1 s）；
  - 1306 个产物与改动前逐字节相同；强制 GC 下 6/6；
  - `dom_contract` 新增 6 组不依赖脚本引擎的树测试，11 个变异全部被发现；
  - owner CPU：与 9.4b 交替 5 轮持平（K 线 P50 3.978 → 3.960 ms），与 9.3 时安静条件下的数值一致。
- **下一步**：9.5，增量快照。第 9 步的 DOM 与脚本分离到此完成。

重构第 9 步之四（c）：slot 表改用空闲堆（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-9-步记录2026-10-11)。
- **结果**：新建节点（以及视图、渲染资源）不再从头扫描 slot 表，仍取最低的可用 slot，行为与产物不变。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 101/101，`windows-debug --parallel 6` 101/101（347.2 s）；
  - 1306 个产物与改动前逐字节相同；
  - base 测试新增 20 万步随机差分，5 个变异全部被发现；
  - 基准：每次“回收一个、新建一个”，2048 个 slot 时 311 → 16 ns，32768 个时 3.48 µs → 19 ns。
- **下一步**：9.4d，原生节点与树操作移入 `src/dom`。

重构第 9 步之四（b）：节点上不再有脚本状态（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-9-步记录2026-10-11)。
- **结果**：包装对象、expando、事件监听器和滚动事件标记从节点移到按 slot 索引的绑定表，节点只剩 DOM 数据。行为不变。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 101/101，`windows-debug --parallel 6` 101/101（323.1 s）；
  - 1306 个产物与改动前逐字节相同；强制 GC 下 DOM、rAF、scene、Canvas、React 5/5；
  - 生命周期测试新增 slot 复用与“监听器里新建节点”两个反例，3 个变异全部被发现；
  - owner CPU：测不出变化。本机负载重，三组高优先级交替对照方向互相矛盾（详见重构计划）；对照工具新增 `--high-priority`。
- **下一步**：9.4c，slot 表改用空闲堆，新建节点不再线性扫描；然后 9.4d，原生节点与树操作移入 `src/dom`。

重构第 9 步之四（a）：DOM 树边改用 NodeId（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-9-步记录2026-10-11)。
- **结果**：节点的 parent、document、children 不再是 JSValue，而是 NodeIdentity。每条边仍在邻居的包装对象上持有一个引用并由 `mark()` 报告，节点存活仍由 QuickJS 收集器决定。行为不变。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 101/101，`windows-debug --parallel 6` 101/101（336.4 s）；
  - 1306 个产物与改动前逐字节相同；强制 GC 下 DOM、rAF、scene、Canvas、React 5/5；
  - 6 个所有权变异中 5 个被发现，剩下 1 个不可观测（理由见重构计划）；
  - owner CPU 持平：K 线复测 5 轮，后台安静的两轮两者都是 3.98–4.01 ms。
- **下一步**：9.4b，把节点上的脚本状态移到绑定层。

重构第 9 步之三：Canvas 状态移到节点旁表，一份 canvas 尺寸解析（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-9-步记录2026-10-11)。
- **结果**：
  - 节点不再持有 Canvas 状态。CanvasState 与 context 包装放在按 slot 与代际索引的 `dom::NodeTable`，由元素的 mark/finalizer 负责这条边与释放；
  - 脚本 DOM 与文档布局共用 `dom::canvas_dimension`；`-0` 现在按 HTML 规则得到 0；
  - getContext 的 options 脚本若已创建 context，现在返回那一个，修掉了原来的引用泄漏。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 101/101，`windows-debug --parallel 6` 101/101（323.8 s）；
  - 新增 `dom_contract`，`canvas_2d_contract` 增加 3 个反例；9 个变异全部被发现；
  - 1306 个产物与改动前逐字节相同；
  - 强制 GC 配置（`build/windows-gc-stress`，每次分配前完整 GC）下 canvas_2d_contract、script_dom_lifecycle、animation_frame_contract、page_react_contract、dom_contract 5/5（53.0 s，日志 `artifacts/local/step9/gc-stress-9.3.log`）；
  - owner CPU：与 9.2 结束时交替 3 轮持平（K 线 P50/P99 4.131/6.890 → 4.038/6.798 ms，散点图 1.669/2.781 → 1.666/2.949 ms）。
- **下一步**：9.4，节点数据改为原生、边用 NodeId，QuickJS 对象只做包装。

重构第 9 步之一、之二：DOM 值类型独立，一个选择器引擎（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-9-步记录2026-10-11)。
- **结果**：
  - 节点身份、快照与几何值移入 `src/dom`，style、layout、scene 不再经 script 的头文件取得它们；
  - DOM 查询与样式层叠共用 `dom/selector`。DOM 新增 `>`、属性运算符、转义与 `:root`/`:hover`/`:active`，样式新增 `~= |= ^= $= *=`；
  - 名称大小写按 HTML 规范：HTML 元素用转成小写的选择器名比较，SVG 按原样，样式中的 SVG 驼峰名（`linearGradient`、`[viewBox]`）现在能匹配；
  - DOM 查询遇到合法但未支持的选择器报 `WEFT_UNSUPPORTED_SELECTOR`，语法错误报 `WEFT_SELECTOR_SYNTAX`；
  - 以数字开头的未转义标识符（如 `[x=1]`、`.1a`）与浏览器一样被拒绝。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 100/100，`windows-debug --parallel 6` 100/100（278.3 s）；
  - 新增 `selector_contract`，20 个变异全部被它发现；
  - 1306 个产物与改动前逐字节相同；
  - owner CPU：与第 8 步结束时交替 4 轮，K 线 P50/P99 4.556/6.995 → 3.991/6.772 ms，散点图 1.981/3.289 → 1.704/2.571 ms，每轮 P50 都更快；anime、framer、spring 持平。
- **下一步**：9.3，Canvas 状态移到按 NodeId 索引的旁表。

重构第 8 步完成（2026-10-11）。8.1–8.3b 见下方各条；收尾两件事：
- **scene 命中按圆角**：盒子区域和圆角 overflow 裁剪都按画出来的圆角判定，与文档一致，D2D oracle 同步更新。`scene_contract` 增加反例。`ctest --preset windows-release --parallel 6` 99/99，`windows-debug --parallel 6` 99/99（309.9 s）；owner CPU 无回退（K 线 6 轮 P50/P99 4.292/6.928 → 4.254/6.671 ms）。
- **paint IR 推迟**：按你的决定，一份 paint IR 与统一渲染器一起做，放在第 10 步渲染工作的开头。两个渲染器的文字、图层与裁剪模型不同，现在只合并类型没有收益。详见 [重构计划](refactoring-plan.md#第-8-步记录2026-10-11)。

下一步：第 9 步，DOM 与脚本分离（NodeId 原生树、与 style 共用选择器、QuickJS 对象成为包装、Canvas 状态移到旁表）。

重构第 8 步之四：文档与 scene 共用一条命中路径（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-8-步记录2026-10-11)。
- **结果**：
  - 文档与 scene 都把绘制过的区域按顺序列成 `HitAreas`（形状、逆平面、裁剪链），由同一个函数判定；
  - scene 的 fill/stroke 包含判断移入共享模块；
  - 命中结果与改动前完全相同；
  - scene 命中现在与绘制一样把 `z-index:auto` 当作 0，以前直接报错。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 99/99，`windows-debug --parallel 6` 99/99（323.9 s；比对辅助函数每帧只建一次命中表，scene gallery、cluster 测试 120、408 s → 82、324 s）；
  - 新增 `hit_test_contract`，8 个变异全部被它发现；
  - 产物差异集合与 8.3a 相同；owner CPU 持平。
- **未解决**：scene 命中仍忽略 border-radius。

重构第 8 步之三：scene 只有一个 transform 模型（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-8-步记录2026-10-11)。
- **结果**：几何与命中、原生绘制、D2D 参考光栅共用一个 transform 文法与求值器，原来的两套文法、一套透视文法与四处矩阵合成删除。顺带修正 SVG 无单位角度被当作弧度的错误：d3_interactive-00 与 Chrome 的平均通道差 0.32 → 0.075。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 98/98，`windows-debug --parallel 6` 98/98（408.7 s；scene cluster 测试 375 → 408 s，Debug 下逐节点解析 transform 的开销）；
  - 新增 `scene_transform_contract`，7 个旧行为变异全部被它发现；
  - 26 组产物逐字节不变；其余差异只有上述 D3 修正，以及原生矩阵改用 double 后最多 1 个通道值的像素差。
  - owner CPU 与 8.2b 交替 4 轮无回退：K 线 P50/P99 4.957/8.651 → 4.868/8.289 ms，散点图 1.977/3.323 → 2.008/3.366 ms，anime、framer、spring 持平。

重构第 8 步之二：文档与 scene 共用一个 Yoga 适配器（2026-10-11）。详见 [重构计划](refactoring-plan.md#第-8-步记录2026-10-11)。
- **结果**：scene 删掉自己的 Yoga 代码，改走共享的 `layout_boxes`，并改用与文档相同的 CSS 语义（flex item 默认收缩并有自动最小尺寸、block 子项不 flex 等）。34 个 gallery 场景和 30 个 cluster 场景的像素与 Chrome 几何对照逐字节不变。
- **关键改动**：
  - 适配器输入类型化，并借用格式化器的样式，`string_map()` 删除；文字测量由格式化器经 `FunctionRef` 提供；
  - grid、svg 比例、行内单行近似、transform 包含块移入共享适配器；
  - 新增 `box_layout_contract`，7 个旧语义变异全部被它发现。
- **验证**：
  - `ctest --preset windows-release --parallel 6` 97/97，`windows-debug --parallel 6` 97/97（375.8 s）；
  - 29 组确定性套件产物逐字节不变；
  - owner CPU 与 8.2a 交替 6 轮，无回退（K 线 P50/P99 4.135/5.896 → 4.121/5.481 ms）；
  - Debug 下 scene gallery、cluster 测试由 89、311 s 变为 121、375 s：共享适配器每次布局都逐属性校验并计算固有尺寸，未优化构建中开销明显。Release 下两者不变（6.56、27.92 s → 6.58、27.77 s）。
- **未解决**：自动最小尺寸仍是估计（百分比指定尺寸不参与上限）；scene 文字不换行，行内子项按单行近似；适配器的错误尚未迁入 `Diag`。

重构第 8 步之一：统一的文档格式化器（2026-10-11）。page、positioned、auto 三个页面 profile 已删除，页面只剩 `layout_document`（原 flow）。详见 [重构计划](refactoring-plan.md#第-8-步记录2026-10-11)。
- **结果**：七项旧浏览器对照全部在统一路径上按原阈值通过，最大差与旧 profile 相同；AlertBar 字体基线 0.4416 → 0.0896 px（阈值 0.5）。
- **关键改动**：
  - 行盒改用 Blink 的半行距规则，与 scene 共用；flow-layout fixture 的片段平均差 0.116 → 0.037 px，没有字段变远；
  - 补齐 CSS 语义：视口初始包含块、flex item margin、canvas 替换元素、匿名盒继承 white-space；
  - React 页面改走共享 packet 构建器；三个样式 profile 合并为一张表（逐项相同）。
- **验证**：`ctest --preset windows-release --parallel 6` 96/96，`windows-debug --parallel 6` 96/96（319.75 s）；除迁移的 7 组套件外产物逐字节不变。
- **未解决**：p、button 的 UA 默认边距未建模（ul 已诊断）；文档字体仍是固定探针 FontSet。

垂直同步开关（2026-10-11）。画廊窗口在 240 Hz 显示器上一直锁在 240 FPS，现在可以关掉垂直同步看实际帧率。详见 [交互性能](gallery-interaction-profile.md#关闭垂直同步2026-10-11)。
- **用法**：
  - 任一窗口模式加 `--no-vsync`；
  - 画廊窗口和 Canvas 窗口里按 F7 切换，HUD 与标题栏显示当前状态；
  - `tools\gallery.bat` 菜单里按 V 切换（作用于 1、2、3、5 项），或 `tools\gallery.bat 1 novsync`。
- **实现**：关闭时以 sync interval 0 present。系统支持时交换链带 `ALLOW_TEARING`，允许撕裂；不支持时退回普通交换链，不等待垂直同步，但合成器每次刷新只显示一帧。
- **结果**（RTX 5090、可见 4K 240 Hz，各 2 次）：
  - present 帧率：anime 拖拽 240 → 490–529，散点图拖拽 240 → 388；
  - 散点图被拖卡片更新 234 → 约 334 次/秒；
  - Canvas 页约 180–200 FPS，受渲染线程每帧约 5 ms 限制，开关前后相同。
- **验证**：`ctest --preset windows-release --parallel 6` 96/96，`windows-debug` 96/96（322.3 s），WARP 窗口测试走带 tearing 标志的交换链；29 组确定性套件 1,306 个产物逐字节不变。
- **注意**：live 基准每个渲染帧注入一次拖拽移动，关闭垂直同步时输入频率随帧率翻倍，owner 指标不能与开启时直接对比。

重构第 7 步：类型化样式（2026-10-11）。计算样式从 `map<string,string>` 改为按 `PropertyId` 排序的 `StyleValues`。每个值保留原文，并带一次解析出的 payload（关键字、带单位数字、calc、颜色）。`src/` 不再使用 `std::regex`。详见 [重构计划](refactoring-plan.md#第-7-步记录2026-10-11)。
- **结果**：
  - owner CPU（与第 7 步前的二进制交替 3 轮，固定 60 Hz，不含 GPU）：K 线 warm tick P50/P99 13.07/20.21 → 4.88/9.50 ms，散点图 3.11/5.28 → 1.93/3.52 ms；
  - 可见窗口拖拽（RTX 5090、4K 240 Hz）：被拖卡片更新 anime 87 → 126、散点图 139 → 200 次/秒；散点图输入到渲染 P50/P99 10.1/16.8 → 5.9/10.5 ms。
- **主要原因**，由新增的 owner 采样剖析定位：
  - 含 `std::map` 的 scene/DOM 节点在 vector 扩容时整棵子树被复制；
  - 值解析经 `std::stod` 对非数字文本抛异常；
  - 按名字查属性。
- **验证**：
  - 29 组确定性套件 1,306 个产物文件与改动前逐字节相同；
  - 新测试 `grammar_differential`：手写语法对照原 regex；
  - `style_values_contract`：payload 对照原解析器，并做 strtod 语法差分；
  - `scene_contract` 增加未注册关键帧属性的反例；
  - `ctest --preset windows-release --parallel 6` 96/96（44.5 s），`windows-debug --parallel 6` 96/96（445.2 s）。
- **命令**：

~~~powershell
cmake --preset windows-profile
cmake --build --preset windows-profile --target weft_d3d11_probe
& .\build\windows-profile\bin\Release\weft_d3d11_probe.exe --gallery-owner-sample artifacts/local/step7/sample kline
python tools/compare_owner_profile.py <前.exe> <后.exe> artifacts/local/step7/pre7-vs-step7 --rounds 3 kline scatter anime framer spring
~~~

- **产物**：`artifacts/local/step7/`（对照、采样、可见窗口测量）。
- **未解决**：
  - 插值仍经 12 位有效数字文本往返；
  - page/positioned/auto/flow 入口仍转回字符串 map；
  - 剩余 owner 开销主要是 DOM 快照的复制与比较，以及 scene 上一帧的整份复制（第 8、9 步）；
  - 第一批可见窗口测量中出现过两次渲染线程长帧（96/194 ms），之后第 7 步的 14 次测量未复现；
  - Android/iOS 未验证（第 10 步暂缓）。

下一步：第 8 步统一格式化路径。

输入优先的早帧（2026-10-10）。`OwnerExecutor` 新增可选的渐进 tick：owner 可在 tick 中途发布完整的早帧，并运行 tick 期间到达的输入。画廊交互窗口里，收到输入的卡片先步进并立即发布；确定性测试不变。详见 [交互性能](gallery-interaction-profile.md#输入优先的早帧2026-10-10第三轮)。
- **结果**（RTX 5090、可见 4K 240 Hz，三轮中位数）：
  - anime 拖拽：输入到渲染线程 P50/P99 12.9/24.9 → 4.3/12.9 ms；
  - 散点图拖拽：12.4/16.9 → 8.6/13.3 ms，被拖卡片更新 133 → 155 次/秒。
- **验证**：
  - `owner_executor_contract` 新增早帧与 poll 反例；
  - `ctest --preset windows-release --parallel 6` 94/94；
  - `windows-debug --parallel 6` 94/94（444.1 s）。

性能修复：画廊拖拽卡顿与 Canvas 帧率（2026-10-10，第 7 步之前）。用户反馈 anime.js 拖拽和 D3 散点图拖拽很卡，Canvas 页只有 60–70 FPS。详见 [交互性能](gallery-interaction-profile.md#拖拽卡顿与-canvas-帧率2026-10-10第二轮)。
- **原因**：
  - Canvas 页：渲染器每帧在 CPU 上把两张半透明 Canvas 位图展开成 float 纹理（prepare 8.7 ms/帧）。
  - 拖拽：240 Hz 显示器上渲染线程的 shell 快照/比较/复制与卡片重绘让单帧接近刷新间隔，帧成批排队又成批阻塞，present 间隔 P99 16–28 ms。
- **改动**：
  - 半透明 sRGB 输入改由 GPU 转换（`srgb_convert.hlsl`），WARP 与 RTX 5090 上 65,536 个组合逐位等于原 CPU 结果；
  - Canvas 快照查表反预乘；
  - shell 按 subtree revision 复用布局与静态包；
  - Canvas 像素不变时复用资产；
  - 交互窗口改用 frame-latency waitable swapchain（最多 2 帧排队）；
  - 新增 live 拖拽/Canvas 基准和 render trace 区段。
- **结果**（RTX 5090、4K 240 Hz，可见窗口三轮中位数）：
  - Canvas 页 Present 64 → 229 FPS，内容更新 57 → 92 次/秒；
  - anime 拖拽 present P99 16.0 → 7.8 ms；
  - 散点图拖拽 present P99 17.2 → 11.0 ms，输入延迟 P99 33.0 → 16.9 ms。

验证：
- **全量测试**：`ctest --preset windows-release --parallel 6` 94/94（43.7 s），`windows-debug` 94/94（455.8 s）。
- **产物对照**：WARP 29 组确定性套件 1,306 个文件与改动前逐字节相同。
- **硬件**：RTX 5090 上 `--gallery-canvas-hardware`、`--scene-native-test-hardware`、`--cache-hardware`、`--complex-css-test-hardware`、`weft_render_d3d11_tests --hardware` 通过。`--gallery-window-test-hardware` 10 次中 1 次单采样点超阈值，原因见交互性能文档（墙钟动画边缘的硬件/软件 D2D 抗锯齿差异，与本轮改动无关）。

未解决：
- anime 页与 Canvas 页的内容更新率受 owner 限制（约 10–11 ms/帧，约 90–95 次/秒）；
- 渲染线程 encode 需要字形图集才能合批。

下一步：第 7 步类型化样式，同时降低 owner 每帧样式解析与原生卡片编译开销。

以下第 6 步及更早内容为历史阶段证据：

重构第 6 步：scene 可移植化（2026-10-10）。scene 的文字、几何与命中不再依赖 Windows，D2D 只作为 Windows 参考 raster。按 [重构计划](refactoring-plan.md#第-6-步记录2026-10-10)，分四部分：
- **几何与命中**：`SceneGeometry`（`src/scene/scene_geometry.*`）计算元素盒与命中，不用图形 API；D2D 重放同一份解析后的 transform 和 path。
- **文字**：
  - `SceneText` 用 HarfBuzz/FreeType 按 Blink 规则塑形与排版；
  - 字体列表是数据（`FontCatalog`），Windows 上由 `weft::platform_win32` 枚举；
  - 测试使用本机 Chrome 155 的 generic 映射。
- **原生 GPU 文字与 SDF**：
  - 原生 pass 的文字图像改由 FreeType 生成（`scene_text_image`），彩色字形与显式 layer 仍用 D2D；
  - SDF field 已在 scene 字体（含 TTC、可变字体、中文）上按 scene 布局重建验证。
- **渲染器迁移**：原探针 `SceneNativeGpu` 移入 `weft::scene_d3d11`（`render::d3d11::SceneRenderer`），shader 移到 `src/render/backends/d3d11/scene.hlsl`。
- **新测试**（共 94 项）：
  - `font_catalog_contract`：字体匹配规则，不依赖平台；
  - `scene_text_metrics` + `scene_text_browser_diff`：852 个 span 对照 Chrome；
  - `scene_text_image`：文字图像与 SDF 重建；
  - scene/cluster 测试内的几何差分（可移植实现对 D2D）。

验证：
- **全量测试**：
  - `ctest --preset windows-release --parallel 6 --output-on-failure`：**94/94（45.5 s）**；
  - `windows-debug --parallel 6`：**94/94（427.3 s）**。之后对文字图像做了一处小修改（负坐标的亚像素拆分），受影响的 16 项 Debug 测试重跑通过，Release 全量重跑 94/94（44.1 s）；
  - 日志在 `artifacts/local/refactor-step6/`。
- **Chrome 文字**：852 个 span 字体选择一致，宽度最大差 1/64 px，高度与基线完全一致。154 个合成粗体例只报告，不要求。
- **Chrome 参考图**（软掩码 IoU，第 6 步前后）：
  - scene D2D 参考：42 张变化，35 张更近、0 张更远，变化图中最小 IoU 0.757 → 0.872；
  - cluster：131 张变化，93 张更近、14 张更远（最大退 0.0017），最小 0.788 → 0.829；
  - 几何 1,476 个字段对照不变，最大 0.274975 px。
- **几何差分**（可移植对 D2D）：
  - scene：rect 最大差 8e-6 px；337,920 个命中点中 234 个边界点不同（D2D 把 0.25 DIP 内的点算作内部）；
  - cluster：7,477 个元素 rect 最大差 0.0287 px（D2D 曲线包围盒是近似值）。
- **产物对照**：D2D 参考的去重与共用定位逐字节不变；6D 迁移对 6C 产物 29 组套件、1,306 个文件逐字节相同。
- **内存**：Debug 全量测试暴露了 6B 的悬空 `string_view`（Release 下恰好读到原值），已修复。ASan Debug 下 4 个文字测试程序通过。
- **耗时**：WARP 下最重的三组原生 GPU 套件，第 6 步前后分别为 27.7/28.3/24.4 s 和 28.1/29.2/25.1 s。

未验证：
- 非 Windows 平台的构建与字体发现。可移植代码只在 MSVC 上编译过；其他平台需宿主提供 `FontCatalog`。
- 硬件 GPU 模式（本步只跑 WARP）、GC 压力配置。
- 合成粗体（font-synthesis）未实现；彩色 emoji 与显式 layer 仍需 D2D；原生 pass 没有 SDF 材质与动态 atlas。

下一步：第 7 步类型化样式（PropertyId、长度/颜色/关键字值，去掉字符串往返）。

以下第 5 步及更早内容为历史阶段证据：

重构第 5 步：D3D11 渲染器移入 `src/render`、`RenderBackend` concept、探针独立翻译单元（2026-10-10）。按 [重构计划](refactoring-plan.md#第-5-步记录2026-10-10)，分四个提交：
- **shader**：`cmake/WeftShaders.cmake` 在构建时用 Windows SDK 的 fxc 编译 HLSL 并内嵌字节码。运行时不读 `shaders/`，也不链接 d3dcompiler。Release 字节码与原运行期编译的输出逐字节相同。
- **渲染器库**：`weft::render_d3d11`（`src/render/backends/d3d11/`）包含四部分：
  - `Renderer`：设备、资源、prepare/encode/完成；
  - `Backend`：C ABI 适配器；
  - 同步宿主 `render_window`/`compose_window`；
  - `TwoFramePipeline`。
- **帧输入**：`render/frame_packet.hpp` 是与 GPU 无关的帧输入。
- **concept**：`render/backend.hpp` 定义 `RenderBackend`/`ExternalTextureBackend` concept，并由 `make_c_backend` 生成 C 函数表。异常在 ABI 处转为错误码或终止，不会跨过 ABI。
- **探针**：`probes/probe_common.hpp` 加 17 个模式 `.cpp`，不再在匿名 namespace 中途包含头文件。
- **新测试**（共 90 项）：
  - `render_backend_concept`：concept 与 thunk 合同；
  - `render_d3d11_contract`：只链接渲染器库，WARP 两帧回读、完成后退休、调试层零消息。

验证：
- **全量测试**：
  - `ctest --preset windows-release --parallel 6 --output-on-failure`：**90/90（38.2 s）**；
  - `windows-debug`：**90/90（216.8 s）**；
  - 日志在 `artifacts/local/refactor-step5/`。
- **WARP 产物对照**：工具是 `tools/compare_artifacts.py`，基线为第 5 步前的构建。
  - Release：28 组确定性套件（2,655 个文件）在每个子步骤后都逐字节相同；
  - Debug：在第 5 步结束时对照，28 组同样相同；
  - 另有 6 组实时窗口套件，同一二进制两次运行也不同，靠自身断言通过。
- **硬件对照**：在 RTX 5090 上分别用旧二进制（`36447e2` 构建）和新二进制运行 22 个 `--*-hardware` 模式，全部通过，调试层开启。
  - 21 组产物逐字节相同；
  - `gallery-preview` 的 14 个文件不同，但旧二进制自己重跑一次也有同样的差异。
  - 产物在 `artifacts/local/refactor-step5/hardware-{before,after}/`。

未验证：
- GC 压力配置的构建与运行；
- 非 Windows 平台。新代码中 D3D11 部分只在 `WIN32` 下构建，concept 头与平台无关，但未在其他平台编译过。

下一步：第 6 步 scene 可移植化。
- 拆出纯几何与命中 TU；
- DirectWrite 量字换成 HarfBuzz/FreeType；
- 随之迁出 `SceneNativeGpu`；
- 重新采集 Chrome 对照基线。

以下第 4 步及更早内容为历史阶段证据：

重构第 4 步：改名 Weft、模块化 CMake、公开/私有头分离（2026-10-10）。按 [重构计划](refactoring-plan.md#第-4-步记录2026-10-10)：
- **改名**：命名空间 `weft::`；C ABI `weft_*`/`WEFT_*`，入口头 `include/weft/weft.h`；诊断码 `WEFT_*`；可执行文件 `weft_*`；类型和文件去掉 `Probe` 前缀。被 fixture 或浏览器参考锁定的名字保持原样（`__ce*` JS 全局、`ProbeRoboto`/`ProbeCjk` 字体族、参考时钟、原样 bundle）。
- **头文件**：`include/weft/` 只放 4 个 C ABI 头；内部头移到 `src/<模块>/`，以 `"<模块>/<文件>.hpp"` 引用。
- **CMake**：根文件只剩选项和子目录；`cmake/WeftTargets.cmake` 提供 `weft_add_library`/`weft_add_executable`/`weft_add_test`/`weft_add_python_test`，第三方依赖在 `cmake/WeftThirdParty.cmake`。
- **目标**：模块库为 `weft::base`/`core`/`render`/`canvas`/`script`/`text`/`style`/`layout`/`scene`/`input`，Windows 另有 `weft::canvas_d2d`/`scene_d2d`。6 个测试可执行文件去掉 `_probe`（如 `weft_canvas_tests`），测试名不变。
- **来源记录**：`flow_input_tests.cpp` 只改了 include 行，已追加等价后继 hash。

验证：
- **测试定义**：`ctest --preset windows-release --show-only=json-v1` 与改前逐项对照，88 项命令、参数、超时、fixture、依赖、工作目录全部一致。
- **GC 压力配置**：用 Ninja 加 `-DQJS_ENABLE_GC_STRESS=ON` 分别配置改前（`fce0361` 的 worktree）与改后，86 项测试定义一致；141 个源文件的宏定义和编译开关一致，差异只有新增的 `src/` include 根，以及合并后 layout 源多出的只影响警告的 `-external:W0`。
- **全量测试**：`ctest --preset windows-release --parallel 6 --output-on-failure` 为 **88/88（68.4 s）**，`windows-debug` 为 **88/88（213.8 s）**，日志在 `artifacts/local/refactor-step4/`。
- **Release 耗时说明**：比第 3 步多出的时间全在 `gallery_runtime_integrity`/`gallery_source_integrity` 两个 Python 哈希检查（34.5/19.2 s）。原因是刚检出临时 worktree 后文件缓存是冷的，清理后单独复跑两项合计 1.1 s。

未验证：硬件窗口探针、GC 压力配置的实际构建与运行。

下一步：第 5 步，把 D3D11 渲染器、FramePacket 和两帧流水线移入 `src/render`，经 `RenderBackend` concept 接 C ABI。

以下第 3 步及更早内容为历史阶段证据：

重构第 3 步：平台 JS 内嵌（2026-10-10）。按 [重构计划](refactoring-plan.md#第-3-步记录2026-10-10)，`web_host`、`animation_frame`、`scene_platform` 三个 owner 平台模块从 `probes/` 移到 `src/script/js/`：
- **内嵌方式**：构建时由 `cmake/embed_bytes.cmake` 生成字节表编进 `caseelephant_script`（现 `weft::script`），CRLF 统一为 LF。
- **接口**：新增 `script_platform.hpp`，提供 `install_script_platform(vm, module)` 等三个函数。运行时库不再需要源码树。
- **调用点**：测试和探针中 37 处按源码路径读取改为通过该接口安装。`owner_hotpath_tests` 的优化前差分仍读 fixture。
- **新增测试**：校验内嵌文本与源文件一致，三个模块无需文件访问即可按序安装。
- **来源记录**：`flow_input_tests.cpp` 只换了安装方式，已追加等价后继 hash。

测试结果：Release **88/88（41.2 s）**，Debug **88/88（215.3 s）**，日志在 `artifacts/local/refactor-step3/`。

未验证：硬件窗口探针和 GC 压力配置未重跑。

仍保留的源码树依赖都属于测试与证据：浏览器参考时钟、fixture、字体、gallery 原样 bundle。

下一步：第 4 步，按模块拆分 CMake 并改用 `ce::` 目标，加 `weft_add_test()`，分离公开/私有头，去掉 `Probe` 前缀。

以下第 2 步及更早内容为历史阶段证据：

重构第 2 步：`tests/support`（2026-10-10）。按 [重构计划](refactoring-plan.md#第-2-步记录2026-10-10) 新增测试与探针共用的头文件库：
- **内容**：断言（`require`/`WEFT_REQUIRE`）、文件读写（保留字节/文本两种读取模式）、`json_string`、WIC PNG 读写，以及从 `probes/` 移来的 host time 格式化、帧率计与 owner trace 记录器。
- **替换的副本**：22 份测试断言函数、14 份读文件函数、3 份 JSON 转义、7 份 PNG 编解码、1 份 HRESULT 检查。
- **自包含**：图像比较头和 trace 头不再依赖包含方预先定义的辅助函数；测试也不再包含 `host_time`、`window_frame_rate`、`gallery_owner_trace` 这三个探针头。
- **来源记录**：`flow_input_tests.cpp` 只改了辅助函数接线，在 `tests/fixtures/flow_input_source_revisions.json` 追加了等价后继 hash。

测试结果：Release **88/88（41.2 s）**，Debug **88/88（215.6 s）**，日志在 `artifacts/local/refactor-step2/`。

未验证：硬件窗口探针（`--*-hardware`）未重跑；它们与 WARP 测试共用这些辅助函数。

仍存在的依赖：四个 scene 测试依赖 `probes/gallery_scene_session.hpp`，留到第 5、6 步迁出。

下一步：第 3 步，用 CMake 生成 `.inc` 内嵌 bootstrap JS，去掉运行时对 `WEFT_SOURCE_DIR` 的依赖。

以下第 1 步及更早内容为历史阶段证据：

重构第 1 步：`src/base`（2026-10-10）。按 [重构计划](refactoring-plan.md#第-1-步记录2026-10-10) 新增 `caseelephant_base`（现 `weft::base`）：
- **诊断与结果类型**：`Diag` 诊断码与 `Result<T>`；过渡用的 `DiagnosticError` 保留原 `WEFT_*` 消息文本。
- **共享 CSS 标量解析**：trim/split/number/unit/color/rect 求交，替换 10 个 profile 中的副本。
- **`GenerationalSlots`**：替换 runtime、renderer、resource lifetime 与脚本 DOM 的四套代际槽表。
- **`OwnerThread`**：统一 8 个文件的 owner 检查。
- **按错误码分派**：两处按消息前缀做控制流的代码改为按 `Diag` 分派。

C ABI 布局、句柄代际序列、异常类型和诊断文本保持不变。唯一的例外是：原先直接漏出的 `std::stof`/`std::stod` 异常，现在报告为所在 profile 的诊断。

验证：
- **单元差分**：新增 `base_contract`，用被替换实现的逐字副本在 2 万条随机拆分输入、4 万条随机数值输入上逐条比对，全部一致。
- **全量测试**：Release **88/88（41.3 s）**，Debug **88/88（210.7 s）**，日志在 `artifacts/local/refactor-step1/`。
- **性能**：与重构前 `9c9f493` 交替对照 owner CPU（`tools/compare_owner_profile.py`）。条件：RTX 5090 / Ryzen 9 9950X3D / Windows 11 26200 / MSVC Release，默认优先级与 affinity，测量期间无并发编译/测试，固定 60 Hz 时钟，每例 240 帧，16 帧预热，不含 GPU。每组 3 轮，每轮交换先后。warm tick 中位数（前 → 后，P50/P99）：K-Line 16.68/22.39 → 16.42/21.99 ms，Scatter 3.86/5.78 → 3.63/5.86 ms，Anime 14 例 0.140/0.672 → 0.143/0.730 ms。各轮差异在轮间波动范围内（候选 K-Line 有一轮 P99 26.3 ms，基线三轮为 19.4–23.1 ms），记为无可测回退，不宣称提速。原始数据在 `artifacts/local/refactor-step1/perf/`。

未验证：GC 压力、ASan 配置与硬件窗口测试未重跑；移动与主机平台状态不变。

下一步：重构计划第 2 步，抽出 `tests/support`，并把 `host_time`、`window_frame_rate`、`gallery_owner_trace` 移出探针。

以下代码规范化及更早内容为历史阶段证据：

代码规范化与命名常量（2026-10-10）：
- **格式化**：`src/`、`include/`、`tests/`、`probes/` 共 108 个 C/C++ 文件按新 [代码规范](coding-style.md) 重排，`.clang-format` 由 clang-format 22.1.3 强制执行。
  - 控制语句一律加花括号，namespace 缩进，一行一语句，运算符两侧留空格。
  - 用 clang-tidy `readability-isolate-declaration` 拆分 370 处局部多变量声明，脚本拆分 247 处成员与条件编译分支中的多变量声明。
  - 源文件统一为 `.gitattributes` 规定的 LF。
  - 代码从 14,212 行变为 38,130 行（含新增常量头），原最长一行 3,082 字符。超过 120 列的只剩嵌入的 JS/CSS 字符串字面量，按设计不改其内容。
- **命名常量**：新增 17 个常量头，共 632 个命名常量。
  - `src/constants/` 下是 `common` 加 9 个模块头，由 `all.hpp` 汇总（第 4 步前位于 `include/caseelephant/constants/`）。
  - 测试常量在 `tests/test_constants/`，探针常量在 `probes/{probe,gallery}_constants.hpp`。
  - 跨模块的同义值（DOM nodeType、码点范围、坐标/视口上限、canvas 默认尺寸、命名空间 URI、CSS 初始值等）只定义一次。
  - M0 与 scene native 两个着色器的材质编号、梯度类型、sRGB/SDF 常量与 C++ 侧互相对照命名。
- **生成器与 provenance**：`tools/generate_flex_cases.py` 改为直接输出规范布局。`flow_input_tests.cpp` 的浏览器 oracle 保留原捕获 hash，在 `tests/fixtures/flow_input_source_revisions.json` 中登记等价后继 hash，校验脚本同时接受两者。
- **重构计划**：架构重构计划另见 [refactoring-plan.md](refactoring-plan.md)，本阶段不改架构。

语义等价证据：
- **方法**：改动前后各构建 Release（无调试信息），对全部 51 个项目目标文件做 `dumpbin /disasm`，用新增的 `tools/disassembly_equivalence.py` 逐函数比较。
- **结果**：
  - 46 个逐函数指令相同（其中 32 个连原始反汇编文本都相同，其余只差 MSVC 作用域编号/COMDAT 顺序）。
  - `render_backend_tests` 只差 `#expression` 字符串化后的空格，`resource_lifetime_tests` 只差 `__LINE__`。
  - 剩下 3 个函数（`page_probe` 的 `Builder::convert`、`auto_page_probe` 的 `Builder::make`、`ScriptDom::Impl::matches_compound`）是与字符串常量比较时，内联 memcmp 改为调用 `operator==`，已逐一人工确认比较内容相同。
- **着色器**：两个 HLSL 以运行时参数 `fxc /Ges /WX /O3` 前后编译，指令相同；m0 像素着色器的 Debug `/Od` 结构不同，数学等价。

实际运行：

```powershell
cmake --build --preset windows-release --parallel 12
python tools/disassembly_equivalence.py dump build/windows-msvc artifacts/local/code-style/disasm-after
python tools/disassembly_equivalence.py compare artifacts/local/code-style/disasm-before artifacts/local/code-style/disasm-after
ctest --preset windows-release --parallel 6 --output-on-failure --output-log artifacts/local/code-style/release-ctest.log
cmake --build --preset windows-debug --parallel 12
ctest --preset windows-debug --parallel 6 --output-on-failure --output-log artifacts/local/code-style/debug-ctest.log
```

测试结果（最终源码）：Release **87/87（41.4 s）**，Debug **87/87（215.7 s）**。产物在 `artifacts/local/code-style/`：前后反汇编、比较结果、着色器汇编与改动前源码副本。

未验证：
- 本阶段没有重跑 GC 压力、ASan 配置和硬件窗口/性能测量。机器码等价覆盖了除上述 3 个函数外的全部路径。这 3 个函数只是字符串相等比较的编译方式不同，其中 `matches_compound` 反而少一条 `sete`/`test`；性能影响预期可忽略，但未重新测量。
- 移动与主机平台状态不变。

下一步：按重构计划第 1 步，抽出 `src/base` 的共享 CSS 标量解析、诊断码与代际 slot 表。

以下通用增量与 profiler 及更早内容为历史阶段证据：

通用增量与可切换 profiler（2026-10-10）：修正正常 ReleaseCapture 的误 Cancel，hover/active 改为 weak generational 旧/新路径；共享 style memo 使用精确受支持依赖、两类2048项/2MiB payload LRU。Scene 分离 canonical raw cascade/展开派生值，owner 专属 mutable update 复用不变样式与 geometry，跨线程仍发布完整独立 immutable 快照。Windows SVG shape/stroke/bounds 有界复用。检查 footer 改 native overlay，避免普通点击触发约440节点目录全量布局；原例自身组件/DOM/事件和固定库不改，没有按案例跳过工作。所有隐藏卡片统一暂停rAF、继续timer，属于检查host显式策略，66/72范围保持。

侧栏 `Profiler: OFF (F8)` / F8 可打开详细曲线与 Latest/P50/P95/P99/Max、sync reads和最重案例；默认关闭阶段 timer/GPU timestamp/trace，基本FPS与必要调度/completion保留。每条600样本、HUD≤5Hz、不额外限帧；trace每线程65,536条latest窗口，退出一次写盘，run_id防止旧文件混入。操作与全部口径见[本次合同和证据](incremental-ui-profiler.md)。

最终 Release/Debug 完整 **各87/87（67.18/246.20s）**；两配置硬件4K各160真实完成、288几何/17包对照/1680像素最大差1、两帧峰值、零debug warning/error。原生消息自动验证开启/hover/click/F8关闭，正常click不触发全目录layout或假capture Cancel。最新源 ASan Release style/Scene **2/2（26.43s）**与160帧硬件窗口通过，非ASan/GC完整重跑。差分、关闭无trace I/O、latest ring/切换、异常恢复证据与初次失败日志均保留。

最后固定owner每次操作的采样状态，避免中途开启profiler使用未启动时钟；分析器拒绝零起始时间。两普通配置追加trace/WARP window各2/2（17.59/53.37s），三个配置hardware再各160帧通过，最终记录 `toggle-*-ctest.log` / `toggle-window-{release,debug,asan}/`。完整87项是该纯采样边界补充之前的最终增量核心，边界补充以追加范围验证，不声称再全量重跑。

独立封存旧源1352份重建baseline；默认priority/affinity、相同4K/字体/bundle、固定240×60Hz、16/224 warm、trace计时开启但曲线HUD关闭，无并发编译/测试。最终25例CPU及K-Line/Anime/Scatter各三组交替、Framer/Spring单对；22个窗口各240真实完成与完整trace覆盖。K-Line隔离CPU P50/P99 22.607/39.188→15.608/22.481ms；完整窗口Owner P99 **49.46→31.93ms**，Anime12卡 **23.27→14.72**，Scatter **20.00→10.86**。warm完成吞吐分别31.29→43.93、44.74→59.46、73.12→88.55FPS，不能当实体窗口FPS或关闭profiler保证。七个轻例P50未改善，部分render/GPU尾部也不改善，全部结果与峰值WS/private/启动见 `comparison-accepted/summary.json` 和新文档。K-Line输入机会build6.39/style3.55/layout3.12ms仍偏重，远未达2ms预算；下一步缩小Scene构建/布局失效范围及降低多卡raster成本。

当前运行：`build/windows-msvc/bin/Release/weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/incremental-ui/interactive`。开启Profiler后操作，Esc退出，再 `python tools/analyze_gallery_trace.py artifacts/local/incremental-ui/interactive`。最终源/二进制/正确性/交替性能/手操副本与新seal在 `artifacts/local/incremental-ui/`，旧seal校验保留。实体输入到显示、长期游戏、公平Ultralight、移动和剩余6例仍未验收。

以下为此前阶段的历史证据；单轮数据和旧缓存/profiler策略对应当时版本：

共享Gallery簇、Sunburst退休和交互/动画优化（2026-10-10）：原样新增D3 Interactive4、K-Line1、Framer6、React Spring3、Icons2、Anime14，累计64 scene+2 Canvas=66/72绘制。共享native geometry/computed-style/CTM、事件、ResizeObserver及受限SVG/Flex/Grid已接入；原包和原组件不改，完整HTML/CSS/整库API与移动仍未实现。原始Anime Throw在Chrome/native也静止，惯性只由同固定库的有效轴变换反例证明，不能把原例标成惯性通过。实际兼容范围见[共享技术簇](gallery-shared-cluster.md)。

Sunburst连续wheel退出已复现为512MiB import预算拒绝（result8），离屏旧卡片三版本未退休。现在只按实际完成serial非阻塞回收隐藏backing，同时归还native透明组scratch预算。Release/Debug WARP与hardware均验证旧策略468,048,384 bytes拒绝、新策略206次真实submission/completion、140次透明组重建且scratch归零；完整HWND连续100次wheel到Sunburst再返回、120帧通过Chrome/crop检查。硬件两配置30例/210张图/630次完成和160帧窗口也通过，debug层warning/error=0。

针对鼠标移动/动画卡顿，新增共用最新scene solve、mutation+layout epoch同步读缓存、256项/2MiB payload的owner style memo、保守静态子树复用及连续pending Move采样合并，Down/Up/Cancel/Wheel/reset边界保留。Unicode/emoji源键去除已composite的authored transform/opacity，字体/颜色/文本/尺寸仍失效；Spring Trail paint P50/P99/max从11.592/12.982/13.267ms降到中途对照0.094/0.141/0.148ms，原例不改。新增512个style memo差分/配额反例、同步读freshness/epoch/detach/GC/reset、1000个Move夹可靠边界、Unicode复用与失效反例，旧全量参考和视觉门槛保留。

同默认priority/affinity的单例owner CPU（25例，每例240固定60Hz、16 warm/224样本、无GPU、无并发编译/测试）P50/P99：K-Line48.777/57.773→13.940/26.622ms，D3 zoom4.424/13.244→2.799/3.825ms，Anime拖拽08 1.459/7.021→0.137/1.170ms，Framer00 .240/.340→.069/.218ms。完整4K+双线程trace最终单轮warm吞吐K-Line53.45、Scatter134.14、Framer157.72、无操作Spring108.07、Anime66.13FPS；分别240次真实完成。K-Line owner P99仍26.94ms、Anime15.89ms，不能用Present FPS或单例改善宣称达到2ms预算/消除全部主观卡顿。CPU/GPU尾部、约400MiB峰值WS/约1GiB private、4.7–5s目录启动和准确可见集见[交互测量](gallery-interaction-profile.md)。

最终正常配置Release/Debug全量各86/86（235.63/524.98s），强制GC五项合同5/5（43.02s）。此前旧Debug诊断对所有SVG节点读取造成每eval超时，以及旧窗口失败/中断日志保留；最终正常全量含窗口已通过，旧窗口崩溃没有独立完整栈归因。额外30原例强制GC重放在115/210 captures时主动停止，部分JSON审计失败，不纳入通过；长时间/大量原例强制GC仍未验完。原样新30例正常配置210组state/DOM、4,408个HTML几何字段通过；原34例102 Chrome图像门槛保持。

实际运行：

```powershell
cmake --build --preset windows-release --parallel 4
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/interaction-profile/interactive
# 手操采样，Esc退出后合并owner/render共同时间轴：
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-window-profile-hardware artifacts/local/interaction-profile/manual
python tools/analyze_gallery_trace.py artifacts/local/interaction-profile/manual
# 可重复的隔离CPU/真实HWND输入采样：
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-owner-profile artifacts/local/interaction-profile/new-cpu all
& .\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --gallery-input-benchmark-hardware artifacts/local/interaction-profile/new-kline kline
```

最终产物在 `artifacts/local/interaction-profile/accepted-cpu/`、`final-trace/`、`hardware-release/`、`hardware-debug/`；完整日志为 `gallery-profile-{release,debug}-tests.log`、`gallery-profile-gc-contracts.log` 与hardware四项日志。`tools/record_gallery_interaction_evidence.py` 校验最终结果并保存来源、二进制、产物SHA及旧render-stages seal，输出本阶段 `run-metadata.json`；历史未跑完的gallery-cluster seal脚本不作为本阶段验收。优化前baseline仅有二进制/数据，无完整独立baseline源快照，限制明示。

下一步：K-Line的全量SVG/style值复制和geometry traversal、多例Anime的JS/scene求值及首帧字体源建立仍是热路径。手操异步真实input-to-display、长期运行、总VRAM、实际引擎/外部owner C ABI、公平Ultralight同负载对照、2WebGL/4Lottie与移动配置/编译/链接/模拟器/真机功能和性能仍未验证。自动输入基准等待每次publication，不能代替手操异步时延。

以下显式源图/独立owner及更早段落为历史阶段证据：

显式源图 / 原生 GPU pass / 独立 UI owner（2026-10-09）：完成后续方向 3、4、5 的内部 C++20 host adapters。owned API-neutral paint 在 D3D11 绘制 shapes/gradients/borders、缓存 glyph/source 与隔离透明组，SVG/projective/复杂 clip 完整回退 D2D；宿主明确选择复合源图，简单形状走 batched shader。D2D damage 相对实际已完成 backing 自己的 frame，未知 bounds/full crop/resize 全量；两帧实际 query/lease 退休保持。34 scene + 2 Canvas VM 在同一个 worker 创建/关闭，输入可靠、refresh 可合并、publication 完整不可变、epoch/reset/异常验证，丢弃大快照在队列锁外销毁。目录 shell 为主线程独立诊断 runtime；public C ABI threading 与实际游戏接入待做。

三交替对 4K、240/16 warm/224 samples：同 affinity 中段 158.60→197.80 FPS（+24.7%）、CPU P50/P99 5.996/8.264→4.668/6.828ms、raster 2.367→1.088；末段 203.41→228.65（+12.4%）。默认中段 122.68→147.23、末段 181.44→194.36。controlled owner 中段 1.594/2.325ms（P50/P99），默认 2.587/3.932，整页未达到2ms；末段 P99 5.970→12.847、约20ms最慢帧仍未收敛。三真实线程 hardware 队列 API P99 .013ms、27/27 输入，但不等于整套 UI 成本；renderer/owner/age/内存/启动/query口径与完整第一轮不利数据见 [记录](render-stages-performance.md)。

Release/Debug 全量各80/80、最后相关各7/7，安静 GC 10/10；两配置 hardware native scene/window，Release hardware damage/三线程 async 重跑。原34例102 Chrome图 unchanged mean/IoU阈值通过（2.32729/.798493），native2605/fallback1512/source6 build720reuse、4117实际退休；新增320帧160对照 mean .20115 / IoU1与缓存淘汰、crop、透明重叠/代际变化，窗口160帧/288几何/17包/1680像素最大差1，debug层零warning/error。GC大型原例/window未重复；实体输入、总VRAM、冷首帧尖峰、实际游戏/Ultralight公平对照、Metal/Vulkan/移动配置到真机均未验证。

当前：`build/windows-msvc/bin/Release/weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/render-stages/interactive`。4K HUD分开显示Present FPS / OWNER更新Hz。产物、663源快照与seal在 `artifacts/local/render-stages/`，650旧源独立重建baseline，旧seal保持、原101文件drift0。下一步缩减完整scene/诊断字符串、转换/producer与字体atlas成本，追踪末段等待尾部，推进实际引擎C ABI接入与移动验证。

以下 owner-hotpath 与更早内容为历史阶段证据：

Owner 热路径两项优化（2026-10-09）：34 个 gallery scene VM 直接调用 task/rAF，最早到期时间避免无任务扫描；native DOM mutation/subtree revision 和 1,024 项弱 journal 覆盖树/属性/文本/scroll，不额外保活 detached，root 未变时复用 owned snapshot。scene 编译热变换/透明度/颜色，GPU 直接消费数值，256 项 / 2MiB accounted 的静态绘制参数程序复用形状、边框、origin/clip/z 下标，递归 eviction 持有自身程序。renderer C ABI、owner/值边界、透明组/裁剪、完整包和实际两帧退休保持，未增加一般 CSS/SVG/HTML 或线程能力。

Windows RTX5090 / 9950X3D / MSVC Release、4K、240 帧/16 预热/224 样本、3 交替对：默认中段 85.63→116.21 FPS、末段 116.65→148.18；两版相同 affinity 0xFFFF / High 子进程中段 140.74→163.04、末段 178.25→208.24，每对 after 均更快。控制中段 CPU P50/P99 6.724/9.151→5.738/8.078ms，未达 P50≤2/P99≤3ms；主要收益是 JS 1.071→0.582 与 snapshot .136→.025ms，scene .963→1.004ms，raster2.315→2.269ms，不能宣称数值路径大幅减轻整页。字符串和完整 scene 仍生成；默认波动、尾部/内存/启动、GPU elapsed 口径与全部限制见 [完整记录](owner-hotpath-performance.md)。

实际 Release/Debug 全量各76/76（27.65/62.67秒），最后 quota/module/owner 边界修正后相关8/8（27.42/57.59秒），hardware complex / 原 scene GPU / window 两配置重跑。新增320 scheduler旧模块差分、512随机revision/full快照、quota/deadline/reset/weak GC/scroll/viewport，complex316图像对照（128typed+4缓存容量）hardware mean/IoU .720556/.958484、4 program evictions/316真实退休。原102图/1476字段/4117真实退休，窗口160帧/288几何/17包/1680像素差最大1保持，调试层零warning/error。GC相关8/8 46.86秒，最后边界同范围8/8 44.22秒；未重复GC大型原gallery/window，非实体输入验收。

当前运行：`build/windows-msvc/bin/Release/weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/owner-hotpath/interactive`。产物、源快照与新seal在 `artifacts/local/owner-hotpath/`，保留并校验上一阶段seal；基准旧版从642封存源在独立runtime source root重新构建，避免误读新bootstrap。原游戏/包/bundle只读不改。下一步消除剩余诊断字符串与完整scene更新、降低producer提交成本，再按游戏UI毫秒预算验收；移动、完整72实绘与公平Ultralight对照仍未验证。

以下为此前阶段的历史证据：

CSS 中段继续优化（2026-10-09）：Windows GPU border 的原 stroke/outer-mask/side-wedge 组合预编译为 filled path，按完整几何参数最多缓存 256 项；较大矩阵和软件参考保留旧 mask 路径，group opacity/overflow/绘制顺序保持整体语义。owner 在 cascade 失效时展开 shorthand，减少 target map 和布局复用时整个 previous frame 的重复复制；当前 returned scene 仍完整独立拥有。原函数/包/bundle、更新频率、两帧退休与 C ABI 不变，没有 D3 专用优化或自动图层提升。

4K / RTX5090 / Ryzen9950X3D / Release debug layer，240 帧/16 预热/224 样本，三组交替：相同 affinity 0xFFFF / High priority 子进程条件下，中段吞吐中位 **117.91 → 136.37 FPS**，末段 **153.07 → 176.64**，约 +16%/+15%；中段脉冲圆环 P50 **0.418 → 0.191 ms**。默认调度中段却为 **83.66 → 79.08**，末段 89.16 → 109.63，多个未改动阶段一起波动；不能据受控结果宣称默认整页稳定提速或确定波动原因，窗口没有强设 affinity/priority。所有样本、尾部/内存/启动、单次首屏/D3 sanity 和未测项见 [本轮完整记录](css-middle-performance.md)。

实际 Release/Debug build + CTest 各 **75/75，26.82/63.75 秒**；两配置另跑硬件复杂 CSS / 原 scene GPU / window。新增 64 帧 owner 布局参考/shorthand/transition/containing-block/viewport 与 full 值/几何/软件图对照；复杂 GPU 对照扩到184（增加96 fractional/异色/不等宽/旋转/重叠透明/越界反例），真实184提交退休，硬件 mean/IoU 0.595009/0.958484；原102图/1476字段、4117提交退休、160帧窗口/288几何/17包/1680像素与零调试 warning/error 保持。强制 GC owner/复杂 CSS/external/two-frame/cache **5/5，15.02秒**，本轮未重复 GC 大型原 gallery/geometry/window，命令和历史证据范围明确列账。

当前预览：`build/windows-msvc/bin/Release/weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/css-middle-performance/interactive`。源/9个executable/图片/两类调度条件全24个主要run/日志/原游戏drift封存 `artifacts/local/css-middle-performance/run-metadata.json`，之前 seal 校验并保留。72目录/36实绘/36诊断与平台未验证项不变；下一步定位默认调度整页波动和降低 JS/scene/producer 提交成本。

以下为前一轮复杂 CSS 与更早阶段的历史证据，当前结论以上述中段阶段为准。

复杂 CSS 优化阶段（2026-10-09）：既有显式 planar 3D 源图改由 Windows GPU perspective 采样，角度变化复用源；四边同宽同色 solid 圆环一次描边，异色/不等宽边框原路径与 256 项 wedge cache 保留；owner 缓存动画 list/每属性 track 和有界端点解析，按完整 underlying style 失效。加入失败计划重复拒绝、继承/隐式端点/动画参数/文本/移除重插对照；layout signature 比较减少 map 分配，数值输出保持 12 位有效数字。没有自动图层提升，C ABI、原游戏/包/bundle、更新频率与视觉阈值不变。

最终 4K / RTX5090 / Ryzen9950X3D / MSVC Release+debug layer、固定 240 帧/16 预热/224 样本，旧版与新版交替测 3 组：**复杂 CSS 中段中位吞吐 67.38 → 87.56 FPS（after 范围 79.00–91.44），末段 80.60 → 106.61（94.04–111.22）**，约 +30%/+32%；中段 CPU P50 的 run 中位数 14.474 → 11.243 ms，末段 11.946 → 9.115。首轮翻转单卡 1.014 → 0.169 ms。首屏首次采样下降但交替样本不一致；D3 三组中位数 185.00 → 166.36，范围重叠，尚未排除退化，未宣称稳定提速。隐藏实际完成吞吐不等于可见长时 FPS，也未与 Ultralight 同负载比较；全部分项/尾部/内存/启动/首轮与重复数据见 [复杂 CSS 实际证据](complex-css-performance.md)。

普通 Release/Debug build 与 CTest presets 实际执行，各 **75/75，26.92/65.26 秒**；两配置另硬件跑 `--complex-css-test-hardware`、`--scene-gpu-test-hardware`、`--gallery-window-test-hardware`，产物 `artifacts/local/complex-css/final/{release,debug}/`，WARP 为 CTest。新增 160 帧 owner 动态 mutation 与关闭增量的 scene 值/软件图对照；GPU 64 mutation +24 angle-only、8 target switch、88 真提交/退休和 fast/fallback 计数，硬件 mean / IoU 0.592469 / 0.964642；24 次 angle-only source upload 1 次。原102图/1476字段/120随机mutation、4117 GPU提交退休/透明滤波/滚动露出、160帧窗口/288几何/17包/1680像素继续通过，阈值不放宽，调试层零 warning/error。

强制 GC scene/geometry/原 GPU/复杂 CSS/external/two-frame/cache **8/8，446.67 秒**；追加 cache proof 后只重建 weft_d3d11_probe / 新复杂 CSS 1/1、2.90 秒。最后仅测试变量更名消除 shadow warning，三配置重建 scene_contract 并复跑各 1/1（0.38/3.21/3.48 秒），没有改测量 executable；本轮不重复 GC gallery window，完整实际命令与未测项见上述文档。最终源/九个二进制/图片/全部性能样本/日志 SHA 和原游戏 drift 在 `artifacts/local/complex-css/run-metadata.json`，旧 seal 保留。

当前预览：`build/windows-msvc/bin/Release/weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/complex-css/interactive`。完整 72 目录 / 36 实绘 / 36 诊断、4K/FPS 保持；仅验证 Windows 固定 CSS/planar 3D 范围，移动/Metal/Vulkan/device-loss/一般 CSS 3D/完整 gallery 与长时真实游戏 UI 未验证。下一步继续降低 scene/group isolation 成本，并定位轻场景吞吐的变动，不能保证所有复杂动画达到 90 FPS。

以下为首次 GPU 和此前阶段的历史证据，当前结果以上述复杂 CSS 阶段为准。

Gallery 性能阶段（2026-10-09）：**26 CSS + 8 D3 的可见 body 已接 Windows D2D GPU 绘制 → linear-premultiplied RGBA16F → 既有 external texture lease**，完整 WIC 软件参考保留。样式解析/样式与布局增量复用、文本布局/emoji/gradient/路径/圆角/3D 平面内容缓存、scratch layer 与三版本卡片池重用，消除每帧完整 CPU 图像转换/上传和活跃资产身份清空。仍单 UI owner、最多两帧在途，实际 completion 后重写/退休，完整 packet 不依赖前一包。公开 C ABI、原游戏/脚本/bundle 和原图阈值不变；72 目录、36 实绘/36 诊断、4K/FPS/输入继续保留。

最终独立 RTX5090 / Ryzen 9 9950X3D / Windows 11 26200 / MSVC Release、debug layer 开启、3840×2160、同字体/原例，240 帧含 16 预热后 224 样本，无截图回读，依次跑保存的旧 binary 与新版：**CSS 首屏 14.09 → 117.56 FPS；16 body 中段 5.22 → 55.14；D3 21.91 → 128.39**。中段 CPU P50/P95/P99/max 从 188.805/218.983/235.638/244.682 降至 17.347/21.203/23.626/31.003 ms。隐藏真实完成吞吐不能替代可见长时 FPS；未跑同负载 Ultralight，中段也未达用户报告的 90 FPS。GPU 范围差异、内存/启动、最慢帧、baseline 223/new 224 interval 公式和原始数据见 [性能条件与完整结果](gallery-performance.md)。

实际执行 `cmake --build --preset windows-release/windows-debug --parallel 8` 及对应 `ctest --preset windows-release/windows-debug --parallel 6 --output-on-failure --output-log artifacts/local/gallery-performance/final-{release,debug}-ctest.log`（每个 preset 分别运行）：**Release 74/74，32.39 秒；Debug 74/74，73.08 秒**。两配置另实际运行 `--scene-gpu-test-hardware artifacts/local/gallery-performance/final/{release,debug}/scene-gpu-hardware` 和 `--gallery-window-test-hardware artifacts/local/gallery-performance/final/{release,debug}/window-hardware`，WARP 从 CTest 执行。34 原例/102 张真实 Chrome 图仍过 mean<=3 / IoU>=0.75（硬件实际 2.39076 / 0.757239），新增半透明 filter/opacity oracle 最大差 1，裁剪/滚动露出验证、4117 提交退休、两帧 peak/零调试 warning。窗口 160 帧/288 几何/17 缓存full/1680 像素最大差 1；1476 浏览器几何与 120 随机 mutation、40 动画帧缓存/full、selector key/owner/reset/卸载反例保持通过。

强制 GC 实际重建 weft_scene_contract_tests / weft_scene_gallery_tests / weft_d3d11_probe，运行 scene/geometry/GPU original/window/external/two-frame/cache 共 8 项；首轮并行 7/8（269.16 秒），窗口加载触及原 5000ms JS 预算，单独复跑 1/1（342.68 秒）。8 项不同检查最终均有通过证据，预算/断言/超时未改，初次失败日志保留；完整实际命令在上述性能文档。源/最终三配置二进制/图片/基准/日志 SHA 与只读原游戏 drift 在 `artifacts/local/gallery-performance/run-metadata.json`，先前 seal 不覆盖。

预览：`build/windows-msvc/bin/Release/weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-performance/interactive`。本轮仅 Windows 固定范围；Android/iOS、Metal/Vulkan、device-loss、长时性能与完整 72 原例尚未验证。projective 投影、部分 gradient、color text 首次生成和 Canvas 仍有软件工作；下一步优先降低复杂 CSS 中段 scene/producer 成本，继续按真实阻断接 SVG 交互与其余案例。

以下 CSS/D3 首次渲染及更早记录为历史证据，软件性能缺口和旧未实现描述对应当时版本；本轮当前结果以上述性能阶段为准。

CSS/D3 scene 阶段（2026-10-09）：**26 CSS + 8 D3 原样案例已新增 native 渲染通过**，连同已有两个 Canvas 共 36/72。已接独立 SceneSolver/Windows D2D SOFTWARE+DirectWrite adapter，固定原 Tailwind utility 离线语义编译（123 utility / 29 原 keyframe），关键帧/transition、gradient/变换/边框/圆角/group opacity、基础 SVG path/轴/文字；native DOM descendant query、function-only interval 和 mouseover/out relatedTarget 扩展。旧 flow/auto/positioned profile 与公共 C ABI 不变，原游戏/函数/包/bundle 未修改。显式范围与诊断见 [scene gallery](scene-gallery.md)。

每个原例 121 帧，seed 0xce202610 / 60Hz / 0–2000ms；CSS 4–6 悬停/离开、CSS7 两次点击，400/1050/2000ms 三张图，共 **102 张 native PNG 对照真实 Chrome 155.0.8059.39、DPR2、完整原 CSS**。各 RGBA channel mean <=3 色阶、soft foreground IoU>=0.75；实际最差 2.33236 / 0.765045。34 个空底色反例必须全部失败。同步变换几何 **1476 字段**，阈值0.3 CSSpx，实际最大0.274975。SVG descendant 通过独立像素检查，尚不宣称 SVG geometry/hit。动画changed pixels、原React状态与卸载后保留bitmap/scene、live_nodes回到3均验证。另120轮随机mutation/full场景布局与像素差分、线程owner拒绝、隐式border终点和alternate-reverse反例通过。

实际构建：`cmake --build build/windows-msvc --config Release/Debug --parallel 8`（两配置分别执行）；最终 `ctest --test-dir build/windows-msvc -C Release/Debug --output-on-failure --output-log artifacts/local/gallery-css-d3/{release,debug}/full-ctest.log` 各 **73/73，69.62/342.14秒**。新增 scene_contract / scene_gallery_original / scene_fixture_integrity / scene_browser_geometry，250项fixture SHA校验通过。Debug/Release另执行 `weft_d3d11_probe.exe --gallery-window-test-hardware artifacts/local/gallery-css-d3/{debug,release}/hardware`，WARP由CTest执行；窗口保持160帧/72标签/288卡片几何/17完整包差分/1680像素采样，新增CSS悬停/点击和中段/末段/D3截图。实际两帧完成退休、最小化恢复和FPS更新通过，调试层零warning/error。各时间含不同验证并行，不是性能指标。

强制GC实际执行 `cmake --build build/windows-gc-stress --config Debug --parallel 8 --target weft_scene_contract_tests weft_scene_gallery_tests weft_d3d11_probe`，`ctest --test-dir build/windows-gc-stress -C Debug -R '^scene_contract$|^scene_gallery_original$|^scene_browser_geometry$|^d3d11_warp_gallery_preview$' --output-on-failure --output-log artifacts/local/gallery-css-d3/gc/final-ctest.log`，最终 **4/4，715.87秒**。完整34例/121帧/102图/1476字段与160帧WARP窗口通过，未重复72模块的隔离短观察，该项明确not_run_gc_instrumentation。初次普通240秒超时（scene在force前、随后窗口），依赖geometry缺文件而失败，日志保留 `gc/initial-timeout-ctest.log`；仅GC把两个大型测试超时设为600秒，不改JS预算/采样/断言。最后纠正“38 pending”文案为36（72减36），重建三配置，普通两配置重跑硬件窗口；GC重跑共享renderer/原Canvas窗口范围，记录 `final-wording-scope.log`，该文字改动不改变34例功能。

运行：`build/windows-msvc/bin/Release/weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-css-d3/interactive`，默认CSS，侧栏D3，4K/FPS/LIVE36，全部72目录可滚动。原其余36例仍显示缺口。34VM保留，8D3离屏继续timer/rAF，可见body才raster/upload；原两Canvas继续离屏暂停。Windows body由OS DirectWrite（本机Microsoft YaHei UI/Arial/Consolas/Emoji）塑形，跨机器字体、移动和矢量GPU路径未验证。

Release RTX5090隐藏正确性窗口末段同时显示16个CSS body：111个暖帧CPU P50/P95/P99/max为203.586/224.414/228.085/249.822ms，GPU离屏UI为4.201/5.314/5.413/5.620ms，成功Present约4.75FPS。带readback并与其他验证并行，**不能用作Ultralight性能对照**；但已明确本全量software raster/布局/RGBA32F上传路径仍有性能缺口，不套用旧flow的200FPS。gallery缓存预算512MiB（旧默认128MiB不变），该run peak缓存536685801 bytes，进程peak working set1413697536 bytes/private at close2195726336 bytes。源/二进制/产物与条件seal在 `artifacts/local/gallery-css-d3/run-metadata.json`，旧seal保留；初期字体/时钟探索输出不算最终通过。完整CSS/Grid、另4个D3 Interactive、WebGL、36待接例、实体设备与移动仍未验收。下一步分项profile并扩展GPU paint/纹理格式，按实际阻断接SVG交互。

以下为此前阶段的历史证据，旧通过数和未实现描述仅对应当时profile：

4K Gallery 检查窗口阶段（2026-10-09）：修复可见粒子/烟花的 timer/rAF 精度不一致；`std::to_string` 向上舍入导致时间倒退，现统一 classic locale / max_digits10，同一 raw double 传给两者，保留单调性验证。新增 72 例/10 组的原生 DOM/CSS 检查 shell，保留原始标签、分组顺序、列数和内容高度；侧栏、原生 wheel、案例摘要、Home/End/PageUp/PageDown、4K/FPS/CPU 分位数和两例原始 Canvas 的同页输入已接通。其他 70 例展示 JS/像素缺口，未新增原始像素通过。普通 Debug/Release 启用现有 Canvas 的隔离 12 次观察为 40 例执行未报错/32 例阻断，与之前无 Canvas 的 38/34 条件不同。

实际命令：`cmake --build --preset windows-debug/windows-release --parallel 10 --target weft_d3d11_probe weft_animation_frame_tests`（各 preset 分别运行），对应 `ctest --preset windows-debug/windows-release --output-on-failure` 全量分别 **69/69，120.17/37.95 秒**；最后仅 GC instrumentation 的观察分支调整后，普通两配置重建并复跑 animation_frame、gallery_preview、gallery_canvas_window 范围，各 3/3，记录在 `final-scope.log`。Debug/Release 均另执行 `--gallery-window-test-hardware artifacts/local/gallery-preview/{debug,release}/hardware`；WARP 为 CTest。每 run 160 帧、288 卡片几何字段、17 次缓存/全量包逐绘制/资产/UV/顺序对照、10 组首屏截图、1,680 个实际 GPU/CPU 像素采样最大差 1（容差 3）；粒子 move/烟花 click/目录 wheel、最小化恢复、FPS 已更新、峰值两帧在途和全部真实 query 退休通过，调试层零 warning/error。

强制 GC 实际重建 `build/windows-gc-stress` 的 weft_animation_frame_tests / weft_d3d11_probe，并运行 `ctest --test-dir build/windows-gc-stress -C Debug -R '^animation_frame_contract$|^d3d11_warp_gallery_canvas_window$|^d3d11_warp_gallery_preview$' --output-on-failure`，最终 **3/3，92.42 秒**。GC 下只验证 72 卡片 shell、两例 live Canvas VM 的完整 160 帧/输入/退休，不重复 72 个大型模块的隔离加载；报告明确标 not_run_gc_instrumentation。第一次把全量模块审计也放入强制 GC，在进入窗口前耗时超过 200 秒后主动停止，失败日志保留 `gc/initial-stopped-module-audit.log`，不是通过证据。

运行入口：`build/windows-msvc/bin/Release/weft_d3d11_probe.exe --gallery-window-hardware artifacts/local/gallery-preview/interactive`。产物/完整错误栈/截图/统计和最终 seal 位于 `artifacts/local/gallery-preview/`；旧 Canvas 与 external-texture seal 保留。可见模式无每帧 readback，隐藏测试带截图复制；FPS 的负载是 shell + 可见 Canvas，尚不能与 Ultralight 原完整 gallery 的 40 FPS 比较。800×600 backing 未变，4K 文字按设备字号生成。原游戏/第三方源码/bundle 未改；SVG/WebGL/原始完整 CSS、其余 70 例可见内容、实体输入、移动和公平性能对照仍待验证，下一步按目录中的真实阻断扩展。详见 [运行和实际限制](gallery-preview.md)。


外部纹理 lease 阶段（2026-10-09）：新增图形 API 无关的 optional C ABI extension，保留原 renderer/frame/backend v1 布局。代际 caller lease、prepare/encode/use 独立 pin、有界 registry/byte budget、caller release 失效、真实 completion 退休与 host-confirmed abandon 已实现；D3D11 reference 导入有效 SRV、验证实际 device/format/dimensions，拒绝 native resource 重复发布和 input/RTV alias。支持同 device/queue 的 linear-premultiplied RGBA8/16F/32F、top/bottom origin、UV/opacity 和有序合成，见 [实际外部纹理合同](external-texture-lease.md)。

最终 **Debug 68/68，74.25 秒；Release 68/68，27.12 秒**。实际执行 `cmake --build --preset windows-debug --parallel 10` / `ctest --preset windows-debug --output-on-failure` 和对应 Release preset。Debug/Release 均显式运行 `weft_d3d11_probe.exe --external-texture-hardware artifacts/local/external-texture/{debug,release}/hardware`，WARP 从 CTest 执行。两配置/两驱动各 48 个 GPU producer 纹理/提交、峰值两个在途、实际 query 退休 48 个 external lease 和 24 个缓存 CPU coverage texture，源像素没有 CPU 读回/重传；每 run 1,572,864 个 CPU bilinear/透明合成采样，WARP sRGB 最大差 3 色阶/linear 0.00263887，RTX 5090 为 2/0.00331104，最终调试层 warning/error 为零。阈值为 sRGB 3 色阶和 linear 1/256；有限 subtexel 精度使初始 2/0.003 的假设过严，已记录边界，不宣称逐像素一致。producer sampler 缺失和 query 重复覆盖的探针 warning 已修正。

合同测试新增 512 轮固定 seed 生命周期压力，覆盖 import/prepare failure、caller 提前释放与两个 use、取消/部分命令错误、容量/owner/epoch/重入、无分配提交/完成和 abandon；C11 新扩展头/入口通过。源/二进制/产物 SHA、最终日志、四张预览 PNG/每 run report 和只读原游戏 drift 在 `artifacts/local/external-texture/run-metadata.json` 及 `{debug,release}/`。本轮没有改 DOM/JS，未重跑 GC instrumentation；普通全量既有 DOM/Canvas/React 通过。测试时长不是性能指标，机器/画幅/资产与未测指标见上述合同。

当前仅验证 reference 的同 device/queue、单 mip/sampling、LDR linear premultiplied；encoded sRGB/straight alpha/HDR、shared/keyed mutex/多队列、driver device-removal、通用 viewport SDK/真实 4K 动态 producer、移动均未验收。WebGL2/ANGLE 尚未接入，gallery 新增通过数仍只有上一阶段的两个 Canvas2D 案例。下一步固定 ANGLE 依赖并实现最小 WebGL2/颜色转换与 canvas/context 生命周期，然后接原 Three/gallery。以下 Canvas/rAF 记录为各自历史阶段证据。


Canvas2D 阶段（2026-10-09）：两个原样 gallery 案例（粒子/烟花）已在自己的 QuickJS、native context、positioned 几何和 D3D11 中运行，各 64 个 rAF、两次移动/点击、400×300 CSS 缩放并恢复，分别 6,500/5,006 次 fill；原游戏与原始函数/包未改。图形 API 无关的 owner raster factory 接 Windows D2D SOFTWARE/WIC，输出完整 immutable sRGB straight RGBA8，Node/context tracing、detached 保活、尺寸同值重置、容量/故障/epoch 均验证。已加入最大化 4K 合成/FPS 窗口，Tab 切换、移动/点击、Esc 退出；原 Canvas backing 仍为 800×600。运行命令、精确值范围与限制见 [Canvas 阶段](gallery-canvas.md)。

最终 **Debug 67/67，74.12 秒；Release 67/67，26.16 秒；强制 GC 的 Canvas/DOM/rAF/八轮 React 4/4，51.32 秒**。实际执行两组 `cmake --build --preset windows-{debug,release} --parallel 10` / 对应 CTest preset；GC 使用 `cmake --build build/windows-gc-stress --config Debug --target weft_canvas_tests weft_script_dom_tests weft_animation_frame_tests weft_page_tests --parallel 10` 与 `ctest --test-dir build/windows-gc-stress -C Debug -R 'canvas_2d_contract|script_dom_lifecycle|animation_frame_contract|page_react_contract' --output-on-failure`。Release 首轮新增 style 断言误用了序列化空格（已改为比较颜色值）；后续发现旧 HUD probe 假设资源必在途，已增加实际提交且完成确认前检查保活。修正后 Release 全量通过，Debug 最终 Canvas/4K 范围另 5/5、9.74 秒。日志分开保存，测试时间不是性能指标；部分任务并行，不用于比较性能。

Debug/Release 的硬件 RTX 5090 与 WARP 各运行两个原样案例。每 run 20,460 个 GPU/独立 CPU 合成采样最大 1 色阶，每例 65 个真实完成提交/65 张退休纹理，峰值两帧；VM reset 后旧 FramePacket 重放一致。Chrome 155 的六张固定 bitmap 有显式 AA/低 alpha 舍入差异：分量最大 51–76、亮区均差 2.16–5.08、交并比 0.9208–0.9843，属于有界形状/颜色回归，非逐像素一致。另自动验证隐藏 4K HWND 点击队列、切换与 FPS HUD 像素；实体输入仍未人工验收。256 mutation snapshot 分批差分、200 detached callback/context 循环、quota/overlap/版本/故障合同通过。

产物与日志在 `artifacts/local/gallery-canvas/{debug,release,gc-stress,reference}/`，固定浏览器 PNG/输入 hash 在 `tests/fixtures/canvas-gallery-reference/`，源/二进制/产物及只读原游戏 drift 记录在 `artifacts/local/gallery-canvas/run-metadata.json`。只验收新增两例，其余 70 例未增加像素验收；完整 Tailwind 页面、通用 Canvas/positioned hit-test、WebGL2/ANGLE、公共 external texture lease、移动和此路径性能均未验证。下一步先 external texture lease，再固定 ANGLE/WebGL2。以下为上一阶段历史证据。


Gallery/rAF 阶段（2026-10-09）：只读捕获 CRPG UI 的 101 个文本文件和全部 **72 个原样案例**，固定原 React 19.2.4/D3/Three 0.183.2 等 59 包、5,354 个源码/许可证文件。完整原页面与十个分组可离线构建，UE bridge 只记录本地值、游戏 socket 禁用。新增可选 owner rAF，公共浮点批次时间、回调间微任务、取消/下一批快照、隐藏恢复、GC/epoch 和整体预算均有反例测试；原 React Spring 三例各由 12 个 rAF 实际改变 native DOM。完整源/依赖和复现见 [gallery 阶段](gallery-foundation.md)。

72 例原生短观察为 **38 例未发现执行错误、34 例阻断**，不标为渲染通过。主要缺口是 Canvas/WebGL、组合 selector、setInterval、NodeList、computed style/geometry 与第三方动画所需浏览器对象。完整模块能够初始化，完整原页面运行仍有错误。真实 Chrome 155.0.8059.39 的 800×600 / DPR 1 / 250 ms 单例参照没有观察到脚本错误，两个 Three.js context 确认 WebGL2；四个 Lottie 远程 JSON 明确阻止，动画未成功加载。浏览器参照不等于自己的 renderer 通过。所有 gallery native pixels/interaction/performance 与移动均未验证。

本阶段全量 **Debug 63/63，83.29 秒；Release 63/63，46.46 秒；强制 GC 的 DOM/rAF/八轮 React 生命周期 3/3，46.09 秒**。实际构建使用两个 build preset，以及 `cmake --build build/windows-gc-stress --config Debug --target weft_animation_frame_tests weft_script_dom_tests weft_page_tests --parallel 10`；全量用对应 CTest preset，GC 用 `ctest --test-dir build/windows-gc-stress -C Debug -R 'animation_frame_contract|script_dom_lifecycle|page_react_contract' --output-on-failure`。随后补充 Spring DOM 变化审计、异常消息/stack 和 callback invocation/coercion 反例，并重跑最终 rAF/gallery 范围（Debug/Release 各 5/5、GC rAF 1/1）；最终细分时间与 binary/source/产物 SHA 记录在 `artifacts/local/gallery-foundation/run-metadata.json`，全量和最终日志分开保存。部分任务并行，耗时不是性能指标。

最新 native JSON 在 `artifacts/local/gallery-foundation/{debug,release}/native-execution.json`，参照/截图在 `artifacts/local/gallery-foundation/reference/`，固定 bundle/metadata 在 `examples/crpg-gallery/native/`。正常构建不联网、不需要原游戏，源码 drift 检查只读。该 Foundation 阶段尚未实现 Canvas；现两例进展见上方。该 Foundation 阶段的公共 external texture API 尚未实现，现进展见上方；ANGLE 继续待实现；SVG/CSS/动画按逐例缺口扩展。旧连续运动窗口和 C ABI 不改变；下方为此前阶段证据。

| 模块 | 实际行为 | 边界 |
| --- | --- | --- |
| 构建 | CMake/CTest、Windows x64 Debug/Release presets、静态库、C11 调用测试 | 未验证 portable-ninja、SDK 打包与符号隔离 |
| 核心 | 实验 C ABI、Runtime/View、owner 检查、有界 View slots、跨 Runtime/代际句柄、busy destroy、失败原子性、单调 host 时间 | weft_view_record_tick 不执行 JS、布局或渲染；React 链路在独立实验宿主 |
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

本阶段完整 **Debug 58/58，54.19 秒；Release 58/58，19.89 秒；强制 GC 的 DOM 生命周期/两帧/640+4K+motion HWND 五项 5/5，48.44 秒**。实际构建 `cmake --build --preset windows-release/windows-debug --parallel 10`（分别调用），GC `cmake --build build/windows-gc-stress --config Debug --target weft_d3d11_probe --parallel 10`；完整对应 CTest preset，GC 命令 `ctest --test-dir build/windows-gc-stress -C Debug -R 'd3d11_warp_(cheetah_flow_window|two_frame)|script_dom_lifecycle' --output-on-failure`。之后仅收紧 motion 场景的 React 字符串状态断言，三配置重建 probe 并复跑新增 motion test，Debug/Release/GC 各 1/1，22.68/3.89/27.04 秒；结果记录于同阶段 `ctest-final-motion.log`。部分构建/测试并行，时长不是性能指标。

Hardware 实际命令 `weft_d3d11_probe.exe --cheetah-flow-window-test-4k-motion-hardware artifacts/local/motion/hardware`；新增 motion hardware/WARP 范围各 158 page/3 retained/161 Present，161 actual completion、serial 319、148 texture 全退休、peak=2，3,910 CPU sample 最大 1 色阶，debug warning/error=0。测试覆盖浮点时间、跳帧反转、30 Hz 保持、非法时钟原子性、按钮速度/暂停、最小化恢复、resize 命中、React 共存和重复退出。Windows / 9950X3D / RTX 5090 / MSVC Release、3840×2160、固定字体图片、13 行/70 DOM、8 warmup+120 合成 +4 ms tick，含最终 drain 的完成吞吐 130.034 tick/s；owner 至 submission P50/P95/P99/max 7.495/8.197/8.325/8.422 ms，layout P50 6.728 ms。持续位置变化比静止 UI 多出每帧全量 style/layout，隐藏长列表吞吐不等于实体 FPS。冷帧/尾部/GPU/内存、可运行步骤及限制见 [连续运动证据](flow-input-window-probe.md#连续运动展示2026-10-09)；产物与源/二进制 hash 在 `artifacts/local/motion/`。实体遮挡/显示延迟、长时间动画 soak、CSS/rAF 与移动未验收。以下两帧及更早数字是历史阶段证据。

两帧宿主阶段：**Debug 57/57，35.95 秒；Release 57/57，16.30 秒；字体/输入/browser、HWND wheel、DOM 生命周期、缓存和八轮 React 强制 GC 范围 11/11，66.33 秒**。实际构建为 `cmake --build --preset windows-release --parallel 10 --target weft_d3d11_probe`、`cmake --build --preset windows-debug --parallel 10`、`cmake --build build/windows-gc-stress --config Debug --parallel 10`；测试为对应 Debug/Release CTest preset 的 `--output-on-failure`，GC 为 `ctest --test-dir build/windows-gc-stress -C Debug -R 'flow_input|flow_window|mouse_input|script_dom_lifecycle|page_react|render_cache|font_fallback|two_frame' --output-on-failure`。部分测试并行运行，时长不是性能指标；日志与源/二进制/产物 hash 在 `artifacts/local/two-frame/run-metadata.json` 及各 debug/release/gc 目录。

4K 窗口默认最多两份宿主提交记录，两个独立 constant buffer/event query，command list 与 immutable HUD/host lease 保留到真实 completion；DOM/JS/style/layout 仍同 owner。正常 tick 非阻塞 poll，队列满等待最旧 slot，截图/resize/close 先 drain。UI 和 host-only composition 同一队列排序；实际 Submitted 上传可被后续同队列 use 复用，只有 completion 标 Ready，取消/录制失败不留下假 ready。C ABI v1 不变，同步 reference 和旧 640×420 golden 保留。独立 hardware 与 Debug/Release/GC WARP 两帧 probe：seed 0xCE102026、48 帧随机 RGBA/coverage/位置/透明顺序、196,608 像素最大差异 0；两个 Submitted pin、host lease、上传复用、丢包、录制失败、第三帧容量、提交后失败和带两份 pending slot 退出均通过。55 实际提交/55 完成、serial 110、57 texture 全退休，调试层零 warning/error。原样 4K React 仍 7 page/5 idle/12 Present、serial 19、128 texture 退休、3,128 CPU sample 最大 1 色阶，额外验证在途 HUD 替换与 resize/close drain。

同一新版 Release exe 分别运行 `--cheetah-flow-window-perf-sync-hardware artifacts/local/two-frame/sequential` 和 `--cheetah-flow-window-perf-hardware artifacts/local/two-frame/pipelined`。Windows、Ryzen 9 9950X3D / RTX 5090、调试层开启、真实 3840×2160、固定 React/Roboto/Noto/三张 PNG、13 项列表/60 DOM 元素，8 帧 warmup 后 120 次 wheel，无测量期 readback，测量后再运行构建/测试。120 帧包含最终实际 GPU drain 的 wall clock **1858.030 → 701.837 ms**，全完成吞吐 **64.585 → 170.980 帧/秒**；两帧 peak pending=2，本轮 capacity wait=0，尾部 drain 14.190 ms 已计入总区间。同步 owner tick P50/P95/P99/max 为 15.340/15.899/16.051/16.297 ms；两帧为 5.658/6.059/6.184/7.657 ms，但前者截止到 completion、后者截止到 submission，不能直接当相同的端到端显示延迟。GPU UI P50 0.068 → 0.067 ms，P95/P99/max 同步 0.072/0.084/0.090、两帧 0.073/0.077/0.083 ms，不含窗口合成/Present。冷首帧 88.538 → 75.616 ms 是各一次观察；working set 峰值 169.496 → 169.555 MiB、末尾 private usage 638.184 → 639.188 MiB；GPU cache 有效载荷峰值均 25.924 MiB，未测总 VRAM。隐藏脚本吞吐不是实体 FPS 保证，垂直同步和桌面调度仍影响窗口。实际命令、数据定义和步骤见 [两帧流水线](flow-input-window-probe.md#两帧流水线2026-10-09)。原游戏、固定 bundle、依赖未改；移动、实际 device removal、游戏宿主和实体输入延迟未验收。以下缓存同步及更早数字保留为历史证据。

4K 滚动优化阶段：主改动全量 **Debug 56/56，37.05 秒；Release 56/56，17.05 秒；字体/输入/browser、HWND wheel、DOM 生命周期、缓存和八轮 React 强制 GC 范围 10/10，77.59 秒**。实际执行 `cmake --build --preset windows-debug/windows-release --parallel 10`（各 preset 单独调用）及对应 `ctest --preset windows-debug/windows-release --output-on-failure`。GC 实际命令为 `ctest --test-dir build/windows-gc-stress -C Debug -R 'flow_input|flow_window|mouse_input|script_dom_lifecycle|page_react|render_cache|font_fallback' --output-on-failure`。之后补空字形缓存配额和超容量准备失败回滚，三配置分别重建 `--target weft_d3d11_probe`，复跑 `-R 'd3d11_warp_(render_cache|cheetah_flow_window_4k)'`；Debug/Release/GC 各 2/2，3.44/2.70/5.26 秒。完整与最后局部日志在 `artifacts/local/flow-scroll-perf/{debug,release,gc}/`；测试时长不当性能数据。

先保存带相同阶段计时器的优化前基线，再在 RTX 5090 / Ryzen 9 9950X3D、MSVC Release、真实 3840×2160、固定原版 React/Roboto/Noto/PNG、13 项列表/60 DOM 元素下测量。隐藏 own-HWND、8 帧 warmup 后 120 次 wheel，测量中不 readback：总帧 P50/P95/P99/max 从 **155.257/201.367/217.471/218.812 ms** 降为 **15.384/15.996/16.558/16.963 ms**，131 次页面提交 texture 创建/上传从 16,563 次降为 128 次，GPU cache hit 16,435。GPU UI command-list P50 1.255 → 0.069 ms，不含 host composition/Present。冷首帧 82.946 → 88.849 ms；进程峰值 working set 179.840 → 170.188 MiB、末尾 private usage 693.574 → 639.090 MiB；GPU cache 有效载荷峰值 25.924 MiB，不是总 VRAM。前一轮有 total 92.576 ms/query wait 84.993 ms 尖峰及冷首帧 205.410 ms，保留在 `optimized/previous-run`，没有掩盖等待尾部和首次加载成本。实际命令 `weft_d3d11_probe.exe --cheetah-flow-window-perf-hardware artifacts/local/flow-scroll-perf/optimized`；这是固定脚本路径测量，实体长时间 FPS/移动/产品预算仍未验收。

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
