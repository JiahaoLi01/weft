# 引擎集成与宿主通信指南

本文档介绍如何通过 [`include/weft/weft.h`](../include/weft/weft.h) 将 Weft 接入游戏引擎，涵盖运行时生命周期、引擎与页面双向通信、3D 世界锚点、外部纹理导入以及全套游戏输入路由。

## 1. 运行时与 View 生命周期

### 创建与销毁

```c
// 配置运行时（传入 NULL 则使用默认系统字体与 View 上限）
weft_runtime_desc rt_desc = {
    .struct_size = sizeof(rt_desc),
    .abi_version = WEFT_ABI_VERSION,
    .max_views = 16,
    .flags = WEFT_RUNTIME_SYSTEM_FONTS,
};
weft_runtime* runtime = NULL;
weft_runtime_create(&rt_desc, &runtime);

// 创建 View（指定逻辑尺寸与设备像素缩放比）
weft_view_desc view_desc = {
    .struct_size = sizeof(view_desc),
    .logical_width = 1920.0f,
    .logical_height = 1080.0f,
    .device_scale = 1.5f,
};
weft_view_handle view = {0};
weft_view_create(runtime, &view_desc, &view);
```

- 所有自定义字体（`weft_runtime_add_font`、`weft_runtime_set_generic_family`、`weft_runtime_add_fallback_family`）与 UI 着色器效果（`weft_runtime_add_effect`）应在加载首个页面之前注册完成。
- 销毁运行时前需先销毁全部存活 View（存在未销毁 View 时 `weft_runtime_destroy` 返回 `WEFT_BUSY`）。
- 窗口分辨率或 DPI 变化时调用 `weft_view_resize`；移动端刘海屏/圆角屏通过 `weft_view_set_safe_area` 注入 `env(safe-area-inset-*)`。

### 页面加载与运行模式标志

`weft_view_load` 通过 `weft_page_desc` 加载页面的脚本、样式表与关键帧，并支持组合以下页面标志位：

| 标志位 | 作用 |
| --- | --- |
| `WEFT_PAGE_FORGIVING_CSS` | 启用浏览器风格宽容 CSS 模式：支持 `@layer`、`@media`、`@supports`、`@property`、CSS 嵌套与 Selectors 4，遇到超出场景子集的声明时丢弃并输出诊断而非拒绝页面（推荐用于 Tailwind 等构建产物） |
| `WEFT_PAGE_WEB_PLATFORM` | 启用完整 Web 平台接口：`console`（转为诊断输出）、`localStorage`/`sessionStorage`、`dataset`、`matchMedia`、`IntersectionObserver`、`requestIdleCallback`、`focus()`/`blur()` 等 |
| `WEFT_PAGE_SPATIAL_NAVIGATION` | 启用方向键与手柄空间导航：自动按几何位置在可交互元素间移动焦点，并同步悬停状态与滚屏 |

### 资源加载与持久化存储

- **资源回调**：通过 `weft_view_set_resource_reader` 注册宿主文件读取回调（例如对接 Unreal Pak、Unity Addressables 或虚拟文件系统），页面的 `<img>`、`background-image: url(...)` 与 `@font-face` 均经此统一加载。
- **本地存储持久化**：使用 `weft_view_set_storage_item` 在加载页面前恢复 `localStorage`，帧末通过 `weft_view_get_storage` 按修订号增量读取变更并写入游戏存档。
- **剪贴板与诊断**：通过 `weft_view_set_clipboard` 对接系统剪贴板；每帧调用 `weft_view_drain_diagnostics` 收集脚本异常、样式警告与 `console` 日志并转发至引擎日志系统。

## 2. 双向引擎通信（`weft.engine`）

Weft 内置强类型消息桥接层，支持 JSON 与 MessagePack 载荷，避免在热点路径上拼接脚本字符串。

### 通信模式总览

| 方向与模式 | 宿主 C API | 页面 JS API | 典型场景 |
| --- | --- | --- | --- |
| **引擎 → 页面事件** | `weft_view_emit` | `weft.engine.on` / `once` / `onAny` | 伤害飘字触发、任务更新、音效/状态通知 |
| **引擎 → 页面视图模型** | `weft_view_set_model` | `weft.engine.model(name)` | 玩家血量、弹药、准星状态、小队属性（自动批处理并通知订阅者） |
| **页面 → 引擎单向消息** | `weft_view_drain_messages` | `weft.engine.send(name, value)` | 按钮点击、菜单切换、技能释放指令 |
| **页面 → 引擎异步调用** | `weft_view_drain_messages` + `weft_view_complete_call` | `await weft.engine.call(name, value, {timeout})` | 背包分页查询、存档读写、异步资产加载 |
| **页面 → 引擎同步函数** | `weft_view_register_function` | `weft.engine.invoke(name, value)` | 本地化查表、键位名称查询、轻量配置读取 |

### 页面侧调用示例

