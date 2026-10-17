# UI 着色器效果的写法

UI 着色器效果是页面里 `background-image: -weft-effect(name)`（也可写在 `background` 简写里）画出的一段 HLSL 像素函数：烟雾、发光、溶解，以及读取元素背后内容的折射、热浪。设计与实现见 [架构](architecture.md) 的“UI 着色器效果、粒子与背景拷贝”。本文写给写效果的人：源文件怎么写、清单怎么填、怎么编译与注册，以及读背景的效果多出来的输入与规则。

## 源文件

一个效果是一个 `.hlsl` 文件，定义：

```hlsl
float4 effect(EffectInput input)
```

返回该像素的**线性预乘** RGBA。weft 的外壳（`src/render/shaders/scene.hlsl` 的 `WEFT_EFFECT` 部分）在它前面，负责取实例记录、圆角与裁剪链的覆盖率、抗锯齿、元素透明度和到目标合成空间的转换，效果函数不碰这些。

源文件可以有自己的函数、类型和 `static` 全局量，但：

- 不声明资源（纹理、采样器、常量缓冲、`register`），不写 `#include` 与 `namespace`，全局量都带 `static`。`cmake/check_effect.cmake` 在编译前检查，不过就拒绝并说明原因。
- 名字不要与外壳已有的重复：外壳定义了 `Paint`、`Clip`、`Vertex`、`EffectInput`、`ps_main`、`effect*` 开头的函数、`TwoPi` 等常量。重复时编译报 `redefinition`。外壳的 `TwoPi`、`effectLinear`、`effectEncoded` 可以直接用。

### EffectInput

| 字段 | 含义 |
|---|---|
| `uv` | 像素在元素盒内的 uv（0 到 1，跨边框盒） |
| `size` | 盒子的 CSS px 尺寸 |
| `position` | 像素在盒内的位置，CSS px（盒子自身的坐标轴，变换旋转时跟着转） |
| `pixel` | 像素在目标里的位置，设备像素 |
| `scale` | 每 CSS px 的设备像素数 |
| `time` | 效果时间，秒（清单声明用时间时帧持续推进） |
| `parameters[16]` | 参数，按清单的顺序，每个一个 `float4`；没设的为 0 |
| `backdrop`、`backdropAcross`、`backdropDown`、`backdropCopied` | 读背景的效果用，见下文 |

纹理最多 4 张：`effectTexture0` 到 `effectTexture3`，按清单的顺序，用 `effectRepeat`（线性过滤、重复寻址）采样。数据纹理（PNG 的红色通道）的值在 `r`；sRGB 图片是线性预乘颜色。

### 外壳提供的函数

- `effectLinear(c)`、`effectEncoded(c)`：sRGB 编码值与线性光之间的转换（标量或 `float3`）。
- `effectBackdropUV(input, uv)`、`effectBackdrop(input, backdropUV)`、`effectBackdrop(input)`：读背景，见下文。

## 清单

宿主按清单注册效果（`weft_runtime_add_effect`），清单与 C ABI 的字段一一对应：

| 清单（JSON） | `weft_effect_desc` | `weft_add_effect`（CMake） | 含义 |
|---|---|---|---|
| `name` | `name` | 第二个参数 | CSS 里 `-weft-effect(name)` 的名字 |
| `parameters`：`name`、`type`、`initial` | `parameters` | `PARAMETERS name:type=initial ...` | 参数，CSS 用自定义属性 `--name` 设置；`type` 为 `number`、`vec2`、`vec3`、`vec4`、`color` |
| `textures`：`name`、`path`、`format` | `textures` | `TEXTURES name ...` | 纹理，`format` 为 `data` 或 `srgb` |
| `animated` | `animated` | `ANIMATED` | 函数读时间 |
| `backdrop` | `backdrop` | `BACKDROP <margin>` | 函数读元素背后的内容 |
| `backdropMargin` | `backdrop_margin` | `BACKDROP` 的值 | 背景拷贝超出盒子的 CSS px（0 到 256） |
| `bytecode` | `dxbc_*`、`spirv_*` | 自动 | 字节码的路径前缀 |

`weft_effect_desc` 的 `backdrop`、`backdrop_margin` 接在结构末尾：`struct_size` 等于 `offsetof(weft_effect_desc, backdrop)` 的旧调用方照常注册，效果不读背景。`backdrop` 只能是 0 或 1，`backdrop_margin` 在 0 到 256 之间，`backdrop` 为 0 时边距必须为 0，否则 `WEFT_INVALID_ARGUMENT`。

## 编译与注册

运行时不带着色器编译器。宿主用 weft 的外壳离线编译：

```
cmake -DSOURCE=refraction.hlsl -DOUTPUT=out/refraction -P cmake/compile_effect.cmake
```

得到 `out/refraction.encoded.dxbc`、`.linear.dxbc`（D3D11、D3D12），有 Vulkan SDK 的 DXC 时还有 `.encoded.spv`、`.linear.spv`。`compile_effect.cmake` 可以连同 `check_effect.cmake` 与 `scene.hlsl` 随宿主发布（CRPG 的 Unreal 插件就这样做）。`weft_page_runner --effects <effects.json>` 按上表的 JSON 注册效果，`bytecode` 相对 JSON 文件。weft 自己的目标用 `weft_add_effect`（`cmake/WeftEffects.cmake`）把效果编进程序。

## 读背景的效果

清单写 `"backdrop": true` 的效果多一份输入：元素背后的内容。折射、热浪、毛玻璃之类的扭曲都用它。

### 背后是什么

