# 自动布局实际合同

这是 M0 的受限页面探针。新增 `caseelephant_auto_page_probe` 和 `build_auto_page`，接收配对的 DOM/computed-style 值树、逻辑 viewport 和固定字体集合，输出自己的 box/paint 值。旧 positioned、Flex 页面接口及旧证据继续保留。

## 已实现

根 scope 要求 `position:relative`；width:auto 按 viewport 扣除 padding，根百分比尺寸按 viewport 求值。节点支持 block/flex/none，absolute 或 Flex item 的 inline display 按 blockification 处理。其他 inline、mixed text/element、grid 等拒绝。所有 margin、border 必须为零，padding 支持 px；正常流百分比 height 要求显式 definite parent height。

block 内容流依次纵向排列，自动高度来自正常流内容及 padding，绝对定位不撑高父盒；block 子项不会因父高度不足而 Flex shrink。此限定子集明确降为 Yoga 的 column packing；不把一般 block 交给 Yoga，也不支持 margin collapse。真实 Flex 继续用已有 CSS adapter，保留 row/column、wrap、gap、grow/shrink/basis、align、min/max 和含定位子树的尺寸计算。部分复杂 intrinsic/percentage 组合仍会诊断，不能从支持属性名推断所有组合。

margin/padding/border-width 的 1–4 值 shorthand 在公共 cascade 中展开到 longhand，先排名再替换变量并选择相应边；任一边无效时整组 invalid-at-computed-value，不能回退到较早声明。此扩展仅由新 profile 启用，旧 single-value profiles 不扩大。

文字测量与最终绘制使用同一行生成器和 FontSet，中文/拉丁 nowrap 的宽度来自完整塑形结果，灰度 glyph 带 font_id。ASCII normal 在空格处换行并输出多行 paint；最终 content width 再排版，支持 left/center/right。主字体浮点 strut 不代替通用 inline line box。Unicode 换行、bidi、富文本、font shorthand、其他字重/斜体、编辑和 CJK SDF 尚未实现。

absolute 支持 auto content sizing、px/% 和 `calc(percent +/- px)`。calc 在 containing-block 祖先依赖上全量重算，最多 16 轮/16,384 node-pass；超限诊断。矩形 overflow:hidden、NineSlice 图片 crop UV 和直接 isolated parent 下的有界 z-index沿用 paint 合同。没有通用 hit-test、滚动、圆角、transform、opacity group 或 HTML parser。

树最多 2,048 节点、深度 64，单叶文字 64 KiB、合计 1 MiB；每次行测量输入累计最多 1 MiB，viewport 1–4,096，坐标/尺寸另有有限值界限。没有缓存/增量/生产 atlas 或布局性能声明。规范基线为 [Flex automatic minimum size](https://www.w3.org/TR/css-flexbox-1/#min-size-auto) 和 [CSS2 absolute sizing](https://www.w3.org/TR/CSS2/visudet.html#abs-non-replaced-width)，具体证据仅覆盖下列 fixture。

## Workload 与证据

`examples/cheetah-auto` 复用完全相同的固定 AlertBar/React/Zustand/store/UE bundle、主题变量和 NineSlice 资产。自己的预览 CSS 移除 AlertBar 内部固定 width/height 和 i/b/span 绝对坐标，以 Flex/padding/内容宽度生成按钮，列表保持指定宽度、高度自动。外部 gallery/card/test buttons 仍有固定尺寸；不是完整 operations.css，展开颜色规则有作者 adapter 的 scope。

八帧真实组件/store 状态、13 项显示前 12 项及 overflow notice、facility/company 两个桥 payload、incoming UE 回执、stale target、reset/GC 后绘制均验证。2 项列表高 48 px，13 项含 notice 高 224 px；绝对列表不改变 32 px 的 header/chip 高度。两位数字使 header 变宽，列表仍独立定位。

浏览器八帧 1,336 个几何字段最大差异 0.258087 CSS px，2,672 个颜色分量最大差异 0.0000003922。逻辑几何阈值 0.3 px；实际浏览器 DPR 1.005 带来小数 padding/line-height 的累积量化。此前固定高度定位阶段 0.2 px 阈值保留。没有 browser/native pixel parity 承诺，线性透明合成仍遵守已有颜色边界。

确定性随机 fixture 共 16 棵树 × 3 轮，48 场景/1,984 个几何字段，最大差异 0.184082 px（阈值 0.3）。同一 JS 在浏览器和原生 VM 修改宽度/gap/中文长度/display、插入删除子项；另覆盖 nested block/Flex、wrap、ASCII 多行、padding、absolute 不撑高、无效 shorthand、身份配对和 owning output。差分找到并修复了 display:none 几何非零及隐藏项参与 intrinsic 统计的问题。

Debug/Release 的 RTX 5090/WARP 各输出八帧及 font_id cache collision 反例。每次独立 CPU 图片/coverage oracle 191,203 样本，最大误差硬件 2/WARP 1 色阶；568 个 fallback glyph、822 textures 实际 query 完成后退休，debug layer 零 warning/error。图形后端接口没有绑定到布局库，GPU 只收到 owning paint/resource 值。移动、完整 UI、真实 UE 游戏命令及性能均未验证。

## 复现

```powershell
cmake --build --preset windows-debug --parallel 10
ctest --preset windows-debug --output-on-failure
& build/windows-msvc/bin/Debug/weft_d3d11_probe.exe --cheetah-auto-hardware artifacts/local/cheetah-auto-debug/hardware
cmake --build --preset windows-release --parallel 10
ctest --preset windows-release --output-on-failure
cmake --build build/windows-gc-stress --config Debug --parallel 10
ctest --test-dir build/windows-gc-stress -C Debug -R 'auto|page_react' --output-on-failure
python tools/verify_auto_fixture.py
python tools/record_auto_evidence.py
```

普通 CMake/CTest 不联网，不读取原游戏，也不执行 Node/Vite 构建。浏览器复现先 `python tools/build_cheetah_auto.py`，再 `python -m http.server 4178 --bind 127.0.0.1 --directory examples/cheetah-auto`；开发预览 [http://127.0.0.1:4178/](http://127.0.0.1:4178/)。只生成自己的资产副本，完整性工具会检查已存在副本。

全量最终测试与 sealed metadata 见 [status](status.md)。证据位于 `artifacts/local/auto-{debug,release,gc-stress}/run-metadata.json`、`artifacts/local/cheetah-auto-*`；浏览器参考在 `tests/fixtures/{auto_layout,cheetah_auto}_browser_reference.json`，实际截图在 `artifacts/local/cheetah-auto-reference/frame-7.png`。

本阶段之后，2026-10-08 的独立 [flow profile](flow-layout-probe.md) 已加入 nested inline、Unicode 断行、同步 geometry/滚动及圆角；本页旧 auto profile 与历史参考保持原边界。下一步是 flow 命中/平台输入、真实档案 UI 和 D3 SVG；block 不能直接扩大到 margin collapse 或 float。
