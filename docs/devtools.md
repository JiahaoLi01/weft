# 开发者工具、热重载与双构型发布指南

Weft 内建 Chrome DevTools 协议支持，并提供 ABI 完全一致的 **Release / Dev 双构型交付机制**：开发阶段零侵入挂载 Chrome DevTools 审查、热重载与自动诊断；正式发布包彻底剔除调试代码、字符串与网络依赖。

## 1. Release / Dev 双构型设计

两套二进制共用完全相同的公开头文件与导出符号表，**宿主引擎无需编写条件编译宏，也不必重新编译或链接宿主代码即可直接切换**：

| 特性 | Release 发布版（默认） | Dev 开发版 |
| --- | --- | --- |
| CMake 开关 | `-DWEFT_DEVTOOLS=OFF` | `-DWEFT_DEVTOOLS=ON` |
| `src/devtools` 模块 | 完全不参与编译 | 编入运行时（默认休眠，仅启用时监听） |
| 网络 / Socket 依赖 | 无（Windows 下不导入 `WS2_32.dll`） | 按需开启监听 |
| `weft_devtools_*` 行为 | 安全返回 `WEFT_UNAVAILABLE` | 正常启停与状态查询 |
| 优化等级 | 支持全优化构建（Release） | 支持全优化构建（可边调试边测真实性能） |

- **动态库集成（Windows / macOS）**：保持其余后端构建选项一致，按游戏构建配置将 `weft.dll` / `libweft.dylib` 替换为对应构型即可无缝切换。
- **静态库集成（如 iOS）**：在链接阶段按目标配置选择发布版或开发版静态库。

## 2. 启动与接入 Chrome DevTools

开发版运行时默认不打开网络监听端口，可通过以下任一方式按需开启：

### 方式 A：零代码修改（环境变量）

```sh
# 仅监听本机回环地址
WEFT_DEVTOOLS_LISTEN=9230 ./your_game

# 允许局域网其他设备远程连接（移动端真机调试等场景）
WEFT_DEVTOOLS_LISTEN=0.0.0.0:9230 ./your_game
```

启动后控制台输出各活跃页面的调试地址：

```text
[weft devtools] open devtools://devtools/bundled/devtools_app.html?ws=127.0.0.1:9230/devtools/page/1-1
```

将 `devtools://...` 粘贴到 Chrome 地址栏，或在 Chrome 打开 `chrome://inspect` → `Configure` 添加 `127.0.0.1:9230`，即可在 Remote Target 列表中点击 `inspect` 连接运行中的游戏画面。

### 方式 B：宿主 C ABI 动态开关

引擎可在控制台命令或编辑器调试面板中随时调用：

```c
weft_devtools_desc desc = {
    .struct_size = sizeof(desc),
    .flags = 0,     // 仅本机回环；跨设备远程调试可传入 WEFT_DEVTOOLS_ALL_INTERFACES
    .port = 9230,   // 填 0 表示从 9230 起自动选取首个空闲端口
};
weft_devtools_start(runtime, &desc);
// 查询当前监听端口：weft_devtools_get_info(runtime, &info);
// 停止监听：weft_devtools_stop(runtime);
```

## 3. 核心调试能力

- **Elements 面板**：实时查看完整 DOM 树、盒模型、命中的 CSS 规则与最终计算样式，支持在游戏画面上直接点选定位元素，以及现场修改样式即时预览。
- **Console 面板**：集中查看页面脚本异常、样式降级警告与 `console` 日志，并支持在当前页面上下文实时求值。
- **零重编实时热重载（`devtools_watch.mjs`）**：监听本地页面目录，CSS 修改后保持脚本运行态原地刷新场景，JS 或资源修改后热重载页面，无需重启或重新打包游戏：

```sh
node tools/devtools_watch.mjs 127.0.0.1:9230 path/to/ui/page
```

- **CI 兼容性巡检（`devtools_report.mjs`）**：拉取当前页面中所有被丢弃或超出子集的 CSS 声明与诊断项；存在问题时返回非零退出码，适合接入持续集成流水线：

```sh
node tools/devtools_report.mjs 127.0.0.1:9230
```

- **Chrome 布局逐项比对（`devtools_compare.mjs`）**：将运行中的游戏文档在无头 Chrome 中重新排版，自动输出几何盒子存在差异的元素清单，帮助快速定位排版偏差：

```sh
node tools/devtools_compare.mjs 127.0.0.1:9230
```

> 远程调试真机设备时，将命令中的 `127.0.0.1:9230` 替换为目标设备 IP 与端口即可（IPv6 地址使用方括号，如 `[fd2a::1]:9230`）。
