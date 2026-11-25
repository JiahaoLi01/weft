# 自定义 UI 着色器效果指南

Weft 支持在任意界面元素上通过 CSS `background-image: -weft-effect(<name>)` 挂载自定义 HLSL 像素着色器，实现发光、流光、能量护盾、溶解、折射、热浪扭曲等游戏级 UI 动效，同时自动继承元素的圆角裁剪、抗锯齿、透明组与目标色彩空间转换。

## 1. 编写效果着色器

每个效果为一个独立的 `.hlsl` 文件，只需实现固定签名的像素入口函数，返回当前像素的**线性预乘 RGBA** 颜色：

```hlsl
float4 effect(EffectInput input)
{
    float pulse = 0.5 + 0.5 * sin(input.time * 4.0 + input.uv.x * TwoPi);
    float4 tint = input.parameters[0]; // 绑定自 CSS 自定义属性
    float alpha = tint.a * (0.35 + 0.65 * pulse);
    return float4(tint.rgb * alpha, alpha);
}
```

### 编写约束

- 外壳管线（`src/render/shaders/scene.hlsl`）已自动处理顶点变换、圆角与裁剪链覆写率、元素不透明度以及输出色彩空间编码，效果函数只需专注像素计算。
- 效果源码中不得声明独立纹理、采样器、常量缓冲（`register`）或 `#include`，文件内全局常量统一使用 `static`；离线编译脚本（`cmake/check_effect.cmake`）会在编译前自动校验。
- 避免与外壳保留符号（`Paint`、`Clip`、`Vertex`、`EffectInput`、`ps_main`、`TwoPi`、`effect*`）重名。

### `EffectInput` 字段与内置辅助函数

| 字段 / 函数 | 说明 |
| --- | --- |
| `input.uv` | 像素在元素边框盒内的归一化 UV（`[0, 1]`） |
| `input.size` | 元素盒子的逻辑 CSS px 尺寸 |
| `input.position` | 像素在元素自身坐标系内的位置（CSS px，随 `transform` 旋转） |
| `input.pixel` / `input.scale` | 目标设备像素坐标与每 CSS px 对应的设备像素比 |
| `input.time` | 效果推进时间（秒；在清单声明 `animated: true` 后自动驱动逐帧重绘） |
| `input.parameters[16]` | 最多 16 个自定义参数（按清单顺序映射到 `float4`，由 CSS `--<name>` 驱动） |
| `effectTexture0` ~ `effectTexture3` | 最多 4 张自定义纹理，通过内置采样器 `effectRepeat`（线性过滤、重复寻址）采样 |
| `effectLinear(c)` / `effectEncoded(c)` | sRGB 编码值与线性光之间的互转（支持标量与 `float3`） |
| `effectBackdropUV` / `effectBackdrop` | 读取元素背后的已绘制内容（仅 `backdrop: true` 效果可用） |

## 2. 读取元素背后画面（折射 / 热浪 / 扭曲）

当效果清单声明 `"backdrop": true` 时，渲染器会在绘制该元素之前自动将当前渲染目标中覆盖元素盒子（并向外扩展 `backdropMargin` CSS px，范围 `0 ~ 256`）的像素拷贝至只读背景纹理，供效果函数以偏移 UV 采样实现玻璃折射、热浪扰动或色散效果。

> 完整折射与热浪示例源码见 [`samples/effects/refraction.hlsl`](../samples/effects/refraction.hlsl) 与 [`samples/effects/heat-haze.hlsl`](../samples/effects/heat-haze.hlsl)。

## 3. 离线编译

Weft 运行时**不内置着色器编译器**，所有效果在构建或资源打包阶段通过 `cmake/compile_effect.cmake` 离线编译为各后端字节码：

```powershell
cmake -DSOURCE=path/to/refraction.hlsl -DOUTPUT=out/refraction -P cmake/compile_effect.cmake
```

编译产物包含目标合成空间（编码空间与线性空间）所需的着色器字节码：

- Direct3D 11 / 12：`*.encoded.dxbc`、`*.linear.dxbc`
- Vulkan：`*.encoded.spv`、`*.linear.spv`（检测到 Vulkan SDK DXC 时自动生成）

`cmake/compile_effect.cmake`、`cmake/check_effect.cmake` 与外壳文件可直接随引擎插件打包分发，供美术或 TA 在离线资产管线中编译自定义效果。

## 4. 清单配置与运行时注册

在加载首个页面之前，宿主通过 `weft_runtime_add_effect` 注册效果及其字节码、参数声明与纹理数据：

| JSON 清单字段 | C ABI (`weft_effect_desc`) | 说明 |
| --- | --- | --- |
| `name` | `name` | CSS `-weft-effect(<name>)` 引用名 |
| `parameters` | `parameters` | 参数列表：`name`（不含 `--`）、`type`（`number` / `vec2` / `vec3` / `vec4` / `color`）、`initial` 默认值 |
| `textures` | `textures` | 纹理列表：`name`、`path`、`format`（`data` 单通道数据或 `srgb` 彩色纹理，支持 PNG 字节流） |
| `animated` | `animated` | 是否依赖 `input.time` 持续驱动动画 |
| `backdrop` / `backdropMargin` | `backdrop` / `backdrop_margin` | 是否采样元素背后画面及外扩安全边距（0–256 CSS px） |
| `bytecode` | `dxbc_*` / `spirv_*` | 各图形后端离线字节码数据 |

注册后，页面中即可像普通背景图一样使用，并通过 CSS 自定义属性或动画实时驱动参数：

```css
.energy-shield {
  background-image: -weft-effect(refraction);
  --bevel: 14;
  --strength: 0.45;
  --tint: rgba(180, 225, 255, 0.35);
}
```
