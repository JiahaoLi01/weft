# CRPG gallery：固定验收与 rAF 第一阶段

2026-10-09。目标是让用户指定的 gallery 全部案例在 Weft 的绘制后端和输入路径工作。本阶段交付可重复的原样案例诊断，以及实验 owner rAF；**没有交付 Canvas/WebGL、完整 gallery 绘制或性能结论**。

## 固定 workload 与四层验收

只读捕获 `F:\Projects\CRPGFramework-5.6.1\CRPGFramework-5.6.1\UI` 的 101 个文本文件，1,158,527 bytes；没有写原项目、安装包或运行游戏服务。`tests/fixtures/crpg-gallery/manifest.json` 固定文件 SHA256，`inventory.json` 用 TypeScript AST 提取全部 72 个注册案例的标签、源码位置和逐案例 SHA256。原始标签里的历史 ✗ 保留，不能据此判定本项目成败。

新增 `third_party/gallery-runtime/` 固定 59 个实际安装包、5,354 个文件、133,430,667 bytes。React/React DOM **19.2.4**、D3 **7.9.0**、Three.js **0.183.2** 等与捕获的原 npm lock 版本一致；与旧 React fixture 的版本分别保留。源码及许可证随仓库保存，esbuild 的平台二进制许可证由同版本 wrapper 的 MIT 文本提供。manifest 记录原 tarball URL/integrity、依赖、文件 SHA256 和来源。**这是安装源码的版本/字节固定；未独立验证 npm tarball integrity。** Windows 构建工具含固定 win32 esbuild binary；其他平台重新打包工具未验证。

正常 CMake/CTest 消费已有 bundle，不联网、执行 npm install 或访问原游戏。手动重建工具只从本仓库固定源码恢复忽略的 node_modules junction。原 gallery 模块的函数体和第三方包没有修改；构建仅追加注册表 exports，作者 harness 包装 mount/unmount 与错误收集。完整原 `TestGallery` 与十个分组入口均保留；分组用于定位缺口，不能替代完整页面验收。

每例分别记录执行、像素、交互和性能。执行阶段的 `no_error_observed` 仅说明短观察没有发现错误，**不是 supported/passed**。原生像素必须由自己的后端实际产生并独立检查；动画/拖拽/滚动必须按原例行为验证；性能另固定硬件、分辨率、帧时钟、随机种子、节点/资产和采样条件，包含尾部与 CPU/GPU 完成。

## 实际首轮结果

Native 每例新建 VM，用原 React 组件和依赖运行 12 个 owner 绘制机会；时间为 `0.25 + 16*n` ms，前后执行有界 timer pump。没有加载完整 Tailwind CSS、安装 layout host 或派发输入，没有绘制到 GPU。每例均检查 unmount/reset，reset 后回到 3 native nodes、1 document root。

| 原始分组 | 数量 | 未观察到执行错误 | 阻断 |
| --- | ---: | ---: | ---: |
| CSS | 26 | 26 | 0 |
| D3 | 8 | 4 | 4 |
| D3 interactive | 4 | 2 | 2 |
| K-Line | 1 | 0 | 1 |
| Canvas2D / Three.js | 4 | 0 | 4 |
| Lottie | 4 | 0 | 4 |
| Anime.js | 14 | 2 | 12 |
| Framer Motion | 6 | 0 | 6 |
| React Spring | 3 | 3 | 0 |
| Lucide | 2 | 1 | 1 |
| 总计 | **72** | **38** | **34** |

主要错误来自 DOM compound 以外的 selector、`setInterval`、NodeList、computed style/geometry 等浏览器对象以及 Canvas/WebGL 缺失。异常记录包含原始消息和 bundle stack；Three 捕获异常并显示错误 UI 仍算阻断。完整模块能够初始化，完整原页面运行时仍有错误，不能宣称整页支持。

原版 React Spring 的三例实际各执行 12 个 rAF：数字从 **72 → 842**，进度宽度从 **0.83335% → 9.63335%**，trail 的 opacity/transform 在原生 DOM 中变化。该检查证明 VM → rAF → 原包 → native DOM 链路；没有验证 CSS transform/opacity 绘制或与浏览器动画轨迹一致。

真实 Chrome **155.0.8059.39** 的 800×600 / DPR 1 离线参照逐例观察 250 ms，72 例没有观察到脚本错误。两个 Canvas2D 和两个 Three.js WebGL2 context 实际创建，Three 报告 ANGLE / RTX 5090 / D3D11。四个 Lottie 原 URL 被显式阻止、未加载 JSON，不能算动画成功。参照 CSS 由固定 Tailwind compiler 与静态候选生成，原字体资产未捕获；截图只作浏览器参照，未做像素正确性/交互验收。浏览器里的 ANGLE 不代表 Weft 接入了 ANGLE。

## rAF 实际合同

