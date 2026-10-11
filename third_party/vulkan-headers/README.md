# Vulkan-Headers 固定版本

tag vulkan-sdk-1.4.363.0（VK_HEADER_VERSION 363），commit 6802bb4733b63ed5efd3adb308a6c885ef180ea1，与本机安装的 Vulkan SDK 1.4.363.0 同一版本。

source 只含上游归档里的 C 头文件（`include/vulkan/*.h`、`include/vk_video/*.h`）、LICENSE.md、LICENSES 与 README.md，逐文件与固定归档一致，没有修改。C++ 绑定（`*.hpp`、`*.cppm`）、registry 与 tests 没有放入。许可证 Apache-2.0 OR MIT。

来源、归档与逐文件 hash 见 metadata.json 和 ../manifest.json。常规构建不联网；运行时由驱动自带的 Vulkan 加载器（Windows 上的 `vulkan-1.dll`）提供实现，不链接 SDK 的导入库。
