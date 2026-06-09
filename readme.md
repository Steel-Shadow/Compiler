# BUAA 编译器设计文档

本项目是一个使用 C++17 实现的 SysY 编译器。编译器前端完成词法分析、递归下降语法分析、语义检查和抽象语法树构建；中端生成自行设计的四元式 IR；后端将 IR 翻译为可在 Mars 上运行的 MIPS 汇编。

当前实现已经在原有 SysY 子集基础上扩展支持了 2026 测试集涉及的 `char`、类型转换、字符串初始化、`static` 局部变量、`switch`、数组形参、内置 I/O 函数等特性。

## 目录

- [整体结构](#整体结构)
- [构建与运行](#构建与运行)
- [前端设计](#前端设计)
- [符号表与语义检查](#符号表与语义检查)
- [中间代码 IR](#中间代码-ir)
- [目标代码 MIPS](#目标代码-mips)
- [错误处理](#错误处理)
- [测试设计](#测试设计)
- [优化与取舍](#优化与取舍)

## 整体结构

编译流程分为三遍：

1. 词法、语法、语义分析：读取源文件，生成 AST，同时维护符号表并输出错误。
2. IR 生成：遍历 AST，生成 `IR::Module`、`IR::Function`、`IR::BasicBlock` 和四元式指令。
3. MIPS 生成：遍历 IR，生成 `.data` 和 `.text` 段汇编。

主入口位于 [src/main.cpp](src/main.cpp)，接口形式为：

```text
Compiler.exe <source> <lexer-output> <error-output> <ir-output> <mips-output>
```

编译过程如下：

```cpp
auto compUnit = CompUnit::parse();
if (!Error::hasError) {
    auto module = compUnit->genIR();
    module->outputIR();
    MIPS::genMIPS(*module);
}
```

如果前端发现错误，只输出 `error.txt`，不进入 IR 和 MIPS 生成阶段。

源码组织：

```text
src/
  AST/              抽象语法树节点、语义检查、IR 生成
    decl/           声明和初始化
    expr/           表达式、LVal、函数调用
    func/           函数定义和形参/实参
    stmt/           语句、控制流、printf
  frontend/
    lexer/          词法分析
    parser/         递归下降公共工具
    symTab/         符号表和类型系统
  middle/           IR 数据结构和输出
  backend/          MIPS 指令、寄存器、栈内存管理
  errorHandler/     错误记录和输出
```

## 构建与运行

项目使用 CMake 构建。使用普通 Ninja / Makefile 生成器时，可执行文件默认输出到 `build/src/Compiler`；在 Windows 上对应为 `build/src/Compiler.exe`。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

默认运行方式：

```bash
./build/src/Compiler testfile.txt lexer.txt error.txt ir.txt mips.txt
```

调试输出由 [src/config.h](src/config.h) 和 CMake 选项控制。默认输出错误和 MIPS；需要额外打开 `MY_DEBUG` 下的 IR、词法、语法或 stdout 输出时，配置时加入：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCOMPILER_DEBUG_OUTPUT=ON
```

常见编译警告由 `COMPILER_ENABLE_WARNINGS` 控制，默认开启。

## 前端设计

### Lexer

词法分析器使用 `Lexer` namespace 实现单例式状态管理。它维护 3 个 token 的预读窗口：

- `curLexType` / `curToken` 表示当前 token。
- `peek(n)` 提供向前预读能力。
- `row[]` 保存 token 行号。
- `lastRow` 保存最近已消费 token 的行号，用于缺失 `;`、`)`、`]` 时按前一个 token 所在行报错。

保留字和运算符通过 `LinkedHashMap` 顺序匹配。这样可以正确区分 `&&` / `&`、`||` / `|`、`<=` / `<` 等多字符和单字符 token。

语句解析中需要区分：

```text
LVal = Exp ;
Exp ;
```

实现上通过 `findAssignBeforeSemicolon()` 判断当前行到分号前是否存在赋值号。为了错误恢复稳定，该扫描遇到换行即停止，避免把下一行赋值误判为当前表达式语句。

### Parser

语法分析采用递归下降。公共函数位于 `Parser` namespace：

- `singleLex(type)`：匹配一个终结符，并在缺失 `;`、`)`、`]` 时上报 `i/j/k`。
- `output(AST)`：按课程要求输出语法成分。

具体的文法解析方法分散在 AST 节点中，例如：

```cpp
std::unique_ptr<CompUnit> CompUnit::parse();
std::unique_ptr<Stmt> Stmt::parse();
std::unique_ptr<AddExp> AddExp::parse();
```

这样可以把“文法结构、语义动作、IR 生成”放在同一个语法节点附近，减少额外 visitor 带来的类型分发。

### AST

AST 按语法类别拆分为声明、表达式、函数、语句四组。节点使用 `std::unique_ptr` 持有子树。

表达式节点提供以下核心能力：

- `evaluate()`：编译期常量求值。
- `getType()`：语义类型推导。
- `getRank()` / `getLVal()`：判断数组维度、数组实参与左值。
- `genIR()`：生成表达式结果对应的 `IR::Temp`。

数组和指针语义集中在 `LVal`：

- 普通局部/全局数组按基址加偏移访问。
- 数组形参本质上是指针，先从形参槽位取出地址，再访问元素。
- `getOffset()` 同时支持常量偏移和动态偏移；动态表达式中如果遇到非 const 值，会停止编译期常量计算，避免把运行期变量当 0 后触发除零。

### 语句与控制流

语句节点继承 `BlockItem`，并重载 `genIR()`。

控制流主要使用 basic block 和 label：

- `if` 生成 true/false/end block。
- `while` 生成 cond/body/end block。
- `for` 生成 body/iter/end block。
- `switch` 将 switch 表达式保存到临时栈变量，再逐个比较 case，支持 fallthrough 和 default。

`break` 和 `continue` 使用栈维护当前控制流目标：

```cpp
ControlFlow::breakLabels
ControlFlow::continueLabels
```

`printf` 生成 IR 时先求值所有格式参数，再输出字符串片段和参数值。这样可以正确处理：

```c
printf("result %d\n", foo(bar()));
```

其中 `bar()`、`foo()` 的副作用应发生在打印 `"result "` 之前。

## 符号表与语义检查

符号表是树形结构。语法和语义分析阶段通过 `deepIn()` / `deepOut()` 进入和离开作用域；IR 生成阶段通过 `iterIn()` / `iterOut()` 重新遍历已构建的符号表树。

`Symbol` 记录：

- 符号类别：变量、函数、形参。
- 基本类型：`Void`、`Int`、`Char`、`IntPtr`、`CharPtr`。
- `const`、`static` 标记。
- 数组维度和初始值。
- 函数形参列表。
- static 局部变量的全局存储名。

内置函数在编译单元解析开始前加入全局符号表：

```text
get_int() -> int
get_char() -> char
get_string(char[], int) -> void
put_int(int) -> void
put_char(char) -> void
put_string(char[]) -> void
put_str(char[]) -> void
```

语义检查覆盖：

- 重定义、未定义、函数参数数量和类型。
- `const` 赋值。
- `break` / `continue` 所在上下文。
- 非 void 函数缺少返回值。
- void 函数返回表达式、非 void 函数 `return;`。
- 标量、数组、函数在表达式和下标中的误用。
- `printf` 格式串数量和类型匹配。
- `switch` case 类型、case 重复、default 重复。

变量声明时会先把当前定义加入符号表，再解析 initializer。这样 `int d = d;` 会按“引用当前已定义变量”处理，而不是误报未定义。

## 中间代码 IR

IR 结构参考 LLVM 的层次设计，但保持为适合课程实现的四元式：

```text
Module
  Global variables
  Functions
    BasicBlocks
      Inst
```

四元式指令结构：

```cpp
struct Inst {
    Op op;
    std::unique_ptr<Element> res;
    std::unique_ptr<Element> arg1;
    std::unique_ptr<Element> arg2;
};
```

`Element` 的主要派生类：

- `Var`：变量或数组对象，包含名称、作用域深度、维度、类型和符号类别。
- `Temp`：临时值。
- `ConstVal`：立即数。
- `Label`：基本块或函数标签。
- `Str`：字符串常量。

主要 IR 指令包括：

- 内存：`Alloca`、`Load`、`LoadPtr`、`LoadDynamic`、`Store`、`StoreDynamic`。
- 算术与逻辑：`Add`、`Sub`、`Mul`、`Div`、`Mod`、`And`、`Or`、`Not`。
- 比较：`Leq`、`Lss`、`Geq`、`Gre`、`Eql`、`Neq`。
- 控制流：`Br`、`Bif0`、`Bif1`。
- 调用：`Call`、`PushParam`、`PushAddressParam`、`Ret`、`RetMain`。
- I/O：`GetInt`、`GetChar`、`GetString`、`PrintInt`、`PrintChar`、`PrintStr`。
- 栈作用域：`InStack`、`OutStack`。

### 作用域栈

局部作用域结束后，后端需要回收其中声明的变量栈空间。IR 中插入 `InStack` / `OutStack`，用于在 MIPS 后端保存和恢复 `StackMemory::curOffset`。

```c
int a;
{
    int b;
}
int c;
```

进入内部块时保存当前 offset，离开时恢复，保证 `b` 的空间可以被后续局部变量复用。

### 数组偏移

数组偏移计算分两类：

- 下标全为常量：在编译期折叠为 `ConstVal`。
- 存在运行期下标：生成动态偏移计算 IR。

元素大小由类型决定：

- `int` 元素偏移乘 4。
- `char` 元素偏移按字节计算。

## 目标代码 MIPS

MIPS 后端位于 `src/backend`，主要组件：

- `Instruction.*`：IR 到 MIPS 指令翻译。
- `Register.*`：临时寄存器和变量寄存器分配。
- `Memory.*`：栈偏移映射。
- `MIPS.*`：模块级汇编输出和简单 peephole 优化。

### 数据段

全局变量和 static 局部变量输出到 `.data`：

- `int` 使用 `.word`。
- `char` 使用 `.byte`。
- 字符串常量使用 `.asciiz`。

static 局部变量在符号表中记录一个唯一的全局存储名，例如：

```text
__static_map_cnt_0
```

### 栈内存

后端维护 `StackMemory::curOffset` 表示当前栈帧中已使用的空间。变量到栈偏移的映射存储在：

```cpp
StackMemory::varToOffset
```

局部变量分配策略：

- 标量变量可优先放入 `$s` 寄存器。
- 数组始终放在栈上，即使长度为 1。原因是数组可能作为实参传地址，必须有稳定可取的内存地址。
- `char` 数组按字节分配；`int` 和 word 对齐对象会按 4 字节对齐。

地址运算使用 `addu/addiu`，避免 Mars 对高地址栈指针加偏移时报 arithmetic overflow。

### 寄存器分配

临时值使用 `$t` 寄存器池：

- IR `Temp` 通常只定义一次、使用一次。
- `getReg()` 获取寄存器后可及时释放已消费的临时寄存器。
- 若临时寄存器不足，会退化到栈上存储。

局部标量变量使用 `$s` 寄存器池：

- 离开作用域时释放对应变量寄存器。
- 函数调用前保存当前使用的 `$s` 和 `$t` 寄存器，返回后恢复。

### 函数调用

本实现没有使用 `$a0-$a3` 传递用户函数参数，而是统一通过栈传参，简化寄存器冲突处理。

调用过程：

1. 按实参逆序生成 `PushParam` 或 `PushAddressParam`。
2. 调整 `$sp`，保存 `$sp`、`$ra`、临时寄存器和变量寄存器。
3. `jal` 到目标函数。
4. 恢复现场。
5. 如果函数有返回值，立即把 `$v0` 移动到新的 `IR::Temp`，避免嵌套调用覆盖。

数组实参传递的是地址：

- 普通数组：基址加偏移。
- 数组形参再传递：先从形参槽位加载原始地址，再叠加偏移。

## 错误处理

错误处理统一由 `Error` 管理。官方错误以：

```text
<line> <code>
```

形式写入 `error.txt`。

为避免级联报错过多，错误集合按 `(row, code)` 去重。内部调试错误只设置 `hasError`，不写入官方 `error.txt`，防止影响测试输出。

缺失 `;`、`)`、`]` 的行号采用最近已消费 token 的行号。这个规则对跨行表达式和不完整函数调用更稳定，例如：

```c
int a = f(
b;
```

应将缺失右括号报在上一处有效 token 所在行。

## 测试设计

测试使用 `testcase-2026` 生成的测试集和 Mars 运行 MIPS 汇编。

常用命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 首次运行会克隆并生成 testcase-2026
python3 test/run_testcase_2026.py --prepare

# 全量前端/错误处理检查
python3 test/run_testcase_2026.py --suite all

# 运行 MIPS 并和 ans.txt 比对
curl -L -o test/vendor/Mars-2024.jar https://github.com/Lord-Turmoil/Mars-for-BUAA/releases/download/v1.0.1/Mars-2024.jar
python3 test/run_testcase_2026.py --mars-jar ./test/vendor/Mars-2024.jar
```

测试脚本流程：

1. 对正确用例编译源程序，要求 `error.txt` 为空。
2. 若指定 `--mars-jar`，使用 Mars 运行 `mips.txt`，并比较输出和 `ans.txt`。
3. 对错误用例比较生成的 `error.txt` 和标准 `error.txt`。

当前已验证的前端/错误处理测试结果：

```text
Correct cases: 243
Error cases:   44
Failures:      0
```

`test/run_testcase_2026.py` 是项目内的测试 harness。外部 testcase 仓库、
生成数据、Mars jar 和测试输出位于 `test/vendor/`、`test/work/` 等 ignored
目录中，不随项目提交。

## 优化与取舍

当前优化以简单、局部、稳定为主：

- 常量数组下标在 IR 生成阶段折叠。
- `char` 值在需要时用 `andi 0xFF` 截断。
- 乘 4 偏移使用 `sll`。
- `li + addu/subu/and/or/slt` 可合并为立即数指令。
- 部分相邻 `move` 可合并。

尚未实现的优化：

- 全局数据流分析。
- 图着色寄存器分配。
- 死代码消除。
- 公共子表达式消除。
- 基本块级控制流清理。

本项目优先保证语义正确性和测试稳定性。尤其在函数调用、数组传参、错误恢复这些位置，采用了更保守但更可控的实现方式。
