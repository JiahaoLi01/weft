# C/C++ 代码规范

适用于 `src/`、`include/`、`tests/`、`probes/` 的 C++20 与 C11 代码，以及 `shaders/` 的 HLSL。格式由根目录 `.clang-format`（clang-format 22）强制，提交前对改动文件运行 `clang-format -i`。生成文件和第三方代码列在 `.clang-format-ignore`；生成器（如 `tools/generate_flex_cases.py`）直接输出本规范布局。

## 格式

1. 控制语句与函数的左花括号留在声明行，`else`、`else if`、`catch` 紧跟右花括号：`} else {`。关键字与括号之间、二元运算符两侧、逗号与 `for` 分号之后各留一个空格，例如 `for (int i = 0; i < count; ++i) {`。
2. `if`、`else`、`for`、`while`、`do`、函数和 lambda 一律带花括号，哪怕只有一行；块内内容从下一行开始，右花括号单独成行。空块写成 `{` 换行 `}`。
3. namespace 内容缩进；嵌套 namespace 逐层缩进，文件末尾不会出现两个同缩进的右花括号。长 namespace 以 `} // namespace name` 结束。
4. 一行一条语义：一条语句、一个声明各占一行。禁止 `int a = 0, b = 1;`、`T x{}, y{};` 和 `if (...) a; b;` 一类写法；成员同样每个声明一行。
5. 行宽 120。超长字符串字面量（嵌入的 JS/CSS/诊断文本）不拆分。include 顺序保持原样，不自动排序。

## 命名常量

非显然的字面量用命名常量，在头文件中统一定义，不在使用处直接写数字：

- 跨模块共享的单位、Web 常量、文档上限、默认视口和测试随机数参数在 `src/constants/common.hpp`（`weft::constants`）。
- 模块专属的预算、上限、阈值与调优值在 `src/constants/<模块>.hpp`（`weft::constants::<模块>`）；`src/constants/all.hpp` 汇总全部模块头。
- 测试专用值在 `tests/test_constants/<领域>.hpp`（`weft::test_constants::<领域>`），探针专用值在 `probes/` 下的常量头。

需要命名：资源预算与容量上限、超时与重试次数、阈值/容差/epsilon、数值钳制、单位换算与物理常量、URI、类别或材质编号、位掩码、随机种子、帧数/预热/采样数、固定视口尺寸、在多处重复且含义相同的值。

保留字面量：普通算术中的 0、1、-1、2（自增、减半、下标偏移、`size() - 1`）；对明显结构的小循环边界（四个角、RGB 三通道）；CSS/DOM/JS 词汇字符串（属性名、关键字、事件名）；诊断消息；嵌入源码字符串的内容；测试自身的预期值与逐案例数据表；C ABI 头里已有的宏和枚举。

常量的类型与它替换的字面量一致（普通整数字面量为 `int`，`u` 为 `unsigned`，`f` 为 `float`，无后缀浮点为 `double`），数值逐位不变。需要其他类型的使用点写 `static_cast<T>(...)`，不得改变任何表达式或比较的类型。命名用 snake_case，按含义命名（`interpolation_cache_max_entries`），不按数值命名（不用 `k512`）。同一含义只定义一次：多个模块共用的值放进 `common.hpp`，模块头中含义更具体的名字写成对它的引用（`max_wheel_delta_px = constants::max_logical_px_magnitude`）。

实现注意：

- 常量直接传给 `const T&` 形参（`std::min/max/clamp`、`std::chrono` 构造）会 odr-use 该变量，MSVC 生成代码随之改变；写成 `static_cast<T>(constant)` 传纯右值。
- 字符串常量是 `inline constexpr const char*`。少数大函数里 `std::string == constants::x` 会从内联 memcmp 变为调用 `operator==`，语义相同；热路径介意时保留字面量并注明。
- 头文件一律在全局作用域、文件顶部包含，不在 namespace 内部包含。D3D11 探针在第 5 步之前曾在匿名 namespace 中途包含探针头，现已拆成独立翻译单元（`probes/probe_common.hpp` 与每个探针模式一个 `.cpp`）。

## 共享代码

- **运行时代码**：用 `src/base/` 的 `Diag`/`Result`、CSS 标量解析、`GenerationalSlots`、`OwnerThread`，不再新写 trim/split/number 解析、代际槽表或 owner 检查。
- **按错误码分派**：控制流按 `Diag` 码分支，不匹配错误消息文本。
- **测试与探针**：用 `tests/support/` 的 `require`/`WEFT_REQUIRE`、文件读写、`json_string`、PNG 读写，不在各文件里重定义。
- **探针代码**：D3D11 探针在全局作用域的 `namespace probes` 中，共享部分放 `probe_common.hpp`，模式入口在 `probe_modes.hpp` 声明。

## 语义等价验证

纯格式或常量化改动必须不改变生成代码。验证方法：改动前后各构建一次 Release（无调试信息），对全部项目目标文件做 `dumpbin /disasm`，按函数比较指令。允许的差异只有：

- 新增花括号作用域导致 MSVC 局部静态/局部类/lambda 的作用域编号和 COMDAT 顺序变化；
- RTTI 名称长度随作用域编号变化；
- 测试宏把表达式字符串化（`#expression`）时的空格，以及 `__LINE__` 行号；
- 与字符串常量比较时内联 memcmp 与调用 `operator==` 之间的切换（须逐个函数人工确认比较内容相同）。

脚本是 `tools/disassembly_equivalence.py`（`dump` / `compare` / `functions` 三个子命令），2026-10-10 规范化阶段的运行命令与结果见 [status](status.md)。

着色器用 `fxc /Ges /WX /O3` 前后编译，比较指令输出。
