# 公共样式求值与原始主题 CSS 探针

> 2026-10-11 重构第 8 步：positioned、flex-page、auto 三个样式 profile 已合并为 `document_style_profile()`，属性表、UA 样式表与标志逐项相同；样式 fixture 的 192 个颜色分量对照不变。见 [重构记录](refactoring-plan.md#第-8-步记录2026-10-11)。

本阶段把 positioned 和 Flex 页面中的重复样式逻辑抽到 `weft::style`。固定 AlertBar 使用原样 React/Zustand/store/UE bundle、`operations.css` 的原始 `:root` 块和原始展开状态选择器，输出八帧中文 D3D11 像素。几何与字体仍使用作者适配；没有执行整个 `operations.css`，没有修改实时 CaseCheetah 工程。

## 模块合同

入口见 [style_probe.hpp](../src/style/style.hpp)，实现见 [style_probe.cpp](../src/style/style.cpp)。输入是自有值 DOM snapshot、作者 stylesheet 和属性 profile，输出是自有值样式树。模块不链接 Yoga、字体、QuickJS 或图形 API，不调用 JS、不测量文字、不保存 Node 指针。DOM 类型头文件只用于值身份和快照。

两种页面 adapter 使用同一层叠、选择器与变量实现。`build_page(root, stylesheet, font)` 新增作者规则入口；原有 inline 入口保留。positioned adapter 新增配对样式树入口：在 `html` 快照上求值，裁取同一份快照中的 `#gallery` 和对应样式，再只格式化 gallery。`:root` 不把任意 scoped div 当作文档根。

样式与 DOM 的配对检查验证 identity、generation、epoch、local name、元素子树数量和顺序。调用方必须传递同一版不可变值快照；身份检查不代替样式 revision/失效系统。此处的稀疏输出是 formatting profile 的输入，不是完整 CSS computed-style 表，也没有提供 JS `getComputedStyle`。

## 实际支持

| 能力 | 本阶段行为 |
| --- | --- |
| 层叠 | 固定 UA、author、inline attribute，`!important`、specificity、同权重 source/declaration order |
| 选择器 | HTML type/id/class compounds、通配、后代/子代、`:root`、属性存在和精确值；后代匹配会回溯祖先 |
| 自定义属性 | ASCII `--name`、区分大小写、继承、重复声明、重要性、有效空值、guaranteed-invalid 值 |
| `var()` | 保留 token 边界，嵌套/逗号/空回退，先在父元素计算再继承，选中分支的循环检测 |
| computed-value 失效 | 缺失变量与支持类型内错误在胜出声明上执行 unset，不重新层叠较早声明；输出普通属性失效诊断列表 |
| CSS-wide | initial/inherit/unset；变量替换后得到该关键字时也处理，包含自定义属性 |
| 颜色 | `#rrggbb`、transparent、legacy 逗号 rgb/rgba 数值分量；颜色和 alpha clamp |

例如 `--n:16; width:var(--n)px` 保留为 number + ident，不能被拼成 16px。父级 `--b:var(--a)` 已计算后被继承；子级覆盖 `--a` 不会重新解释继承的 `--b`。

循环采用当前编辑草案的 substitution-context 行为：未选中的回退不求值；实际循环中的参与属性均失效，即使循环路径内存在有效回退。这里有一个标准版本差异：旧发布版按静态依赖图计入未使用回退，本次以 Chromium 实测、[当前 Variables 编辑草案](https://drafts.csswg.org/css-variables-1/)、[Values 5 的 substitution context](https://drafts.csswg.org/css-values-5/#substitute-arbitrary-substitution-function) 和 [WPT cycles](https://github.com/web-platform-tests/wpt/blob/master/css/css-variables/variable-cycles.html) 的现行行为为依据；没有宣称完成全部 WPT。

## 边界与资源限制

这是有界 CSS profile，不是通用 CSS parser。ASCII 语法、单值 inset/box-edge/gap、px/% 与受限 calc；不支持转义、Unicode CSS 标识符、非矩形 transform、嵌套规则、at-rule、cascade rollback/layers、复杂 selector、动画或注册自定义属性。quoted 字符串与函数 token 可保留，但不因此获得 URL、font shorthand 等使用能力。DOM querySelector 仍是旧 compound 子集，stylesheet 的属性/组合选择器没有同步扩展 DOM query API。

属性元数据规定初始值、继承、支持类型。font-family/list-style/flex 等仍把 token 值交给格式化 profile 验证；不是完整属性 grammar registry。支持范围外的单位、颜色形式、多值 shorthand 或格式化行为有显式诊断，不把有效但未支持的值偷偷降为零。stylesheet 中未知属性即使规则不匹配也会诊断。语法错误采用严格 probe diagnostic，不是浏览器完整的错误恢复。

上限：stylesheet 64 KiB/256 selector rules，单声明块 8 KiB，单值 4,096 tokens/8 KiB，函数深度 32，样式树 2,048 元素/64 层，每元素 128 个继承后的 custom properties，全树 1,000,000 个 token 工作单位和 16 MiB 输出字符串。继承复制和展开也计入工作预算；超限直接诊断。预算是探针保护，不是性能验收数据。

两种 formatting context 仍独立，没有 Flex/block/inline dispatcher。省略属性沿用已有探针默认值；显式 CSS-wide 初始值如果需要尚未支持的格式化行为，会诊断。Bootstrap 的 CSSOM `setProperty(...,'important')` 尚未支持，不能从 attribute 中的 important 求值推断该 JS API 已实现。

## 原始组件与证据

[新 workload](../examples/cheetah-style/profile.css) 由 [构建工具](../tools/build_cheetah_style.py) 从固定样本组合：保持此前 native IIFE 完全一致，精确保留原始 `:root` 变量块和 `.alert-chip > button[aria-expanded="true"]` 规则。字体继续使用 ProbeRoboto/ProbeCjk，图片继续使用捕获的卡片；作者 override 保持明确尺寸，并使用主题变量、原始 rgba 数值。

旧 AlertBar 的 bundle、golden 和历史产物未替换。八帧场景仍为关闭、展开、选 facility、再展开、选 company、切到 13 项、展开 12 项加通知、接收 UE 示例回执。浏览器实际点击控件；native 使用 epoch/代际身份派发，卸载/reset/GC 后才绘制自有值快照。桥捕获本地协议数据，没有执行 Unreal 游戏动作。

独立 JS/CSS 语义 workload 有 16 个 case，真实浏览器主题/属性按钮更新后的三帧与 native 比较 **192 个颜色分量**，最大差异 0.0000004314，阈值 0.00001。包括空值、缺失回退、已计算继承、选中/未选中循环、替换后 CSS-wide、错误类型和 rgba。

AlertBar 八帧比较 **2,672 个颜色分量**，最大差异 0.0000003922；**1,336 个几何字段**，最大差异 0.151123 CSS px，阈值 0.2。状态与桥协议完全一致，浏览器 Roboto/CJK 均已加载。RTX 5090/WARP 的 Debug/Release 和 GC WARP 各完成 217,065 个独立 CPU/GPU 像素检查、568 个回退字形、788 个 texture 退休；WARP 最大差异 1、硬件 2 个 8-bit 色阶，调试层 warning/error 为零。浏览器逐像素一致未验收，仍有既定的 linear-sRGB 合成差异。

最终 Debug **37/37，11.55 秒**、Release **37/37，7.76 秒**、新增范围加八轮 React 强制 GC **7/7，40.36 秒**。测试结果与来源/hash 封存在 `artifacts/local/style-{debug,release,gc-stress}/run-metadata.json`；GPU PNG/report 位于 `artifacts/local/cheetah-style-*`；实际浏览器截图位于 `artifacts/local/cheetah-style-reference/`。这不是移动、完整游戏、全 Unicode 或性能验收。

## 复现

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-debug --parallel 10
ctest --preset windows-debug
cmake --build --preset windows-release --parallel 10
ctest --preset windows-release
build/windows-msvc/bin/Debug/weft_d3d11_probe.exe --cheetah-style-hardware artifacts/local/cheetah-style-debug/hardware
build/windows-msvc/bin/Release/weft_d3d11_probe.exe --cheetah-style-hardware artifacts/local/cheetah-style-release/hardware
```

有 GC build 时：`cmake --build build/windows-gc-stress --config Debug --parallel 10`，再运行 `ctest --test-dir build/windows-gc-stress -C Debug -R 'style|positioned_css|page_react' --output-on-failure`。强制 GC 包含八次 React mount/update/unmount/reset，执行预算仍使用既有压力配置。

本地浏览器：`python tools/build_cheetah_style.py` 准备自己的静态副本，然后 `python -m http.server 4177 --bind 127.0.0.1 --directory examples/cheetah-style`，访问 `http://127.0.0.1:4177/`。没有 npm 安装或 live game 构建。正常 native CMake/CTest 直接用固定 bundle/字体/图片，不依赖这些忽略的浏览器 asset 副本；副本存在时 integrity test 会验证每个 hash。改变 fixture 必须重新执行浏览器采集，不能只刷新 golden hash。

下一步把共享样式输入接到格式上下文调度和 intrinsic 尺寸，先解除 AlertBar 的手写位置/高度，再逐项接 font shorthand、padding、transform、圆角和 overflow。

后续自动布局阶段已提供独立新 profile/workload，将固定 AlertBar 内部位置与列表高度改为内容计算；本篇保留旧阶段证据，不覆盖新范围。见 [自动布局实际合同](automatic-layout-probe.md)。
