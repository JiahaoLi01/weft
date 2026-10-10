# 原生 React 窗口与鼠标

本文保留早期 Flex-only 主鼠标窗口的范围与历史证据。后续 [flow 输入窗口](flow-input-window-probe.md) 使用原样 CaseCheetah React 和新的 flow hit regions，接入 HWND 滚轮、圆角/inline 裁剪与滚动重绘；可见和隐藏运行命令分别记录。

2026-10-07，React/Vite 页面已接入 Windows HWND、D3D11 flip swapchain 和主鼠标消息。Add one 更新实际 React state，Reverse list 重排原生节点并重新绘制。应用 JSX 和原版 React bundle 不变，exe 不依赖浏览器或 Vite server。

## 运行

从项目根目录运行：

```powershell
.\build\windows-msvc\bin\Release\ce_d3d11_probe.exe --react-window-hardware artifacts/local/window-interactive
```

程序打开 CaseElephant 原生窗口，可点两个按钮并拖动窗口边缘调整尺寸，关闭退出。显式 WARP 使用 --react-window-warp。启动保存 window-0.png，关闭后写 report.json。普通 CMake 构建不需要 Node/npm。

## 实际链路

[Win32 宿主](../probes/react_window_host.hpp) 的 WindowProc 只排队输入值与窗口元数据，不执行 JS、layout 或 GPU。队列最多 1,024 条，连续 move 可合并；overflow 停止样例。关闭请求延后到 owner 安全点，避免 Present 重入时销毁 HWND。

[page adapter](../src/layout/page_probe.cpp) 输出自有布局、paint 与 hit 列表。命中项仅含弱代际身份、parent 索引和逻辑盒，不含 Node/JSValue。当前限定静态 Flex，无 positioned stacking、z-index、clip；不支持的声明报错。透明 box 也可命中，按已支持 paint 顺序逆向选择，边界为半开区间。

[mouse router](../src/input/input_probe.cpp) 每次命中前 flush 当前布局。mousedown 保存弱身份；mouseup 派发后重读当前树，再以 down/up 最近共同祖先为 click 目标。disabled 禁止激活；display:none 排除子树；pointer-events 支持 auto/none 的继承与后代覆盖。移除节点或 View reset 不向旧目标派发 click。[鼠标序列参考](https://www.w3.org/TR/2015/WD-uievents-20151215/#events-mouseevent-event-order)

平台 SetCapture 用于收到窗口外的 release；这不等于 DOM setPointerCapture。WM_CAPTURECHANGED、WM_CANCELMODE、失去 OS 焦点、最小化和关闭取消 press。按下按钮、在页面外松开不激活按钮；在另一个元素松开时只 click 共同祖先。client 坐标使用实际视口的物理 → 逻辑变换，负坐标保留。[Win32 mouse message](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-lbuttondown)

桥接按 slot/generation/View epoch 直接派发真实 UIEvent/MouseEvent，保留坐标、modifier、timestamp、冒泡/取消、GC roots 和微任务检查点。宿主事件的只读 isTrusted=true，JS new MouseEvent 为 false；它区别创建者，不能据此声称实体鼠标实测。当前 primary single-click profile 不包含完整 hover/dblclick、多按钮、wheel、PointerEvent、DOM 焦点、键盘、IME 或触摸。screenX/Y 目前保留 Win32 physical screen 单位，完整浏览器 screen-coordinate 兼容保持未验证；client 坐标用于本次实际命中。

## GPU 与 resize

持续使用同一个 D3D11 device。glyph 经 prepare/encode/真实提交协议绘制到固定 640 × 420 目标，GPU quad 再合成到 swapchain 的 sRGB RTV。显示不通过 CPU 像素上传；PNG 为诊断 backbuffer readback。

随后此窗口路径已迁移到 [独立 backend C ABI](render-backend-api.md)：适配器接收宿主 deferred context/RTV 并返回 command list，宿主仍负责实际提交、合成/present 和真实 query 通知。页面、布局和命中不绑定 D3D11 类型；具体 Vulkan/Metal adapter 尚未实现。

共享队列 serial 覆盖 UI 与宿主合成。每次提交只 End 一次 completion marker，实际 query 完成后才释放 glyph。若 post-submit 的宿主步骤抛错，scope guard 仍写 marker，先 drain 再释放 backbuffer/views。窗口消失不冒充 device abandon。

resize 在 owner 安全点 ClearState、释放旧 backbuffer/views，再 ResizeBuffers。尺寸为零时暂停绘制，恢复后重建目标。WM_DPICHANGED 使用系统建议 rectangle；真实多显示器 DPI 切换未验收。[ResizeBuffers 引用合同](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers)、[Present1](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiswapchain1-present1)

固定逻辑页以等比 fit/letterbox 展示，输入保存消息接收时的相同变换。resize 尚非响应式 CSS，也没有按新 DPR 重栅格字体；非整数倍率的灰度字体质量未验收。每次绘制阻塞等待完成并重建 glyph texture，静态 DOM 不重复绘制；这是正确性参考路径，生产 atlas、异步 renderer 和性能预算未交付。

## 验证与产物

mouse_input_contract 覆盖真实 React click、paint order/边界、letterbox/零尺寸、modifier/GC、取消/disabled/hidden、共同祖先、capture/bubble、pointer-events、detached/epoch。

d3d11_warp_window 创建测试专属隐藏 HWND，PostMessage 到自己的窗口，经过同一 WindowProc → owner → hit → React 路径。真实创建 swapchain、ResizeBuffers、Present1，并读 actual backbuffer；关闭后 unmount/reset、drain。physical_mouse_verified 保持 false，实体鼠标需在已准备的可见窗口实测。

```powershell
ctest --preset windows-debug
ctest --preset windows-release
.\build\windows-msvc\bin\Debug\ce_d3d11_probe.exe --react-window-test-hardware artifacts/local/window-debug/hardware
```

Debug/Release hardware/WARP 均完成 **7 帧、7 次 Present、3 次 resize、14 条 mouse message**，最终 Count: 2、顺序 moss/tide/ember；每会话退休 **605 个 glyph texture**，completed serial 为 **14**。单独的 post-submit failure 也实际 drain，调试层 warning/error 为零。全量与强制 GC 结果见 [status](status.md)。

报告/PNG 在 artifacts/local/window-debug、window-release、window-gc-stress；自动 WARP 输出在 build/windows-msvc/artifacts/window-Debug/warp 或 window-Release/warp。window-0/1/2 是初始/计数/反转；window-3/4 是宽窗口，window-6 是窄窗口。无 PNG 的 frame 也计入实际提交与完成。

HTML parser、CSS 样式表、键盘/DOM focus/IME、触摸、SVG、SDF 页面材质、device removal 和移动后端仍待接入。本阶段只交付受限 native mouse window，不代表完整网页运行时。
