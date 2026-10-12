# M0 D3D11 提交、颜色与 readback 探针

2026 年 10 月 7 日：C++20 独立探针在 **NVIDIA GeForce RTX 5090** 与 **WARP/Microsoft Basic Render Driver** 上，Debug、Release 都通过 **121 项断言**。两个设备都启用了 D3D11 调试层，没有 warning/error。已实际执行 command list、上传、混合、query completion、staging readback 与 PNG 导出。

这证明了固定 D3D11 场景的颜色合同与资源退休路径，不是完整页面渲染器。没有窗口、swapchain、DOM/CSS/layout 输入、D3 SVG paint、移动后端或性能测量。WARP 是实际 D3D11 软件执行；硬件结果单独记录，未由 WARP 推断。

## 验证链路

固定 scene 提供 GPU-free 值型快照、绘制顺序与不可变 CPU 资产，renderer 准备三个 native 输入纹理。它是实验内部类型，未成为公开 FramePacket API。Ahem 的 coverage 由真实 FreeType 生成后作为固定测试资产使用；尚未接通逻辑 GlyphRun → renderer raster/atlas 流程。

| 阶段 | 实际动作与反例 |
| --- | --- |
| Prepare | 创建 D3D11 texture/SRV，建立 ResourceLifetime 的完整依赖集合与 prepared pin |
| Encode | 先 EncodeFrame 预留 use pin/提交容量，再在 deferred context 录制上传与绘制 |
| 取消 | 丢弃完整 command list 后解除 use pin；未执行上传保持 Created |
| 录制失败 | 注入上传后的失败，完成并销毁部分 command list 后解除 pin；下一次独立录制包含全部上传 |
| CPU 释放 | prepared 与 CPU lease 在提交前释放，encoded pin 保留三个 native 纹理 |
| 宿主提交 | 宿主 immediate context 执行 command list；serial 42 在此后通知，录制成功不能代替提交 |
| 宿主状态 | ExecuteCommandList(TRUE) 后，先前 viewport/scissor 恢复，probe 检查实际返回值 |
| 在途与 shutdown | CPU/prepared 引用已为零，submitted pin 仍为一；Shutdown 返回 Busy，native registry 不释放纹理 |
| 完成 | 宿主追加两次 readback copy，End event query 对应共享 serial 43；GetData 实际返回完成后才通知并退休 |
| 退休 | command list 已释放，完成通知后 registry 释放 texture/SRV；三个资源均退休，Shutdown 返回 Ok |

两个路径各使用独立 device/实验实例，最终共退休六个输入纹理。该计数是我们 registry 的 COM 引用释放次数，不是 D3D driver 私有内存释放或泄漏的测量。shader、constant buffer、sampler、目标和 staging 由实验宿主/renderer 保持到提交完成，未按帧销毁。

使用 D3D11_QUERY_EVENT + immediate GetData 证明实际完成，未要求未完成状态一定能被 CPU 观察到。即使 GPU 很快完成，我们仍在正式 completion 通知前保留 pin。query 的 S_OK/TRUE 完成语义见 [Microsoft query 文档](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)。[ExecuteCommandList](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist) 与 [GetData](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata) 均在实际探针中调用。

显式 Flush、最多五秒等待和 readback 只属于诊断宿主。未来 renderer Encode 不能隐藏 wait/readback。真实 device removal/恢复、swapchain resize、生产线程队列、资源缓存驱逐和长时 GPU 压力尚未验证；当前实验不是可直接接入引擎的后端。

## 颜色结果

