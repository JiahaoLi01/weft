# 中文回退与原样 AlertBar

后续已独立增加 [公共样式与原始主题阶段](style-cascade-probe.md)，保持本页的 bundle/golden 和历史证据不变。新 workload 接原始 theme/expanded rule，完整 operations.css 仍待实现。

2026-10-07：沿用固定 CaseCheetah 样本，增加 C++20 字体集合和中文灰度绘制，原样 AlertBar 与 Zustand/store/UE 桥在自己的 DOM 和 D3D11 backend API 上运行。原游戏工程保持只读；本轮没有再次捕获或改写样本。

## 实际链路

[AlertBar.tsx](../tests/fixtures/casecheetah/UI/src/shell/AlertBar.tsx)、[ui-store.ts](../tests/fixtures/casecheetah/UI/src/stores/ui-store.ts)、focus.ts、ue-bridge.ts、NineSlice.tsx 均保持捕获时的字节。Zustand 5.0.15 是原锁文件同版本的完整原包，使用其真实 ESM React/vanilla store，而非测试替身。React 19.3.0/Vite 8.3.3 仍采用 Weft 的既有固定运行时，区别于游戏的 19.2.8/6.4.3。

[测试外壳](../examples/cheetah-alert/src/main.tsx) 提供固定仓库/公司数据、13 项分支和捕获型 `window.ue.sendEvent`；它只保存 JSON，不调用真实 Unreal。选项触发原始 openModal → focusOn → sendToUE，实际 Zustand dossier/dock 状态更新。另一按钮使用原始 `window.__ue_dispatch('setQuote', data)`，回执经真实 handler 进入 store 并使 React 重绘。这个接收端尚未绑定 C++ 游戏服务。

样式是明确标注的 [preview CSS](../examples/cheetah-alert/src/layout.css)：显式 block/absolute 尺寸、单行文本与矩形底色，并保留原五条 NineSlice 规则和原始 card PNG。**没有运行游戏 operations.css**；其 auto Flex、variables、font shorthand、translateX、圆角和多行列表仍需扩展。新样例不是原游戏布局或逐像素复刻。

## 字体与所有权

[FontSet](../include/caseelephant/text_probe.hpp) 按顺序持有固定 Roboto 和 Noto Sans CJK SC Regular。Noto Sans2.004 来自官方 commit `523d033d6cb47f4a80c58a35753646f5c3608a78`，OTF 为 16,437,364 字节，SHA-256 与 SIL OFL 1.1 原文随源码保存。它是可打包的 UI sans 候选，不等同于游戏的宋体/系统字体。正常构建不查询系统字体、不联网。

当前范围是 LTR Latin/Han/Common/Inherited。UTF-8 解码和 cluster 来自固定 HarfBuzz；主字体缺字时为整个 cluster 选择后续字体，再合并相邻同字体段重塑形，保留英文连字、源 UTF-8 cluster 与每 glyph 的 font_id。组合字符、缺字、非法 UTF-8、其他 script/bidi 控制、字号/face/owner 反例有测试。HarfBuzz cluster 不是通用编辑 grapheme；未实现 Unicode 段落分析、换行、RTL、任意脚本、语言配置、caret 或 IME。

font_id 是同一不可变字体集合内的索引，不是跨集合的全局句柄。owner 在字体集合仍存活时栅格化自己的 paint snapshot，之后 FramePacket 只含不可变像素和值，GPU 不访问字体对象或 DOM。生产资源服务仍需全局 resource identity/version、持久缓存与异步载入。集合限制 1–8 个 face、总输入 64 MiB、单 face 32 MiB、文本 64 KiB；没有测量峰值内存或移动预算。

小字号使用灰度 coverage；现有单字体 SDF 探针继续通过。CJK SDF、回退字体 SDF 页面材质、生产 atlas/subset 和移动端未验收。Probe 字形缓存已包含 font_id/glyph_id/字号，另用两个字体的同一 glyph ID、不同真实 bitmap 验证不会串用缓存，并实际送 GPU。

