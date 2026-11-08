# 游戏内 UI：导出纹理扩展

2026-11-06：设计与首版实现（C ABI、公共层、Metal 后端、契约测试）和 macOS 3D 示例。Vulkan/D3D11/D3D12 后端、iOS 运行、图集与脏矩形尚未做。

## 要解决的问题

现有渲染器只有两个方向：

- **画到宿主的目标上**（`weft_renderer_encode`）：HUD、菜单这种屏幕空间 UI，画在游戏画面之上。
- **外部纹理导入**（[external-texture-lease.md](external-texture-lease.md)）：宿主的纹理进入 UI 合成（`-weft-texture(name)`）。

游戏内 UI（世界空间的面板、终端屏幕、角色头顶的血条、手持平板）需要反过来：**UI 画进一张纹理，交给宿主，宿主在 3D 世界里当普通材质采样**。

宿主自己建一张 BGRA8 纹理再调 `encode` 也能"凑合"，但有四个问题：

1. 场景 pass 输出的是 **sRGB 编码的预乘色**（浏览器合成契约），引擎在线性空间做光照和混合，直接采样会偏暗、半透明边缘发黑。
2. 没有 mip：世界空间的面板离远了会严重走样闪烁，宿主得自己补一遍生成 mip 的 pass。
3. 宿主要自己管轮换和同步：UI 正在写的纹理不能同时被上一帧的 3D pass 读。
4. 每一帧都重画浪费：UI 通常几秒才变一次，而游戏每秒 60–120 帧。

导出扩展把这四件事收进渲染器，契约和导入扩展对称。

## 和导入扩展的对称关系

| | 导入（external texture） | 导出（export texture） |
| --- | --- | --- |
| 纹理归属 | 宿主 | weft（渲染器创建、销毁） |
| 谁借给谁 | 宿主借给 weft，weft 持调用方 lease | weft 借给宿主，宿主持图像 lease |
| 借用结束 | weft 的使用全部完成 → 退休回调 | 宿主释放 lease 并给出最后一次采样它的提交序号 → 那次提交完成后回到空闲 |
| 内容 | 线性预乘 | 线性预乘（采样后得到的值） |
| 同步 | 同 device、同队列，生产者写入排在 UI 提交之前 | 同 device、同队列，宿主采样排在 weft 的写入提交之后 |

## C ABI

头文件 [render_export_texture.h](../include/weft/render_export_texture.h)，可选扩展，不改动 v1 核心布局。

- **安装**：`weft_renderer_install_export_backend`，在渲染器空闲时装一次。后端表给出支持的格式、所有导出加起来的图像数上限和字节预算，以及三个回调：`create_image`、`destroy_image`、`encode`。公共层在安装时一次性分配全部跟踪容量，之后提交/完成回调不分配。
- **导出对象**：`weft_renderer_create_export(desc)` 按尺寸、格式、mip 级数、环大小（1–8 张）**一次性创建全部图像**，游戏运行中不再分配显存。`weft_renderer_destroy_export` 立即使句柄失效；宿主仍持有的图像和仍在途的写入照常存活，结束后再销毁。
- **绘制**：`weft_renderer_encode_export(prepared, export, pass)` 选一张空闲图像，把 View 帧画进去并转换成线性预乘、生成低级 mip，返回普通的 use 句柄（照常 `notify_submitted` / `notify_completed` / `discard_encoded`）和一张图像 lease（`weft_render_export_image`：句柄、版本号、尺寸、格式、mip 级数、后端原生纹理对象）。没有空闲图像时返回 `BUSY`，什么都不录，宿主继续显示手里那张，下一帧再试；prepared 帧仍然有效。
- **释放**：`weft_renderer_release_export_image(image, queue, serial)`。句柄立即失效；`serial` 是宿主**最后一次采样这张图像的提交**（从未采样填 0）。该序号完成（`notify_completed` 到达它）且写它的 use 已退休后，图像回到空闲环。宿主可以在采样它的命令提交之前就释放，只要那个序号之后确实会提交并报告完成。
- **查询**：`weft_renderer_get_export_info` 返回支持的格式、导出数、存活图像数、宿主持有的 lease 数和字节数。

图像的状态只有三种条件：宿主持有 lease、有未退休的写入 use、释放序号尚未完成。三者都不成立时才会被选中重写。

`weft_renderer_destroy` 遇到存活的导出或图像返回 `BUSY`，与存活的导入纹理一样。设备 abandon 后释放序号不再等待（宿主已保证设备不再访问）。

## 颜色、mip 与坐标