- 元素开始绘制之前目标里的内容：拷贝在元素的第一个绘制项之前进行，所以不含元素自己的背景色、外阴影和 `backdrop-filter`，也不含它的子元素。
- 元素自己因为透明度开了隔离组（`opacity` 小于 1 且有内容），拷贝在组开始之前，读的是组外的目标；效果画进组里，随组一起按透明度合成。
- 在祖先的隔离组里，只读到该组已经画的内容（与 `backdrop-filter` 的背景根相同）；组外的内容读不到。
- 在最外层，包括宿主目标上原有的内容（游戏画面）。Vulkan 的目标不可读时（图像没有 `TRANSFER_SRC` 用法），拷贝是透明的，并计一条 `backdrop of an unreadable target`。
- 带 `filter` 的元素连同后代、透视平面里的元素画进自己的图像，只读到该图像里已经画的内容；元素自己带 `filter` 时什么也读不到。

### 拷贝的范围与映射

拷贝的范围是盒子在四边各加 `backdropMargin` CSS px（在盒子自身的坐标里），经元素变换与设备缩放后的外接矩形，取整到设备像素，限制在目标之内。读到拷贝范围之外（超过边距，或超过目标边缘）时取最近的纹素，即边缘拉伸。位移不超过边距时读到的都是真实内容；拉伸只在超出时出现。

- `input.backdrop`：本像素在拷贝里的 uv。
- `input.backdropAcross`、`input.backdropDown`：盒子 uv 横向、纵向每变化 1 时，拷贝 uv 的变化量（已含元素的旋转与缩放、设备缩放和拷贝的大小）。
- `effectBackdropUV(input, uv)`：盒子 uv 处的点在拷贝里的 uv。按盒子自身的坐标位移：`effectBackdropUV(input, input.uv + offset / input.size)` 是本像素沿盒子坐标轴移动 `offset` CSS px 处的背景；元素旋转时位移跟着旋转。
- `effectBackdrop(input, backdropUV)`：拷贝里 `backdropUV` 处的颜色，线性过滤，线性预乘；`effectBackdrop(input)` 是本像素背后的颜色。
- `input.backdropCopied`：拷贝已绑定时为 1。清单没有声明 `backdrop` 的效果调用 `effectBackdrop` 读到透明，各后端相同。

最小的例子，把背景移动 `--shift` CSS px（`tests/effects/backdrop_shift.hlsl`）：

```hlsl
float4 effect(EffectInput input) {
    float2 shift = input.parameters[0].xy;
    return effectBackdrop(input, effectBackdropUV(input, input.uv + shift / input.size));
}
```

在整数个设备像素的位移上，它画出的就是背景的纹素本身（测试用它逐像素核对）。

### 代价与限制

- 每个读背景的效果在绘制前拷贝一块区域（D3D11 的 `CopySubresourceRegion`、D3D12 的 `CopyTextureRegion`、Vulkan 结束渲染过程后的 `vkCmdCopyImage`），不与其他效果合批；拷贝打断它前后的批次。
- 帧里有读背景的效果时，任何变化都整幅重画（与 `backdrop-filter` 相同），保留的 pass 也不打补丁。
- 文档页面（线性合成空间的显示列表渲染器）不画读背景的效果；D2D 参考光栅不画任何效果。
- 带 `filter` 的元素与透视平面里的效果只读到所在图像的内容（见上）。

## 样例

`samples/effects/`：两个读背景的效果与它们的清单 `effects.json`。构建时（`weft_sample_effects` 目标）用 `compile_effect.cmake` 编到 `build/<preset>/samples/effects/`，连同清单，可以直接给 `weft_page_runner --effects` 用。`tests/CMakeLists.txt` 也把它们编进测试。

- **折射**（`refraction.hlsl`，`backdropMargin` 24）：透过一块玻璃看背后。离各边 `--bevel` CSS px 之内玻璃弯曲，视线向盒子中间偏折，最多 `--strength` CSS px，越靠边越强（强度乘以离边远近的平方）；玻璃表面还有周期为 `--cell` CSS px 的平滑波纹，按 `--ripple` 乘以强度偏移视线；`--tint`（CSS 颜色）盖在上面。默认 `--strength: 10`、`--bevel: 18`、`--ripple: 0.2`、`--cell: 40`、`--tint: transparent`。
- **热浪**（`heat-haze.hlsl`，`animated`，`backdropMargin` 8）：背后的内容像热空气上方那样晃动。相距 `--wavelength` CSS px 的波纹以 `--speed` CSS px/s 上升，最多移动 `--amplitude` CSS px；盒子底部最强，往顶部按 `--fade`（0 到 1）减弱，离盒子边缘一个波长之内渐弱为零，所以盒子没有硬边。只依赖位置与时间：同一时间画出同样的画面。默认 `--amplitude: 3`、`--wavelength: 28`、`--speed: 36`、`--fade: 0.6`。

页面里这样用：

```css
.lens { background-image: -weft-effect(refraction); --strength: 14; border-radius: 12px; }
.heat { background-image: -weft-effect(heat-haze); --amplitude: 2.5; }
```

## 测试

`scene_backdrop_effects_contract`（`tests/scene_backdrop_effects_tests.cpp`）：显示列表的拷贝点（元素的第一个绘制项，或它的组的开始）、pass 的拷贝与被它截断的批次、列表检查的拒绝、线性 pass 的拒绝、损伤；D3D11（WARP）在缩放 1 与 2 下、Vulkan 在本机设备上，与不带效果画出的背景逐像素对照已知的位移：经过边距、不读元素自己的背景色、无边距时与目标边缘的夹取、元素自己的透明度组、祖先的组、旋转的盒子；两个样例在不移动时与背景相同、`--tint` 盖在背景上、斜面只移动边缘、同一时间画面相同。`render_d3d12_view_contract` 经 C ABI 注册效果（含旧大小的结构与拒绝的值），D3D12 与 D3D11 逐字节相同。