positioned profile 新增 span/b/ul/li 和显式 normal font-style、400 weight、list-style:none；i/b 的 UA 字体默认不能被普通继承悄悄覆盖，ul 必须显式清零 margin/padding。列表仍是明确高度的 block。显式单行行高使用主字体 strut，fallback 不单独移动基线；还不是一般 inline line-box 算法。

## 验证证据

八帧分别为关闭、展开、选仓库、再展开、选公司、切换 13 项、展开截取 12 项与“另有 1 项”、接收 UE 回执。头按钮身份保持；卸载的列表 target 被拒绝。卸载/reset/GC 后仍能从值快照输出全部帧。另有一帧独立 font ID 缓存碰撞场景。

Debug/Release 的硬件 RTX 5090 和 WARP 都通过。每次 568 个 fallback glyph、217,065 个独立 CPU bilinear/coverage oracle 样本（包含可能重复的 glyph quad 像素访问），最大差异 1 色阶；788 个图片/字形 texture 在实际 completion query 后退休，D3D11 debug layer warning/error 为零。GPU quad 本身不包含 JS/Node 指针。

独立浏览器通过真实按钮点击采集八帧；[固定参考](../tests/fixtures/cheetah_alert_browser_reference.json) 包含来源 hash，不是从 native 输出生成。状态、两个 focus payload 和 store 回执完全一致。1,336 个几何字段最大差异 0.151123 CSS px（阈值 0.2）；四个 16px 文本的 advance 最大差异 0.013681（阈值 0.1）。主字体浮点 strut 与浏览器设备像素量化的 baseline 差异为 0.441648（声明阈值 0.5），纯英文与中文混排相同。浏览器 DPR 1.005；没有宣称浏览器像素相等或一般字体度量均已验收。

最终全量与强制 GC 结果、运行时间及 binary/source/output hash 在 [status](status.md) 和 `artifacts/local/cheetah-alert-{debug,release,gc-stress}/run-metadata.json`。本轮 GC 只覆盖新增 workload、字体、positioned profile 与固定输入/对照五项，不宣称全量 32 项 GC。

## 复现

~~~powershell
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
build/windows-msvc/bin/Debug/weft_d3d11_probe.exe --cheetah-alert-hardware artifacts/local/cheetah-alert-debug/hardware
python tools/compare_cheetah_alert.py tests/fixtures/cheetah_alert_browser_reference.json artifacts/local/cheetah-alert-debug/hardware/report.json --output artifacts/local/cheetah-alert-debug/hardware/browser-comparison.json
~~~

软件入口为 `--cheetah-alert-warp`；生成 report.json、native-dom.json 和 frame-0..7.png。它是隐藏 HWND 的实际 Present/readback 探针，直接按弱 target 派发事件；没有通用 positioned hit-test、实体鼠标或键盘验收。

普通 CMake/CTest 直接使用固定 native IIFE，不需要 Node。重建时运行 `node tools/build_cheetah_alert.mjs`，仅写自己的 public/dist/native/profile.css，随后 `python tools/verify_cheetah_alert.py` 验证来源与 notices。修改输入后要重新采集浏览器 golden，不得按 native 数值刷新它。浏览器预览：

~~~powershell
python -m http.server 4175 --bind 127.0.0.1 --directory examples/cheetah-alert/dist/browser
~~~

## 下一步

组合 author styles 与 Flex/block/inline，逐步解除 AlertBar 预览 CSS 的显式尺寸适配，再加入滚动、圆角与样式变量，扩大左右档案/视图条 workload。真实素材需要持久图片/字体缓存：本样例上传一个 6 MiB 解码 card，reference 转成约 24 MiB RGBA32F texture，host 仍解码 allowlist 的三张图共 18 MiB；每帧全量上传和完整 16 MiB OTF 不能当作产品成本。D3 SVG、完整文档加载、同步 geometry、IME、Vulkan/Metal 和移动真机继续分别验收。

后续自动布局阶段已提供独立新 profile/workload，将固定 AlertBar 内部位置与列表高度改为内容计算；本篇保留旧阶段证据，不覆盖新范围。见 [自动布局实际合同](automatic-layout-probe.md)。
