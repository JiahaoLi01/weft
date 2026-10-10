# SDF 字体：当前实现与合同

本轮在 C++20 字体适配器和独立 D3D11 探针中实现了**单通道 SDF**。复用已固定的 FreeType 2.13.3 与 HarfBuzz 11.2.1，没有修改上游源码。它还不是 HTML/CSS 页面中的正式文本后端；中文回退、移动 GPU、通用 atlas 管理和 MSDF 尚未实现。

## 字体与绘制职责

HarfBuzz 的 glyph ID、advance、offset 与 cluster 决定逻辑排版；SDF 只改变字形图像与 shader 材质。生成时记录 base ppem、spread、generator 和包含 padding 的 bitmap bearing。绘制 quad 使用该 bearing，不能把 padding 加进 advance、line box 或文本 hit box。空格继续保留 advance，返回空 bitmap，不分配 quad。

`TextProbeFont::rasterize_sdf` 接受 base 16..128 ppem、spread 2..32 源 texel，字体仍在 owner 线程串行使用。默认先生成普通灰度 bitmap，再让 FreeType 的 bsdf renderer 生成距离场；显式 Outline 选项直接使用 sdf renderer，不做隐式 generator fallback。每次重置 range、sign、Y 方向与 outline overlap 策略。FreeType 此处的 flip_sign/flip_y 属性参数是 FT_Int，overlaps 是 FT_Bool；两者不能混用。

bsdf 避免直接 outline renderer 对细小特征、重叠/自交轮廓的部分限制，但保留了基础 bitmap 的精度边界。单通道距离场的尖角与大倍率效果需要独立质量验收；不会承诺任意倍率与字体都精确。FreeType 的限制与 generator 说明见 [官方 spread 文档](https://freetype.org/freetype2/docs/reference/ft2-properties.html#spread)；实际实现以本仓库固定 2.13.3 的头文件和源码为准。

## R8 距离材质

距离场是 **R8_UNORM linear data**，与灰度 coverage 分开标记，不经过 sRGB transfer。固定版本的 SDF bitmap num_grays=255，字节编码仍覆盖 0..255；轮廓零点在 byte 128，不能直接用 normalized 0.5 代替。

```text
source_distance = (sample * 255 - 128) * spread / 128
aa = max(fwidth(source_distance), epsilon)
fill = saturate(0.5 + source_distance / aa)
outer = saturate(0.5 + (source_distance + stroke) / aa)
premultiplied_output = fill_color * fill + outline_color * (outer - fill)
```

正距离表示内部。shader 的描边宽度以源 texel 表示，颜色转为 linear premultiplied 后合成，父 opacity 再统一应用。当前探针限制 stroke < spread - 1，避免越出已存储的距离范围；更大的模糊/描边需要更大的 field/padding，不能仅改 shader 参数。

探针使用一页 256×128 atlas、单层纹理、每个 cell 一 texel 零值 guard，显式 SampleLevel(...,0)，不生成普通颜色 mipmap。相邻 guard 外加入白色 poison 区域，检查取样边界。它只是固定测试资源，尚无动态缓存、LRU、atlas 扩页或在线搬移。

小字号正文仍优先使用原有灰度 coverage。SDF 是独立可选材质，适用于本轮已测试的缩放和描边场景。未来选择策略由 renderer 配置决定，保持 CSS font/布局语义；缓存 key 必须包含字体 asset/version、glyph/variation、raster mode、base ppem、spread 与 generator。逻辑塑形缓存与 DPR/raster 缓存分开。FramePacket 继续保存逻辑 glyph 和字体 lease，生产 atlas UV 只在 renderer Prepare 时解析，不能让 DOM 或布局依赖 atlas 坐标。

## 已执行的验证

| 检查 | 本机结果 |
| --- | --- |
| CPU Ahem 独立方形几何 | 40 ppem → 56×56 bitmap，8 texel padding；横截面距离最大误差 0.5 源 texel，包含最外侧饱和值 |
| CPU 合同反例 | 内外符号、spread 切换/重置、Outline generator、空格、advance/灰度栅格不受影响、非法范围/glyph/source 与非 owner 调用拒绝 |
| 真实 Roboto | “SDF Ag” 塑形/距离场；GPU 同一份 “SDF” field 绘制 24/48 ppem |
| GPU 方形缩放 | 同一 field，0.5×/1×/1.5×，位置含 0.25 px 偏移；7,811 个独立 area-coverage 样本，432 个部分覆盖像素 |
| GPU 直边误差 | linear coverage 最大差异：RTX 5090 0.056129，WARP 0.063315；阈值 0.12 |
| GPU 描边 | 2 源 texel 的蓝色描边，直边 linear coverage 最大差异 0.034307 |
| GPU 提交与退休 | 复用真实 command list/query 合同，取消、录制失败、CPU 提前释放、host serial 43 确认完成后退休；direct/RTT 各一份 atlas |
| GPU 全图 parity | Debug/Release、硬件/WARP 的 direct/RTT 最大差异均为 0 个 8-bit 色阶 |
| GPU 调试层 | 两种 driver、两种配置均启用，warning/error 为零 |
| 完整回归 | Debug 15/15（4.13 s），Release 15/15（3.56 s）；D3、Flex 与原有颜色合同继续通过 |

GPU 的 55,545 项断言包含大量逐像素检查，不代表 55,545 个独立功能。直边 oracle 排除了双边同时接近的角区域，**没有证明尖角/曲线的完整像素质量**；Roboto 的可见性与截图不等于完整字形差分验收。测试无旋转、透视、极端 minification、自动 mipmap 或中文字段。本轮没有采集 SDF 浏览器 golden，也没有测量性能预算。

Roboto Regular 2.138 已包含于固定 HarfBuzz perf/fonts，未复制或修改字体。字体 name table 指定 Apache-2.0，许可正文和独立归属保存在 `third_party/probe-font-notices`，同时固定其 hash；HarfBuzz 的代码许可不替代字体许可。Ahem 仍只用于确定性几何测试。

## 复现与产物

```powershell
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
.\build\windows-msvc\bin\Debug\ce_d3d11_probe.exe --sdf-hardware artifacts/local/sdf-debug/hardware
.\build\windows-msvc\bin\Debug\ce_d3d11_probe.exe --sdf-warp artifacts/local/sdf-debug/warp
cmake --build --preset windows-release --parallel 4
ctest --preset windows-release
.\build\windows-msvc\bin\Release\ce_d3d11_probe.exe --sdf-hardware artifacts/local/sdf-release/hardware
```

每次 GPU 运行产生 report.json、direct.png、rtt.png 和 distance-atlas.png。最后一张展示的是距离字节，不能作为 coverage 截图解读。CTest 自动 WARP 产物在 `build/windows-msvc/artifacts/sdf-Debug/warp` 与 `sdf-Release/warp`；硬件产物、最终 CTest 日志和代码/报告 hash 元数据在 `artifacts/local/sdf-debug`、`sdf-release`。生成产物被 Git 忽略。

剩余工作：把 SDF/coverage 材质接入正式 GlyphRun→Prepare→atlas 路径与预算；固定中英文字体和 Unicode 规则；完成 Android/iOS 后端实际编译及真机验证。MSDF 是后续质量选项，尚无其生成器、RGB distance encoding 或 shader 验收。
