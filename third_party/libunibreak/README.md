# libunibreak 6.1 固定来源

- 上游：https://github.com/adah1972/libunibreak
- 完整 commit：`304585d8e2d63187507368d612c3d5fff1486368`（6.1）。
- 下载：https://codeload.github.com/adah1972/libunibreak/tar.gz/304585d8e2d63187507368d612c3d5fff1486368
- archive SHA256：`af93a94c170d3ead4a1c3a8a97c39ce0d60dc0b4e3f6eaee49b28eb559446105`。
- `source/` 为未经修改的完整 61 文件，逐文件 SHA256 记录在 `third_party/manifest.json`；没有第三方补丁。
- 许可：`source/LICENCE` 的 zlib 风格许可和数据源码中的 Unicode 条款均原样保留。

CMake 私有静态构建 unibreakbase/def、linebreak/data/def、eastasianwidthdef、emojidef、graphemebreak、wordbreak；生成表由上游源码 include，不能再次作为独立 compilation unit 编译。不构建上游下载/生成工具，正常构建不联网。UTF-8 验证和解码使用已有 HarfBuzz；没有自研 Unicode 表或 decoder。

本版本断行数据为 Unicode 15.0 / UAX #14 revision 49，字素数据为 Unicode 15.1 / UAX #29 revision 43。2026-10-08 Windows Debug/Release 使用未经修改的 `src/tests.c`：断行 7,619/7,619，通过并按 `test_skips.h` 跳过 35 项（34 项 LB25 与数值 regex tailoring 的区别，1 项 XX→AL）；字素 1,187/1,187、无跳过。它不是最新 Unicode 全量一致性或本项目完整文字兼容的声明。

本项目的 CSS 空白处理、`white-space` 控制和 `line-break:auto` 的 SP/solidus tailoring 位于自己的 `src/layout/inline_probe.cpp`，不会改动上游 UAX 数据。实际合同和回归见 [flow probe](../../docs/flow-layout-probe.md)。
