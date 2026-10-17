# weft 的 logo

weft 的标志与字标，用 weft 的 scene 页面画（只用 SVG 的线与路径描边：圆头、圆角连接、虚线偏移的关键帧，没有字体，没有图片），四个版本各是一个页面文件夹：

| 文件夹 | 版本 |
| --- | --- |
| `dark/` | 深色版：深底（`#0f1117`）上的浅色线，动画 |
| `light/` | 浅色版：浅底（`#f7f5f0`）上的深色线，动画 |
| `color/` | 彩色版：靛蓝的经线，琥珀、珊瑚、青绿的纬线，墨色字母与珊瑚色横笔，动画 |
| `static/` | 静态版：彩色版的最后一帧，不动 |

各文件夹的 `page.json` 先跑自己的 `variant.js`（`globalThis.weftLogo = {palette, animated}`），再跑共用的 `weft-logo.js`；样式表与关键帧共用 `weft-logo.css`、`weft-logo-keyframes.css`。别的组合（比如深色的静态版）只要一个新的 `variant.js`。

## 设计

- **标志**：三根竖的经线、三根横的纬线织成平纹：纬线 j 在经线 i 的上面当且仅当 i + j 是偶数。压在上面的线两侧各有一道 3 个单位宽的底色，把下面的线断开，显出上下。线宽 12、间距 28（设计单位，画出来是 2 倍）。
- **字标**：`weft` 四个字母用同样粗细的线写成；f 和 t 共用一根横笔，从 f 的上面、t 的下面穿过去，和标志的纬线一样。
- **动画**（约 2.4 秒，停在最后一帧）：经线从上往下画出（0.05–0.73 秒）；三根纬线依次被拉过（0.5 秒起，每根晚 0.16 秒，匀速），压在经线上的那几段在线头到达时跟着出现；然后写字母（1.1 秒起）；最后横笔被拉过 f 与 t（1.95 秒起）。每一笔都是 `stroke-dasharray` 等于笔长、`stroke-dashoffset` 从笔长降到 0 的关键帧（`weft-logo-draw`）。

## 运行

```powershell
& build\windows-msvc\bin\Release\weft_page_runner.exe --page examples/logo/color --size 960x400 --frames 180 --out artifacts/local/logo
```

宿主（UE、Unity 插件）载入页面文件夹即可；页面把 logo 居中在 `#root` 里，`#root` 的背景是版本的底色。

## 与 Chrome 对照

`tools/browser_logo_reference.mjs` 用 Chrome 的无头模式与 `probes/page_reference_clock.js` 的时钟，在 `reference-cases.json` 的各帧（1/60 秒一帧，960 × 400）截图，写进 `tests/fixtures/logo-reference`（带截图与来源文件的 SHA256）。`logo_browser_reference`（`tests/logo_reference_tests.cpp`）用 View 与原生 D3D11 pass 画同样的帧逐像素比较：一帧里超过 32 级差的像素不能多于 0.5%；`logo_fixture_integrity` 检查截图是不是现在的 logo 的。改了 logo 之后重新截图：

```powershell
node tools/browser_logo_reference.mjs
ctest --preset windows-release -R logo_ --output-on-failure
```
