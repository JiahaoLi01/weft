# 3D 游戏内 UI（导出纹理扩展）

除了将界面直接合成到屏幕后缓冲（HUD、菜单）与将引擎纹理导入 UI（`-weft-texture`），Weft 提供对称的**导出纹理扩展**（[`include/weft/render_export_texture.h`](../include/weft/render_export_texture.h)），用于将 UI 渲染输出为标准 GPU 纹理，供引擎作为普通 3D 材质贴附于世界空间中的电脑终端、全息面板、战术平板、载具仪表盘或角色装备表面（Diegetic UI）。

## 1. 为什么使用导出纹理扩展

与宿主手动创建一张临时渲染目标并直接调用 `weft_renderer_encode` 相比，导出纹理扩展将 3D 世界空间 UI 的四项核心工程问题统一收口至渲染器内部：

1. **正确的线性预乘色彩输出**：屏幕空间 UI 遵循浏览器 sRGB 编码预乘合成契约，而 3D 场景通常在线性 HDR 空间执行光照与透明混合。导出扩展自动完成至**线性预乘 RGBA** 的转换，避免贴附到 3D 场景后出现暗化或半透明边缘发黑。
2. **自动生成完整 Mipmap 链**：世界空间面板随相机距离与倾角变化容易产生严重走样与闪烁；导出扩展在同一命令缓冲内自动生成预乘正确的低级 Mipmap。
3. **预分配环形缓冲与显式读写同步**：创建导出对象时按配置的环大小（1–8 张图像）一次性分配全部显存，运行期零显存分配，并通过提交序号严格避免 UI 写入与上一帧 3D 采样发生数据竞争。
4. **按需重绘（零空转开销）**：当 3D 面板内容未发生变化时无需每帧重绘，3D 渲染管线继续以 60–240 FPS 免费采样上一份有效纹理租约即可。

## 2. 与外部纹理导入的对称契约

| 维度 | 外部纹理导入（Engine → UI） | 导出纹理扩展（UI → Engine） |
| --- | --- | --- |
| **资源归属** | 宿主引擎创建与销毁 | Weft 渲染器预分配与销毁 |
| **租约方向** | 宿主借给 Weft 采样 | Weft 借给宿主 3D 管线采样 |
| **回收时机** | 最后一次 UI 录制提交完成 → 触发 `retired` 回调 | 宿主归还租约并提交最后采样序号 → 该序号完成且写入完成后回归空闲环 |
| **采样色彩空间** | 线性预乘 RGBA | 线性预乘 RGBA（硬件采样即可直接以 `One, OneMinusSrcAlpha` 混合） |
| **队列同步** | 同设备同队列，引擎写入排在 UI 录制之前 | 同设备同队列，引擎 3D 采样排在 UI 导出提交之后 |

## 3. C ABI 接入流程

```c
// 1. 初始化阶段：安装导出扩展并创建预分配纹理环
weft_render_export_backend export_table = {0};
weft_render_metal_export(metal_backend, &export_table);
weft_renderer_install_export_backend(renderer, &export_table);

weft_render_export_desc desc = {
    .struct_size = sizeof(desc),
    .abi_version = WEFT_RENDER_EXPORT_ABI_VERSION,
    .identity = renderer_info.identity,
    .width = 1024,
    .height = 512,
    .format = WEFT_RENDER_TARGET_SRGB_RGBA8, // 推荐：硬件解码至线性空间
    .mip_levels = 0,                         // 0 表示生成完整 Mip 链
    .images = 3,                             // 三重环形缓冲（1 ~ 8）
};
weft_render_handle panel_export = {0};
weft_renderer_create_export(renderer, &desc, &panel_export);

// 2. 仅在 View 内容有更新时重绘导出纹理
weft_render_handle use = {0};
weft_render_export_image exported = {0};
weft_render_result rc = weft_renderer_encode_export(renderer, prepared, panel_export, &pass, &use, &exported);
if (rc == WEFT_RENDER_OK) {
    // 宿主将 exported.native_texture 绑定到 3D 材质，并归还上一张旧租约
    weft_renderer_release_export_image(renderer, prev_image.image, queue_id, last_3d_sample_serial);
    prev_image = exported;
    // 提交命令缓冲后照常调用 weft_renderer_notify_submitted(renderer, use, queue_id, serial)
} else if (rc == WEFT_RENDER_BUSY) {
    // 所有环槽均在途时保持显示 prev_image，下一帧重试；prepared 帧仍保持有效
}
```

### 格式与坐标约定

- **推荐格式**：`WEFT_RENDER_TARGET_SRGB_RGBA8` / `WEFT_RENDER_TARGET_SRGB_BGRA8`（8 位高带宽效率，硬件采样自动转线性）或 `WEFT_RENDER_TARGET_LINEAR_RGBA16F`（高精度 HDR 管线）。
- **纹理坐标**：UV `(0, 0)` 对应页面左上角，与 Direct3D、Vulkan 及 Metal 纹理坐标系一致。
- **射线交互**：引擎对 3D 面板网格执行射线检测得到表面 UV `(u, v)` 后，乘以对应 View 的逻辑宽高 `(u * logical_width, v * logical_height)` 并直接注入 `weft_view_pointer` / `weft_view_hit_test`，即可实现与屏幕空间完全一致的悬停、点击、滚动与拖拽交互。

> 完整可运行的 3D 游戏内 UI 宿主示例（含射线点击与性能基准模式）见 [`samples/macos-ingame`](../samples/macos-ingame)。当前导出纹理扩展已在 Metal 后端完整实现，D3D11 / D3D12 / Vulkan 后端遵循同一套公共 C ABI 与状态机扩展接入。
