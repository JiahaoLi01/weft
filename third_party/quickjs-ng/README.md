# QuickJS NG 固定源码

本目录的 source 为未修改的 QuickJS-NG v0.17.0，commit 6d46d07d04041b40f4f49eaa7fdebe44c314c699。采用上游 CMake 的 qjs 静态库，不向脚本导出 shell/std/os 能力。正常构建不联网。

来源为 [官方 commit 归档](https://github.com/quickjs-ng/quickjs/archive/6d46d07d04041b40f4f49eaa7fdebe44c314c699.zip)，下载归档 SHA256 为 d0d41bf4842480ca8672a856eb86cb2d5cb6aae008072c9fe91786f9185a0692。归档被 Git 忽略，source 与 [上游 LICENSE](source/LICENSE) 保留。

没有上游源码补丁。候选引擎尚需 GC、框架、平台和内存探针才能作为正式 SDK 依赖。