- 宿主通过图像格式的视图采样得到的值是 **线性预乘 RGBA**，与导入扩展对外部纹理的要求一致，引擎可以直接用预乘 alpha 混合（`One, OneMinusSrcAlpha`）叠到线性 HDR 场景里。
- 格式：`SRGB_RGBA8` / `SRGB_BGRA8`（推荐，8 位存储，硬件采样时解码到线性，过滤在线性空间进行）和 `LINEAR_RGBA16F`（HDR 管线、需要更高精度时）。线性 8 位格式的暗部会出色带，不在 Metal 首版列表里。
- 绘制路径：场景 pass 照常在 sRGB 编码预乘空间合成（保持和浏览器一致的观感）到一张 BGRA8 暂存纹理，然后一个全屏三角形的转换 pass（[export_resolve.hlsl](../src/render/shaders/export_resolve.hlsl)）逐像素做"反预乘 → sRGB 解码 → 重新预乘"，写进导出图像的 0 级。暂存纹理按 use 归属，use 退休后回到后端的备用池。
- mip：`mip_levels` 为 0 表示完整链。转换后在同一个命令缓冲里生成低级 mip（Metal 用 blit encoder 的 `generateMipmapsForTexture`）。预乘数据的 mip 过滤是正确的（不会出现直通 alpha 的暗边）。
- 坐标：0 行是页面顶部（左上原点），与 Metal、Vulkan、D3D 的纹理坐标一致；OpenGL 宿主需要翻转 V。
- 页面按 `pass` 的尺寸和 prepared 帧的缩放绘制：导出 1024×512、View 逻辑尺寸 512×256 时用缩放 2。

## 宿主一帧的流程

```c
// 启动时
weft_renderer_install_export_backend(renderer, &export_table);   // Metal: weft_render_metal_export()
weft_render_export_desc desc = {sizeof desc, WEFT_RENDER_EXPORT_ABI_VERSION, identity,
                                1024, 512, WEFT_RENDER_TARGET_SRGB_RGBA8, /*mip_levels*/ 0, /*images*/ 3};
weft_renderer_create_export(renderer, &desc, &panel);

// 每帧
weft_view_acquire_frame(runtime, view, &frame);
if (revision(frame) != shown_revision || animated(frame)) {       // 只在 UI 变了时画
    weft_renderer_prepare_scene(renderer, &scene, &prepared);
    if (weft_renderer_encode_export(renderer, prepared, panel, &pass, &use, &image) == WEFT_RENDER_OK) {
        if (current.image.kind) weft_renderer_release_export_image(renderer, current.image, queue, last_frame_serial);
        current = image;
        shown_revision = revision(frame);
    }
    weft_renderer_release_prepared(renderer, prepared);
}
draw_world(current.native_texture);          // 同一个命令缓冲里，在 weft 的命令之后
commit(); weft_renderer_notify_submitted(renderer, use, queue, serial);
// 帧完成时
weft_renderer_notify_completed(renderer, epoch, queue, completed_serial);
```

## 输入

宿主负责把世界里的交互映射回页面：射线与面板求交 → 面板 UV → 逻辑 px（`u × logical_width`，`v × logical_height`）→ `weft_view_pointer`。`weft_view_hit_test` 告诉宿主这个点上是否有可交互元素，决定点击给 UI 还是给游戏。这部分不需要新接口，放在后续的 3D 示例里演示。

## 性能要点

- **只在新帧时画**：UI 静止时每帧开销为零，3D pass 只是多采样一张纹理。帧的 `revision` 不变且非动画时跳过。
- **不分配**：图像在 create_export 时全部建好；暂存纹理、录制 slot 走后端的备用池；公共层回调路径不分配。
- **环大小**：同一命令缓冲里画完就采样、每帧释放上一张时，2 张足够；宿主多帧在途（三缓冲）时用 3 张。环满返回 `BUSY`，不会阻塞或等 GPU。
- **字节预算**：所有图像（含 mip）的逻辑字节受后端预算限制，Metal 默认 256 MiB、最多 32 张图像。
- **mip**：远处面板采样低级 mip，既减少带宽也消除闪烁；只有在 UI 变化时才重新生成。

## macOS 3D 示例

`samples/macos-ingame`（`weft_macos_ingame.app`）只用公开头文件，演示宿主一侧的完整做法：

- **场景**：天空、网格地面、中间一个会转的反应堆核心，三块世界空间面板。三个 View 在同一个运行时里，每块面板一个导出（环 3 张、`SRGB_RGBA8`、完整 mip）。
  - 控制终端（480×320 px，2 倍）：三个按钮改变反应堆输出，页面用 `weft.engine.send('reactor.output', n)` 通知游戏，核心的颜色、亮度和转速跟着变。
  - 状态板（420×260 px，2 倍）：游戏每秒发一次 `status` 事件，更新时钟和温度柱。按 A 打开一条每帧都在动的扫描条，用来测"页面每帧都变"的最坏情况。
  - 远处的招牌（1024×256 px，2 倍）：大字和细斜纹。按 M 切换有无 mip 的采样器，可以直接对比。
