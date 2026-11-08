# libwebp 1.6.0 固定来源

- 上游：https://chromium.googlesource.com/webm/libwebp（镜像 https://github.com/webmproject/libwebp）
- 完整 commit：`4fa21912338357f89e4fd51cf2368325b59e9bd9`（tag v1.6.0）。
- 下载：https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz（2026-11-08 取得）。
- archive SHA256：`e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564`。
- 签名：发布的 `.asc` 当时一并取得，但本机没有 gpg，**签名未验证**；只核对了 SHA256，并确认解包结果与 `source/` 逐文件相同。
- `source/` 为未经修改的完整 367 文件，逐文件 SHA256 记录在 `third_party/manifest.json`；没有补丁。
- 许可：BSD 3-Clause（`source/COPYING`）与专利授权（`source/PATENTS`），原样保留。

用途：页面图片（`<img>`、`url()`）的 WebP 解码，动画 WebP 只取第一帧。Chrome 用的也是 libwebp（Chromium 156 自带的是 1.6.0 之后的提交）。

构建：`cmake/WeftThirdParty.cmake` 用上游 CMake（`add_subdirectory`，`EXCLUDE_FROM_ALL`）：

- 关掉所有工具、extras、mux、webp.js 与 fuzz；关掉线程（`WEBP_USE_THREAD`）与 near-lossless。
- 链接 `webpdemux` 与它依赖的 `webp`（编码器部分只按需链接），解码用默认选项（平滑上采样）。
- SIMD 按上游在运行时选择；libwebp 保证 SIMD 与 C 路径结果逐位相同。正常构建不联网。

测试图片：`tools/image_decode_fixtures.mjs` 用本目录源码另行构建的 `cwebp`、`img2webp` 生成夹具（不进入 weft 的构建），并复用上游 `examples/test.webp`。
