# QuickJS NG 固定源码

本目录的 source 为未修改的 QuickJS-NG v0.17.0，commit 6d46d07d04041b40f4f49eaa7fdebe44c314c699。采用上游 CMake 的 qjs 静态库，不向脚本导出 shell/std/os 能力。正常构建不联网。

来源为 [官方 commit 归档](https://github.com/quickjs-ng/quickjs/archive/6d46d07d04041b40f4f49eaa7fdebe44c314c699.zip)，下载归档 SHA256 为 d0d41bf4842480ca8672a856eb86cb2d5cb6aae008072c9fe91786f9185a0692。归档被 Git 忽略，source 与 [上游 LICENSE](source/LICENSE) 保留。

没有上游源码补丁。候选引擎尚需 GC、框架、平台和内存探针才能作为正式 SDK 依赖。

另外 `cmake/WeftThirdParty.cmake` 把同一份未修改的 `source/libunicode.c` 单独编成 `weft_unicode_case` 静态库（它依赖的 `cutils.h` 在这一版是纯头文件），供 `weft::text` 的 CSS `text-transform` 大小写映射使用（`lre_case_conv`、`lre_is_cased`、`lre_is_case_ignorable`），不链接引擎。表的 Unicode 版本随本版本固定。