- **只在变化时画**：每帧取 View 最新的帧，`revision` 与面板上那张图像的相同就跳过。所有要重画的面板录进同一个命令缓冲，先于世界的命令缓冲提交到同一队列，用同一个帧序号报告提交。旧图像用上一帧的序号释放。
- **世界**：sRGB 目标、线性光、4×MSAA（Apple GPU 上颜色和深度都是 memoryless），面板按距离从远到近、预乘 alpha 混合，采样器为三线性加 8× 各向异性。
- **输入**：鼠标位置换成相机射线，与各面板平面求交，取最近且落在页面内的那块，交点换成页面的逻辑 px 交给 `weft_view_pointer`。光标形状来自 `weft_view_hit_test`。按下后指针被这块面板捕获，直到松开。离开面板时发 `LEAVE`。没打在面板上时，拖动转相机，滚轮拉近拉远。
- **自动化**：`--shot` 无窗口画 90 帧，第 30 帧把 +10 按钮的页面坐标投影到屏幕，再走同一条射线路径点击。程序会检查射线落点、按钮下的光标，以及游戏收到的输出从 40 变成 50。`--bench S` 让相机自动绕圈，关垂直同步，输出帧时间的分位数。

实测（2026-11-06，Apple M5，macOS 26.6，Debug 构建，窗口 1280×720 点、2560×1440 px、4×MSAA，关垂直同步，相机绕圈，各 10 s，去掉第一秒）：

| 情形 | CPU（View 推进 + 导出 + 世界录制） | GPU 世界 | GPU 页面（有重画的帧） | 页面重画 |
| --- | --- | --- | --- | --- |
| UI 基本静止（状态板每秒一变） | p50 0.044、p99 0.057、最大 3.3 ms | p50 0.14、p99 0.63 ms | p50 0.18 ms | 10 s 内 14 次 |
| 状态板每帧都动 | p50 0.160、p99 0.186、最大 6.1 ms | p50 0.25、p99 0.76 ms | p50 0.15、p99 0.68、最大 1.3 ms | 每帧 1 次（840×520 px，含转换和 mip） |

- UI 静止时，面板对每帧的开销只是三次纹理采样，相机移动也不会让页面重画。
- 一块页面每帧都变时，CPU 每帧多约 0.12 ms，GPU 多约 0.15 ms。
- 启动的第一帧画三块页面用了约 29 ms GPU（字形、图像首次上传和纹理创建），之后不再出现。
- 单帧最大值（3.3 ms / 6.1 ms CPU）的来源见下一节。

### CPU 峰值（2026-11-06）

用 `--profile` 把每帧 CPU 拆成几个阶段：游戏发事件、每个页面的 advance、每个重画页面的 prepare 和导出录制、提交、世界录制，并列出最慢的帧。`--status-every-frame` 让状态板每帧都更新，用来给 Time Profiler 攒样本。

- **位置**：每秒一次，就是游戏发 `status` 事件的那一帧。点击终端的那一帧也一样，都是"页面上的文字变了"。其他帧 Release 下 p50 0.03 ms。
- **大小**：
  - Release 离屏（`--shot --frames 7200 --profile`）：稳定在约 0.6 ms（状态板 advance 约 0.33 ms + 导出录制约 0.23 ms），偶发到 1.8 ms。120 s 内不增长；数值在约 0.35 ms 和约 0.6 ms 两档之间跳，像是 CPU 换核或换频率。
  - Debug：2–4 ms。
  - 窗口被遮住、帧率被系统压到 60 fps 时，CPU 降频，Debug 下到 5–10 ms。前面表里的最大值就是这种情况。
- **冷启动**：同样的更新每帧都做时只要约 0.12 ms（Release，`--status-every-frame`），每秒做一次却要约 0.6 ms。大半是缓存和 CPU 频率的冷启动。
- **Time Profiler**（Release，每帧更新，3596 个样本，`xctrace record --attach`）：
  - advance 占主线程 48%。
    - 约一半是页面脚本。16 次 `style.height` 写入走 [web_host_bootstrap.js](../src/script/js/web_host_bootstrap.js) 的 CSSOM Proxy：每次写入都先用正则转属性名，把整个 `style` 属性 split 成 Map，再 `Array.from`/`join` 回字符串，最后 `setAttribute`，原生端再解析一遍。这部分约占 advance 的 40%。
    - 另一部分是样式、布局和文字测量（`SceneSolver::update`），约 37%。
  - 导出录制占 42%。
    - 变化的文字在 CPU 上用 FreeType 栅格化成整段图像（`scene_text_image` → `Font::rasterize`），约 33%。
    - Metal 驱动创建编码器（场景 pass 的几个 render encoder、resolve、mip 的 blit），约 30%，其中生成 mip 约 10%。
