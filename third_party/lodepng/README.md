# lodepng 固定来源

- 上游：https://github.com/lvandeve/lodepng
- 完整 commit：`e584a8490db1b76b81bf298c11c09d3b55d9c270`（`LODEPNG_VERSION_STRING` 为 `20261006`；上游没有发布标签，按 commit 固定）。
- 下载：https://codeload.github.com/lvandeve/lodepng/tar.gz/e584a8490db1b76b81bf298c11c09d3b55d9c270（2026-10-16，262,771 字节）
- archive SHA256：`c9cc1dcd143bd1365f0ce50cfc14bacab43d7bee5a2be212d6f4b73eefa8626f`。
- `source/` 为未经修改的完整 26 文件，逐文件 SHA256 记录在 `third_party/manifest.json`；没有第三方补丁。
- 许可：`source/LICENSE` 的 zlib 许可原样保留。

用途：scene 页面 CSS `url()` 背景图的 PNG 解码（宿主经资源回调交来文件的字节，见 [与引擎通信](../../docs/engine-communication.md)）。lodepng 自带 inflate，不引入 zlib。

CMake 只把 `source/lodepng.cpp` 编成私有静态库 `weft_lodepng`，定义 `LODEPNG_NO_COMPILE_ENCODER`、`LODEPNG_NO_COMPILE_DISK`、`LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS`、`LODEPNG_NO_COMPILE_CPP`：只有解码器，不读写文件（文件由宿主读），不解析文本、时间、ICC 等辅助块（颜色按 sRGB 解释，gAMA/iCCP 不参与），只用 C 接口。上游的示例、单元测试、模糊测试与工具（`examples/`、`lodepng_unittest.cpp`、`lodepng_fuzzer.cpp`、`lodepng_util.*`、`pngdetail.cpp`、`lodepng_benchmark.cpp`）随源码保留，不编译。
