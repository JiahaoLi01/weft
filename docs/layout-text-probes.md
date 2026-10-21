# M0 Flex 与字体探针

> 2026-10-11 重构第 8.2 步：本文的 Flex adapter 成为文档与 scene 共用的 Yoga 适配器 `layout_boxes`。CssAdapter 与 RestrictedBlockAdapter 合并；绘制属性不再被拒绝，只有影响盒子却没有映射的属性报错；需要百分比边距等的 intrinsic 限制只在需要自动最小尺寸时报错；文字盒高度不再被可用高度截断。33 场景的 raw/css 报告与浏览器对照逐字节不变。见 [重构记录](refactoring-plan.md#第-8-步记录2026-10-11)。

2026 年 10 月 7 日：独立 C++20 Flex adapter 的 **33/33** 场景、**452 项**盒几何与文本指标，在 Debug 和 Release 均与实际 Chrome 参考结果一致。FreeType/HarfBuzz 的字体度量、塑形和字形 coverage 已实际执行。结果支持继续验证 Yoga，但尚未达到 M0 的完整布局与文本退出条件。

## 比较方法与范围

唯一输入是 [flex_layout_cases.json](../tests/fixtures/flex_layout_cases.json)。生成器产出 C++ 与浏览器 JS 两份输入，并嵌入同一个 SHA256；CTest 验证生成文件没有过期。浏览器页面使用实际 DOM/CSS、getBoundingClientRect 和 Range 行片段测量，等待随项目固定的 Ahem 字体加载完成。提取工具检查浏览器捕获的输入 hash，拒绝把旧捕获关联到新输入。

固定参考是 [flex_browser_reference.json](../tests/fixtures/flex_browser_reference.json)，本机 Chrome 文件版本为 154.0.8037.98，headless UA 为 Chrome/154.0.0.0。参考页面的外围 stage 宽 600 CSS px，字体默认为 20px/20px。比较 x、y、width、height、text_width、text_height 和 text_lines，节点与场景不得缺失或增添；行数要求完全一致，尺寸允许 0.025 CSS px。该阈值用于浏览器亚像素量化与 native 浮点差异，只适用于逻辑几何，不能用于证明像素或 GPU 颜色一致。没有忽略失败场景。

| 场景组 | 实际覆盖 |
| --- | --- |
| Flex 分配 | row/column、grow、加权 shrink、reverse、多行 wrap 与 gap |
| 自动最小尺寸 | 文本 min-content、显式 min:0、overflow:hidden、指定宽度/max 限制、嵌套容器、纵向换行与空盒收缩 |
| Basis | definite 百分比，indefinite column 的 0px、40px、0%、40%，padding 与 inline-row 回归 |
| 盒与定位 | content-box/border-box、padding/border、max-size 与 edges、百分比尺寸/inset、static/relative containing block |
| 文本 | 两种字号的 baseline、按最终宽度换行、零高盒内的溢出文本 |

额外 native 合同测试证明：Yoga 在固定宽高文本上不调用 measure 时，最终文本仍形成两行；重建时改变宽度与字体，行数/尺寸随之改变。它验证全量路径，没有增量失效或字体异步到达证据。

## Yoga 的取舍与补丁

固定 Yoga 3.2.1，UseWebDefaults=true、errata=None、pointScaleFactor=0。adapter 设置 CSS static/content-box 默认值，展开所支持的 flex shorthand，提供文本 measure/baseline 与受限 intrinsic prepass。自动最小尺寸计算结束后交给 Yoga 求盒几何；收集输出不再修改盒子。最终文本独立按求解出的 content-box 宽度排版。

最初的 24 场景中，未加 CSS 默认值/auto-min 适配的 baseline 模式通过 14/24，加适配后为 23/24。剩下的反例是 indefinite column 中 `flex-basis:0px` 被当作 content，错误地与 `0%` 合并。源码的一处条件修正使已解析 length basis 在 column 高度不确定时仍生效；未能解析的百分比继续采用 content。补丁限定 column，并保留 shrink-to-fit row intrinsic 的反例。

补丁位于 [0001-definite-flex-basis.patch](../third_party/yoga/patches/0001-definite-flex-basis.patch)。[metadata](../third_party/yoga/metadata.json) 记录原始 archive/文件、修改后文件和补丁 SHA256。只有 CalculateLayout.cpp 被修改，其余 vendored Yoga 文件与 archive 一致；补丁可从原始 archive 应用重现。尚未提交上游 issue，未来上游版本通过 length/percentage 和 row intrinsic 反例后删除本地补丁。

第二个补丁 [0002-fit-content-without-room.patch](../third_party/yoga/patches/0002-fit-content-without-room.patch)（2026-10-21，在第一个之后应用）：测量时，一个只需"放进可用尺寸"（FitContent）而可用尺寸不大于 0 的节点，Yoga 不排它的子节点，直接把它定为内边距的大小（`measureNodeWithFixedSize`）；可完整算法对不滚动的节点在 FitContent 下取的是内容的大小。于是高 0 的块里的块、高 0 的列 flex 里不能缩到内容以下的项目，都只剩内边距：宣传页 FAQ 的答案收起时（面板 `height: 0; overflow: hidden`）只量出 26 px，展开后只露一行。补丁让这条捷径只用于滚动容器，那时两者一致。浏览器反例：高 0 的块（overflow hidden 或 visible）里一个下内边距 26px、内有 40px 子块的块，Chrome 为 66 px；高 0 的列 flex 里同样的项目为 66 px，项目本身 overflow: hidden 时为 26 px（它的自动最小尺寸是 0）。`scene_contract` 检查这四种。上游修好这种情况后删除。

最终报告中的 `raw-yoga.json` 与 `css-adapter.json` **都使用同一个已打补丁版本**。raw 模式仅关闭 CSS position/box-sizing 默认值及自动最小尺寸适配，仍共用输入、样式解析和字体回调，不能解释为完全未经适配的 Yoga。最初未打补丁的 24 场景输入、浏览器参考和两份 baseline 报告独立保存在 artifacts/local/flex-native-debug 的 upstream-* 文件中。

## 真实字体基础

固定 FreeType 2.13.3 与 HarfBuzz 11.2.1。font adapter 持有不可变字体字节；FreeType face 由单个 owner 线程访问，HarfBuzz 每次调用使用独立 font/buffer，以 OpenType 度量和 26.6 scale 返回 glyph ID、UTF-8 cluster、advance 与 offset。FreeType 按独立字号栅格化无 hint 的灰度 coverage，输出拥有自身内存，不引用 face 内部 bitmap。

Ahem 是 WPT 的确定性测试字体，字体原始许可与 CC0 fallback 随项目保存。测试实际证明：20px 字体 ascent=16/descent=4；`XX XX` 有五个 glyph、总 advance=100；字号从 20→40→20 没有残留 scale；X 字形的 20×20 coverage 为全 255；40px 栅格变为 40×40。中文缺字显式标记，错误字号和跨 owner 调用被拒绝。

Ahem 不是产品字体，也不覆盖中文。HarfBuzz 的 UTF-8 cluster 不是 UTF-16 caret 或 grapheme 边界。布局探针没有 ICU、bidi 段落处理、Unicode 换行、字体回退、跨 span 空白或组合字符编辑，只处理 printable ASCII 按空格换行，其他文本显式诊断；不能由此宣布中英文文本系统完成。后续独立 GPU 探针已实现灰度上传与固定 SDF atlas，多倍率/描边的合同与质量边界见 [SDF 字体](sdf-font-probe.md)；尚未接入此布局管线或生产 atlas 管理。

后续已增加独立 FontSet 和 positioned 单行中文页面，固定 Roboto/Noto CJK 的回退、组合 cluster、英文连字、font ID 缓存与原样 AlertBar 通过。它尚未接入这里的 ASCII Flex 换行适配器，二者边界不可合并推断，见 [中文阶段](cjk-alert-probe.md)。

## 探针边界

输入是显式 flex/inline-flex 容器及独立文本/空叶节点，样式表是已选择值的 map。它没有 HTML parser、cascade、computed style、正常 block/inline 调度、viewport 或 DOM geometry API。shorthand 与对应 longhand 同时出现会诊断，声明顺序须由未来 CSS 层解决。

每次完全重建 Yoga 树，限 2,048 节点（13.4 起 16,384）、64 层和每节点 64 KiB 文本。未知属性、未支持单位、非法值、重复 ID 和混合 text/flex children 被拒绝。需要 intrinsic 计算的百分比 edges/gap，或无法确定纵向文本 auto-min 所需横向约束时，也显式拒绝。回调异常在 C 边界内部捕获，求解返回后重新抛出，不回调 JS。

Intrinsic prepass 是针对这些反例的受限实现；复杂嵌套、边距、跨轴 min/max、aspect-ratio、replaced element、auto width 容器与更多百分比组合仍需独立反例。已接受的属性名不等于该属性所有组合已验证。探针结果不进入完整兼容承诺；需要大规模猜测或第二份盒几何时仍按 [决策](decisions.md) 淘汰 Yoga。

## 复现

正常 CTest 使用已保存的独立浏览器参考，构建和测试不联网，也不要求每次安装 Chrome：

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
cmake --build --preset windows-release --parallel 4
ctest --preset windows-release
.\build\windows-msvc\bin\Debug\weft_flex_layout_probe.exe third_party/ahem/Ahem.ttf artifacts/local/flex-native-debug
python tools/compare_flex_reports.py tests/fixtures/flex_browser_reference.json artifacts/local/flex-native-debug/css-adapter.json --output artifacts/local/flex-native-debug/adapter-comparison.json
```

更改输入后先生成，再用实际浏览器捕获。下面是本机已经执行的命令，其他电脑需调整 Chrome 与 workspace 绝对路径：

```powershell
python tools/generate_flex_cases.py
New-Item -ItemType Directory -Path 'artifacts/local/flex-reference' -Force | Out-Null
$flexBrowser = Start-Process -FilePath 'C:\Program Files\Google\Chrome\Application\chrome.exe' -WindowStyle Hidden -ArgumentList @('--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check','--virtual-time-budget=5000','--user-data-dir=F:\Projects\CaseElephant\artifacts\local\flex-reference\profile','--dump-dom','file:///F:/Projects/CaseElephant/tests/fixtures/flex_layout_reference.html') -RedirectStandardOutput 'F:\Projects\CaseElephant\artifacts\local\flex-reference\browser-dom.html' -RedirectStandardError 'F:\Projects\CaseElephant\artifacts\local\flex-reference\browser-stderr.txt' -Wait -PassThru
if ($flexBrowser.ExitCode -ne 0) { throw 'Chrome capture failed' }
python tools/extract_flex_reference.py artifacts/local/flex-reference/browser-dom.html --output tests/fixtures/flex_browser_reference.json
python tools/generate_flex_cases.py --check
```

不要根据 native 结果修改浏览器参考。依赖 lock 与 fixture/reference 保存在源码中；完整构建开关、测试与平台状态见 [building](building.md) 和 [status](status.md)。独立的真实 GPU 提交/颜色 readback 已推进，见 [GPU 探针](gpu-probe.md)。下一步继续 Unicode/产品字体验证，再接通 HTML/CSS/layout/text 和 D3 SVG paint。
