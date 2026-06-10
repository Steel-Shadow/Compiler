# BUAA 编译器项目

本项目目前保留编译器前端和 AST 基线：词法分析、递归下降语法分析、语义检查、符号表维护和抽象语法树构建。旧版中间代码和 MIPS 后端已经移除，新的中端/后端可以在这个基线上重新设计。

## 当前结构

```text
src/
  AST/              AST 节点、语义动作、常量求值和类型检查
    decl/           声明和初始化
    expr/           表达式、LVal、函数调用
    func/           函数定义和形参/实参
    stmt/           语句、控制流、printf
  frontend/
    lexer/          词法分析
    parser/         递归下降公共工具
    symTab/         符号表和类型系统
  common/           公共类型工具
  errorHandler/     错误记录和输出
  tools/            通用容器工具
```

## 构建

项目使用 CMake 构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

默认可执行文件位于：

```text
build/src/Compiler
```

## 运行

为了兼容现有测试脚本，命令行参数仍保留旧的 5 输出形式：

```bash
./build/src/Compiler testfile.txt lexer.txt error.txt ir.txt mips.txt
```

当前版本只执行前端流程：

1. `Lexer::init(source, lexer-output)`
2. `CompUnit::parse()`
3. 语义错误写入 `error-output`

`ir-output` 和 `mips-output` 会被创建为空文件，等待新的中端/后端接入。

## 前端能力

前端支持当前 AST 中已经实现的语法和语义检查，包括：

- 变量、常量、数组、函数、形参和实参。
- `int`、`char`、`void`、数组形参和显式类型转换。
- `if`、`while`、`for`、`switch`、`break`、`continue`、`return`。
- `printf` 格式串和内置 I/O 函数符号。
- 重定义、未定义、参数数量/类型、`const` 赋值、返回值、控制流上下文、数组/标量误用等错误。

## 后续重构入口

新的中间表示可以从 AST 节点和符号表重新接入。建议先确定：

- IR 的所有权模型和模块/函数/基本块边界。
- AST 到 IR 的遍历方式，是 visitor 还是节点局部方法。
- 静态存储、数组形参、字符串常量和内置 I/O 的统一抽象。
- 后端是否仍以 MIPS 为目标，或先做更稳定的 IR dump。