固定画幅 256×128，DPR 1。输入 CSS 颜色先 sRGB decode，再 premultiply；coverage 不做 transfer。两像素 straight-sRGB 图片逐 texel 转成 linear-premultiplied float 后再线性过滤，避免透明蓝色污染红色边缘。半透明输入的转换自 2026-10-10 起在 GPU 上进行（`srgb_convert.hlsl`，与原 CPU 表逐位相同，见 [交互性能](gallery-interaction-profile.md#拖拽卡顿与-canvas-帧率2026-10-10第二轮)）。没有接入产品 PNG decoder；这是固定资产反例。

Direct 绘制到 R8G8B8A8 typeless texture 的 sRGB RTV；RTT 先绘制到 linear RGBA32F，再整体合成到同样的 sRGB RTV。blend 为 ONE / INV_SRC_ALPHA，RGB 与 alpha 均使用 premultiplied 规则。sRGB 输出的线性混合与编码合同参考 [Microsoft output-merger](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage)，实际 readback 另做验证。

| 检查 | Native 预期 RGBA8 | 实际 Chrome 参考 RGBA8 |
| --- | --- | --- |
| 50% 白叠黑 | 188,188,188,255 | 128,128,128,255 |
| 50% 红后叠 50% 蓝 | 137,0,188,255 | 64,0,128,255 |
| #808080 不透明输入 | 128,128,128,255 | 128,128,128,255 |
| coverage=128/255 的白色 | 188,188,188,255 | 128,128,128,255 |
| 父 opacity=.5，内部不透明红后蓝 | 0,0,188,255 | 0,0,128,255 |
| 透明蓝与不透明红之间的过滤点 | 188,0,0,255 | 127,0,0,255 |
| 实际 FreeType X 字形中心/外侧 | 白/黑 | 白/黑 |
| scissor 内部/外部 | 红/黑 | 红/黑 |

十个独立像素检查的 RGB 容差为两个 8-bit 色阶，alpha 必须完全一致。透明 linear RGBA32F 目标另读取半透明绿色，direct/RTT 都是 `[0,0.5,0,0.5]`，每通道容差 1e-5；它检查未被不透明背景遮蔽的 premultiplied RGB/alpha。

Direct/RTT 对全部 32,768 个像素的四通道比较，最大差异：硬件为 **1**，WARP 为 **0**。这是小场景的数值一致性结果，不是视觉相似度或性能预算。

父 opacity 的红/蓝子元素在 probe 内固定为一组：先在透明 linear target 绘制两者，再给整体结果乘一次 opacity，证明没有误用逐子元素透明度。尚未实现通用分组 IR、任意嵌套 opacity、圆角/旋转 clip 或层叠树。

Chrome 文件版本为 154.0.8037.98。参考由独立 CSS/Canvas/Ahem 页面实际截图取得，固定 sRGB profile/DPR/窗口大小，检查 font loaded 与场景完成，并用 WIC 从 PNG 解码像素。[参考 JSON](../tests/fixtures/gpu_color_browser_reference.json)、[原始 PNG](../tests/fixtures/gpu_color_browser_reference.png) 和 DOM 捕获随源码保存，带 HTML/font/PNG/DOM SHA256。比较工具不会从 native 结果生成浏览器预期。

透明混合的差异是已选择的 linear-sRGB 产品边界。我们记录并验证各自合同，不把 Chrome 的 128 与 native 的约 188 当作逐像素一致。整个探针没有扩大浏览器颜色兼容承诺。

## 复现与产物

正常构建/CTest 不联网，无需重新运行 Chrome。固定 [HLSL](../src/render/shaders/scene.hlsl)（2026-10-11 起是共享的场景着色器，原 `quad.hlsl` 已删除）在构建时由 Windows SDK 的 fxc 编译并内嵌，运行时不再调用 D3DCompiler；WIC 只处理诊断 PNG。发布包的后端封装仍待定。

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
.\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --hardware artifacts/local/gpu-debug/hardware
python tools/compare_gpu_reports.py tests/fixtures/gpu_color_browser_reference.json artifacts/local/gpu-debug/hardware/report.json --output artifacts/local/gpu-debug/hardware/browser-comparison.json
cmake --build --preset windows-release --parallel 4
ctest --preset windows-release
.\build\windows-msvc\bin\Release\weft_d3d11_probe.exe --hardware artifacts/local/gpu-release/hardware
```

默认 CTest 注册 WARP 的真实执行与离线浏览器合同比较，硬件通过显式 --hardware 执行，不会静默降级到 WARP。启动尝试 D3D11 debug layer，SDK component 缺失时报告不可用；本机四次最终执行均启用成功。InfoQueue 若发现 warning/error，测试失败。

| 产物 | 位置 |
| --- | --- |
| Debug/Release WARP | build/windows-msvc/artifacts/gpu-Debug/warp/、gpu-Release/warp/ |
| Debug/Release RTX 5090 | artifacts/local/gpu-debug/hardware/、gpu-release/hardware/ |
| 每次输出 | report.json、direct.png、rtt.png；比较工具输出 browser-comparison.json |
| 最终 CTest 日志 | artifacts/local/gpu-debug/ctest-final.log、gpu-release/ctest-final.log |
| 浏览器原始运行与元数据 | artifacts/local/gpu-browser/；固定参考同时保存在 tests/fixtures/ |

重新捕获浏览器时，调整下面本机绝对路径，然后保留原始 PNG/DOM：

```powershell
New-Item -ItemType Directory -Path 'artifacts/local/gpu-browser' -Force | Out-Null
$colorBrowser = Start-Process -FilePath 'C:\Program Files\Google\Chrome\Application\chrome.exe' -WindowStyle Hidden -ArgumentList @('--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check','--hide-scrollbars','--force-color-profile=srgb','--force-device-scale-factor=1','--window-size=256,128','--virtual-time-budget=5000','--user-data-dir=F:\Projects\CaseElephant\artifacts\local\gpu-browser\profile','--dump-dom','--screenshot=F:\Projects\CaseElephant\artifacts\local\gpu-browser\reference.png','file:///F:/Projects/CaseElephant/tests/fixtures/gpu_color_reference.html') -RedirectStandardOutput 'F:\Projects\CaseElephant\artifacts\local\gpu-browser\browser-dom.html' -RedirectStandardError 'F:\Projects\CaseElephant\artifacts\local\gpu-browser\stderr.txt' -Wait -PassThru
if ($colorBrowser.ExitCode -ne 0) { throw 'Chrome capture failed' }
.\build\windows-msvc\bin\Debug\weft_d3d11_probe.exe --browser-png artifacts/local/gpu-browser/reference.png artifacts/local/gpu-browser/pixels.json
Copy-Item -LiteralPath 'artifacts/local/gpu-browser/reference.png' -Destination 'tests/fixtures/gpu_color_browser_reference.png'
Copy-Item -LiteralPath 'artifacts/local/gpu-browser/browser-dom.html' -Destination 'tests/fixtures/gpu_color_browser_reference.dom.html'
python tools/capture_gpu_reference.py artifacts/local/gpu-browser/pixels.json tests/fixtures/gpu_color_browser_reference.png --dom tests/fixtures/gpu_color_browser_reference.dom.html --browser-version 154.0.8037.98 --output tests/fixtures/gpu_color_browser_reference.json
```

本机硬件驱动为 32.0.16.1062，feature level 请求 11_0，MSVC 19.42.34444、Windows SDK 10.0.26100.0。没有 GPU timestamp、采样时段或预算数据；121 项断言与 CTest 耗时仅是正确性证据。下一步继续 Unicode/产品中英文字体和剩余 M0 反例，再串起真实页面与 SVG 绘制。全部状态见 [status](status.md)。
