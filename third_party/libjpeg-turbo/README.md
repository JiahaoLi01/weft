# libjpeg-turbo 3.1.4.1 固定来源

- 上游：https://github.com/libjpeg-turbo/libjpeg-turbo
- 完整 commit：`9217719d3a58633923b096af4c1d50d304768a64`（tag 3.1.4.1，2026-03-27 发布）。
- 下载：https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.4.1/libjpeg-turbo-3.1.4.1.tar.gz（2026-11-08 取得）。
- archive SHA256：`ecae8008e2cc9ade2f2c1bb9d5e6d4fb73e7c433866a056bd82980741571a022`。
- 签名：同目录发布的 `.sig` 当时一并取得，但本机没有 gpg，**签名未验证**；只核对了 SHA256，并确认解包结果与 `source/` 逐文件相同。
- `source/` 为未经修改的完整 633 文件，逐文件 SHA256 记录在 `third_party/manifest.json`；没有补丁。
- 许可：IJG License 与 Modified BSD（`source/LICENSE.md`、`source/README.ijg`），原样保留。

用途：页面图片（`<img>`、`url()`）的 JPEG 解码。Chrome 用的也是 libjpeg-turbo（Chromium 156 自带 3.1.0），选同一系列的最新修补版：解码结果按设计不随修补版变化，带有之后的安全修复。

构建：上游 CMake 拒绝 `add_subdirectory()`，所以 `cmake/WeftThirdParty.cmake` 的 `weft_jpeg` 照抄上游 `CMakeLists.txt` 的 `JPEG_SOURCES`（libjpeg API 库）自建静态库：

- 配置头 `jconfig.h`、`jconfigint.h`、`jversion.h` 由上游模板生成，取值与上游构建的选择相同（版本、`HIDDEN`、`INLINE`、`THREAD_LOCAL`、`SIZEOF_SIZE_T`、`HAVE_BUILTIN_CTZL` 等）。
- 不带 SIMD：x86 上需要 NASM；Chrome 用的精确整数 IDCT 在 C 与 SIMD 下结果逐位相同。
- 不带算术编码（`WITH_ARITH_DEC`、`WITH_ARITH_ENC` 关）：Chromium 的 libjpeg-turbo 没有它，Chrome 对这种 JPEG 只给尺寸、什么也不画；weft 一致。
- 不构建 TurboJPEG API、工具、测试或 Java。正常构建不联网。

解码设置照 Blink 的 `JPEGImageDecoder`：`JDCT_ISLOW`、平滑上采样、块平滑，输出 RGBA；CMYK 与 YCCK 按 Adobe 反相 CMYK 换成 RGB（与 Blink 相同的整数公式）；应用 Exif 方向；不读 ICC 配置（颜色当作 sRGB）。见 `src/view/view_image_codecs.cpp`。

测试图片：`tools/image_decode_fixtures.mjs` 用本目录源码另行构建的 `cjpeg` 生成夹具（不进入 weft 的构建），并复用上游 `testimages` 里的四张 JPEG。