- **对照实验**：把 16 次 `style.height` 换成一次 `style.cssText` 写入，每帧更新的 CPU p50 从 0.119 降到 0.094 ms（各跑三次）。
- **结论**：没有异常，也没有泄漏。峰值就是"页面内容变了"的正常代价，Release 下远低于一帧的预算。
- **已修**（2026-11-07）：内联样式的单个属性写入改成原生实现。
  - `element.style` 的写入和读取各变成一次原生调用（`__ceStyleSet`、`__ceStyleGet`，在 `src/script/script_dom.cpp`），在 C++ 里解析、修改并写回 style 属性。JS 那层只剩 Proxy、属性名换算（带缓存）和 `String()` 转换。
  - 语义和原来的 JS 实现逐字一致：
    - 按 `;` 切分，同名声明原地更新；
    - 名字和值按 `String.prototype.trim` 的规则去空白（含 Unicode 空格与行终止符）；
    - 括号或引号不配对的值忽略，空值删除该属性；
    - 写回格式为 `name: value;` 用空格连接。
  - 新测试 `inline_style_contract` 把原来的 JS 实现原样留作参照，24 个种子各 1500 步随机操作（赋值、`setProperty`、`removeProperty`、`cssText`、`setAttribute`），每步比较属性文本和所有读取。值里有 Unicode 空白、引号、括号、分号和非 ASCII 文字。故意注入三处错误（漏掉 U+3000、接受不配对的值、同名追加而不更新）都会被抓到。
  - Debug 下单次属性写入从 16.7 µs 降到 3.5 µs。
  - 示例里的效果（Release，新旧两个程序交替各跑多次）：
    - 状态板每帧更新时，CPU p50 从 0.117 ms 降到 0.094 ms，p95 从 0.154 ms 降到 0.132 ms，p99 从 0.202 ms 降到 0.171 ms。
    - 每秒一次的峰值主要取决于 CPU 当时的状态：新旧两版同样在约 0.28 ms 和约 0.6 ms 两档之间，这次修改省下的约 25 µs 在噪声以内。
- **已修**（2026-11-07）：静止文字的字形缓存。
  - 文字图像本来就是逐个字形用 FreeType 栅格化再合成的。现在 `SceneText::glyph` 按（字体、字形、字号、偏移、倾斜）的精确位模式缓存每个字形的覆盖率位图，相同输入得到逐字节相同的结果。
  - 只给静止文字用（`scene_text_image` 带 phase 时）：它的笔位落在横向 1/4 像素、纵向整像素上，内容变而位置不变的一行（时钟、计数器、日志）只需栅格化没画过的字形。移动或变换中的文字位置任意，照旧直接栅格化，不污染缓存。彩色字形（emoji）不变。
  - 上限 4096 个字形、4 MiB，满了整体清空，与 `SceneText` 现有缓存的做法一致。
  - 新测试 `scene_glyph_cache`（可移植，用源码树里的 Roboto）：
    - 600 个随机用例（文字、字号、缩放、phase、颜色、合成斜体），缓存出的图像与一个全新 `SceneText` 直接栅格化的逐字节相同；
    - 时钟从 `00:00:00` 到 `00:00:01` 只栅格化 1 个新字形；
    - 不带 phase 的文字不读也不写缓存；
    - 字形数超过上限后，缓存会清空且始终在上限内，清空后图像仍然一致。
  - 效果（Release）：
    - 一行 22 px、2 倍的时钟文字图像从 24.7 µs 降到 13.1 µs（−47%）。
    - 示例里状态板每帧更新的 CPU p50 从约 0.104 ms 降到约 0.093 ms（新旧交替三轮，两轮降、一轮持平），每秒一次的峰值中位数 0.27 → 0.26 ms，p90 0.35 → 0.31 ms。
    - 这个页面每次只换一行短文字（约 9 个字形），FreeType 原本只占一次更新的约 9%，所以上限就是一成左右。文字变化多、字号大、字形复杂（CJK）时收益更大。
  - 剩下的文字图像时间主要是整张图的分配、清零和逐像素合成。
- **还能优化的点**：减少导出路径的编码器数量（约占导出录制的三成）：resolve 并进最后一个场景编码器，mip 用一次 compute 或渲染完成。

## 后续

1. iOS 验证；Vulkan、D3D12、D3D11 后端（Vulkan 需要定义写入后的 image layout 和队列族）。
3. 图集：多个小面板共享一张大纹理，减少绑定和 draw call。
4. 脏矩形：只重画变化的区域（依赖场景 pass 的局部重画能力）。
5. 直接线性合成：16F 导出可以省掉暂存纹理和转换 pass，但会改变半透明叠加的观感，需要单独的合成模式。