```js
// 1. 订阅视图模型（仅在批次更新结束时触发一次回调）
const weapon = weft.engine.model('weapon');
const unsub = weapon.subscribe((value, changedKeys) => {
  ammoText.textContent = `${value.mag} / ${value.reserve}`;
});

// 2. 监听引擎事件
const off = weft.engine.on('combat.hit', payload => {
  showHitMarker(payload.critical);
});

// 3. 发起带超时的异步引擎调用
try {
  const presets = await weft.engine.call('settings.load', null, { timeout: 3000 });
  applySettings(presets);
} catch (err) {
  console.error('Failed to load settings:', err);
}

// 4. 调用宿主注册的同步函数
const label = weft.engine.invoke('i18n.get', 'menu.continue');
```

## 3. 3D 世界锚点（Zero-Script Anchors）

对于每帧跟随 3D 角色或世界物体移动的 UI（头顶血条、NPC 名牌、交互提示、目标指示器），传统做法每帧往 JS 同步坐标并修改 `transform` 会带来大量脚本与样式开销。Weft 提供**零脚本世界锚点机制**：

1. **页面声明**：在元素上标注 `data-anchor="<key>"` 属性即可，无需编写每帧更新的 JS 逻辑。
2. **宿主写入**：引擎将 3D 世界坐标投影到逻辑屏幕空间后，每帧调用 `weft_view_write_anchors(runtime, view, anchors, count)` 提交锚点数组（包含键名、逻辑屏幕坐标、缩放、深度与可见/背面裁剪标记）。
3. **渲染直驱**：锚点变换直接进入渲染显示列表与命中测试几何，不触发 DOM 变更、样式重算或布局重排，确保 3D 物体与 UI 标记严格同帧对齐、零延迟抖动。

## 4. 引擎 GPU 纹理导入（`-weft-texture`）

UI 界面可直接零拷贝显示引擎渲染出的实时 GPU 纹理（例如小地图、3D 角色检视渲染目标、实时狙击镜画面、动态图标集）：

1. 通过渲染器的外部纹理扩展 `weft_renderer_import_texture` 导入宿主原生 GPU 纹理并绑定逻辑名称（支持静态冻结纹理 `WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED` 与每帧重绘的实时渲染目标 `WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE`）。
2. 在页面 CSS 中直接引用：

```css
.minimap {
  background-image: -weft-texture(minimap_rt);
  background-size: cover;
  border-radius: 50%;
}
```

3. 纹理的生命周期受 GPU 完成序号保护；宿主释放租约后，渲染器会在最后一帧采样该纹理的 GPU 提交完成后通过 `retired` 回调通知安全回收。详见 [渲染后端集成指南](render-backend-api.md)。

## 5. 完整游戏输入与交互路由

Weft 不拦截操作系统消息，所有输入由宿主按需注入，并返回明确的事件消费状态，方便引擎实现“UI 优先、空白透传给游戏”的精确分流。

### 指针、滚轮与命中测试

- **快速透传判定**：调用 `weft_view_hit_test(runtime, view, x, y, &hit)` 可在逻辑线程查询指定坐标下是否存在可交互 UI 元素及其 CSS `cursor` 类型，用于决定鼠标与滚轮应交给 UI 还是透传给游戏视角控制。
- **事件注入**：通过 `weft_view_pointer` 注入移动、按下、抬起、离开与滚轮事件；自动合成浏览器语义的 `pointer*`/`mouse*`/`click`/`dblclick` 序列及可滚动容器滚轮行为。

### 多点触控（移动端）

- 通过 `weft_view_set_input_devices` 声明主输入设备（鼠标、触屏或手柄），驱动 `@media (pointer: coarse)` / `(hover: none)` 与 `navigator.maxTouchPoints`。
- 通过 `weft_view_touch` 注入多指触点生命周期（`BEGAN` / `MOVED` / `ENDED` / `CANCELLED`），内置符合移动端浏览器习惯的点击判定、拖拽阈值、遵循 `touch-action` 的滚动与抬手惯性滑行（Fling）。

### 键盘、文本框与系统输入法（IME）

- `weft_view_key` 与 `weft_view_text` 注入物理按键与字符输入；`weft_view_set_focus` 同步宿主窗口/控件的焦点状态。
- 内置完整的 `<input>` / `<textarea>` 光标、选区与编辑行为；宿主可通过 `weft_view_text_input` 查询当前是否处于文本编辑态以及光标屏幕矩形（用于定位系统候选窗），并通过 `weft_view_composition` 与 `weft_view_commit_text` 无缝对接 Windows IMM/TSF、macOS 与 iOS 系统输入法。
- 文本输入期间的按键由页面消费（防止玩家在聊天框打字时误触发角色移动），页面未消费的按键则原样交还宿主游戏输入系统。

### 手柄与空间导航

- `weft_view_gamepad` 按标准 Gamepad 布局同步手柄按键、摇杆与扳机状态，向页面暴露标准 `navigator.getGamepads()` 与连接/断开事件。
- 开启 `WEFT_PAGE_SPATIAL_NAVIGATION` 后，方向键、手柄十字键与左摇杆自动在屏幕可交互元素间执行空间焦点导航（A 确认、B 取消），内置长按连发与自动滚动，且严格向宿主反馈每个按键是否被 UI 消费，避免手柄输入泄漏至游戏角色控制。