`src/script/js/animation_frame_bootstrap.js`（构建时内嵌，`install_script_platform(vm, ScriptPlatformModule::AnimationFrame)` 安装）是可选 M0 平台模块；`ScriptDom::animation_frame(double time_ms, bool drawable)` 在 UI owner 驱动。旧 flow motion 展示继续用局部时钟，公共 Runtime/View C ABI 不变。

- 注册异步回调，单批快照与公共浮点时间戳；回调中新注册的任务留到下一次绘制机会。取消可从前一个回调的微任务检查点阻止后续回调。
- 每个回调后清空有界微任务；整批共享一次 execution deadline 和 10,000 job 上限。普通 callback exception 记录后继续；VM 资源/时间失败进入 Faulted，必须 reset，残余任务不能自动续跑。
- 4,096 pending、每批最多 1,000、非复用 uint32 handle；超限有诊断。最多保存 64 条、每条 512 字符的回调错误。
- pending Map 由 QuickJS tracing，持有 detached node 的闭包在调用/取消后可以 GC。reset 清空旧 VM/epoch 队列；跨线程调用拒绝。
- `drawable=false` 推进宿主时钟并保留 pending，恢复只派发当前批次，不补发历史绘制机会；标准 visibility/document lifecycle、browser error event 尚未实现。host 必须保证每个绘制机会只调用一次，API 不根据相同 timestamp 自动去重。

测试覆盖微任务取消/新注册、时间单调与失败原子性、普通错误/恶意 formatter、每批公共时间戳、隐藏恢复、quota、整体超时、无限微任务、wrong owner、GC 与热重载。强制 GC 只验证这些合同和既有 DOM/React 生命周期；没有在 GC instrumentation 下执行 72 例大 bundle。

## 下一阶段合同

先完成 Canvas2D 的两例：backing size 与 CSS size 分离、`getContext` 类型互斥、fillRect/arc/path/fill、颜色/alpha、指针与资源生命周期，实际输出可更新的纹理。再接公共外部纹理 lease：代际设备/资源身份、encoding/alpha、宿主 producer/consumer 同步、实际 submission 完成后退休；渲染线程不访问 Node/JSValue，帧包独立。

WebGL2 的标准 JS 入口、对象身份、参数/错误校验和 DOM/canvas 生命周期由本项目实现；**ANGLE** 候选负责 ES 3 着色器与图形后端，复用同一 canvas/外部纹理合成路径。ANGLE 不直接提供浏览器 WebGL JS API。宿主引擎 viewport 另提供直接纹理入口，适合游戏模型/场景；不能拿它替换 Three.js 来宣称原 WebGL 例通过。ANGLE 尚未固定候选源码或完成 probe；D3D11、Vulkan、Metal 和移动都须各自验证。接入前先验证固定 ANGLE build、QuickJS typed arrays/同步 WebGL 调用、同设备纹理交换、resize/context loss/销毁和透明合成。

其他缺口按 SVG paint → DOM selector/CSSOM → CSS 动画及第三方调度/交互推进。Lottie 的四份 JSON 需要由宿主资源 provider 固定来源与 SHA，并在原 URL 调用契约下加载；不开放任意游戏服务连接。某例无效资源、源码问题和渲染缺失分别记录，不把删例、占位 UI 或换库算通过。

## 复现与产物

在仓库根目录运行；不需要原游戏路径：

~~~powershell
cmake --build --preset windows-release --parallel 10
ctest --preset windows-release
.\build\windows-msvc\bin\Release\weft_gallery_probe.exe artifacts/local/gallery-foundation/manual
python tools/verify_gallery_execution.py artifacts/local/gallery-foundation/manual/native-execution.json
# 可选：只用固定源码重建 JS/CSS
node tools/build_gallery.mjs
# 可选：真实浏览器参照，所有外部请求会被阻止
node tools/browser_gallery_reference.mjs artifacts/local/gallery-foundation/reference
# 人工浏览原页面，UE bridge 只捕获本地值
python -m http.server 4174 --bind 127.0.0.1 --directory examples/crpg-gallery/native
~~~

浏览器打开 `http://127.0.0.1:4174/?original=1`；单例例如 `/?group=WEBGL_TESTS&index=2`。这是浏览器参照入口，当前没有 gallery 原生 GPU 窗口。固定 bundle/metadata 在 `examples/crpg-gallery/native/`；最新 native 报告在 `artifacts/local/gallery-foundation/{debug,release}/native-execution.json`，最新浏览器在 `artifacts/local/gallery-foundation/reference/`。阶段测试日志、原源码 drift 只读检查与 SHA seal 在同一阶段目录，具体结果见 [status](status.md)。

后续 2026-10-09 两例 Canvas2D 已有原生像素/交互证据，公共 external texture lease 的首版同 device/queue linear-premultiplied 输入亦已完成，见 [Canvas](gallery-canvas.md) 与 [external lease](external-texture-lease.md)。上述 Foundation 的短观察和历史 seal 不扩张为新增案例通过；ANGLE 仍待接入。
