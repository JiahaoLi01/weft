# SheenBidi 3.0.0 固定来源

- 上游：https://github.com/Tehreer/SheenBidi
- 完整 commit：`cfe430e7375a7845b679adae9d51dac6deaa8858`（tag v3.0.0，2026-01-24 发布）。
- 下载：https://codeload.github.com/Tehreer/SheenBidi/tar.gz/cfe430e7375a7845b679adae9d51dac6deaa8858（2026-11-07 取得）
- archive SHA256：`9546f423820d4618c704a30c51c191b0b002aa79100e0ec34cb2cf461fd6353e`。
- `source/` 为未经修改的完整 224 文件，逐文件 SHA256 记录在 `third_party/manifest.json`；没有第三方补丁。
- 许可：Apache License 2.0（`source/LICENSE`），原样保留。

用途：Unicode 双向算法（UAX #9：段落层级、嵌入与隔离、括号配对、L1 行尾空白）和文字系统切分（UAX #24 的 script run，Common 与 Inherited 随上下文归属）。它只依赖 C 标准库头文件。HarfBuzz 不做这两件事；本项目不自研 Unicode 表或这两个算法。

构建：`cmake/WeftThirdParty.cmake` 的 `weft_sheenbidi` 私有静态库，只编译上游的单一编译单元 `Source/SheenBidi.c`（定义 `SB_CONFIG_UNITY`，与上游 CMake 默认一致），公开头文件取 `Headers/`。保留上游默认的 scratch memory（线程本地缓冲）；不构建可选的 text API（`SB_CONFIG_EXPERIMENTAL_TEXT_API`）、数据生成器或上游测试。正常构建不联网。

数据版本：Unicode 17.0.0（`Tools/Unicode/*.txt` 的文件头）。2026-11-07 在 macOS（Apple clang，Debug，非 unity 构建）用上游 CMake 构建并运行未经修改的上游测试：16/16 通过；`AlgorithmTests` 对 `BidiTest.txt` 与 `BidiCharacterTest.txt`（91,707 行数据）报告 0 错误，含镜像字符检查。复现：

```
cmake -S third_party/sheenbidi/source -B <临时目录> -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSB_CONFIG_UNITY=OFF -DBUILD_TESTING=ON
cmake --build <临时目录>
ctest --test-dir <临时目录> --output-on-failure
```

Windows、Android、iOS 上的上游测试尚未运行。这是上游算法一致性的证据，不是本项目从右到左排版与 Chrome 一致的声明；那部分见 [文字方向与复杂文字](../../docs/bidi-text.md)。
