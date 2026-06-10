# BUAA 编译器设计文档

## 1. 项目概述

本项目实现了一个面向 SysY/BUAA 编译实验语言的编译器。编译器读取源程序，完成词法分析、语法分析、语义检查、AST 构建、中间代码生成、标量 mem2reg 优化，并最终输出可由 Mars 运行的 MIPS 汇编。

当前编译流程如下：

```text
source
  -> Lexer
  -> Recursive-Descent Parser + AST
  -> Semantic Check + Symbol Table
  -> IRGenerator
  -> Scalar Mem2Reg
  -> MIPS Backend
  -> Mars
```

命令行接口保持实验框架要求的五输出形式：

```bash
./build/src/Compiler testfile.txt lexer.txt error.txt ir.txt mips.txt
```

如果前端检测到语义错误，编译器只输出 `error.txt`，不会继续生成有效 IR 和 MIPS；否则会输出 `ir.txt` 和 `mips.txt`。

## 2. 目录结构

```text
src/
  AST/              AST 节点定义、递归下降解析、语义动作和常量求值
    decl/           声明、定义、初始化值
    expr/           表达式、LVal、函数调用、类型转换
    func/           函数定义、主函数、形参/实参
    stmt/           语句、控制流、printf、switch
  frontend/
    lexer/          词法分析器和 token 类型
    parser/         递归下降解析辅助函数
    symTab/         符号表、符号对象、作用域遍历
  common/           类型系统公共工具
  errorHandler/     错误收集与输出
  ir/               IR 数据结构、IRBuilder、AST 到 IR 生成、优化 Pass
  backend/          MIPS 后端
  tools/            通用容器工具
  main.cpp          编译流程入口
test/
  run_testcase_2026.py  自动化测试脚本
```

## 3. 总体架构

编译器入口位于 `src/main.cpp`，核心函数为 `compile`。该函数依次完成：

1. `Lexer::init` 初始化词法分析器并预读 token。
2. `CompUnit::parse` 启动递归下降解析，构建 AST，同时维护符号表并记录错误。
3. 如果 `Error::hasError` 为真，停止后续阶段。
4. `IR::generateModule` 从 AST 生成模块级 IR。
5. `IR::runScalarMem2Reg` 对可提升的局部标量变量做 SSA 化。
6. `MIPS::generate` 将 IR 翻译为 MIPS 汇编。

各阶段之间的数据边界比较清晰：前端输出 AST 和符号表，中端使用 AST 与符号表生成 IR，后端只依赖 IR 模块，不直接访问 AST。

## 4. 前端设计

### 4.1 词法分析

词法分析器位于 `src/frontend/lexer`。`Lexer::init` 读取完整源文件，维护当前字符位置、行号、列号，并预读固定深度的 token。对外主要提供：

- `Lexer::curLexType` 和 `Lexer::curToken`：当前 token。
- `Lexer::next()`：消费并返回下一个 token。
- `Lexer::peek(n)`：查看未来 token，用于递归下降分支判断。
- `Lexer::findAssignBeforeSemicolon()`：辅助区分赋值语句和表达式语句。

词法阶段会输出 token 序列到 `lexer.txt`。为了兼容测试仓库中的原始 C 文件，词法器会跳过以 `#` 开头的预处理指令行，例如 `#include`、`#ifdef`、`#endif`。

### 4.2 语法分析与 AST

语法分析采用递归下降方式实现。每类语法成分通常对应一个 AST 结构体和一个静态 `parse()` 方法，例如：

- `CompUnit::parse()`
- `Decl::parse()`
- `FuncDef::parse()`
- `Stmt::parse()`
- `Exp::parse()`

AST 节点使用 `std::unique_ptr` 表达所有权，`CompUnit` 作为根节点保存全局声明、普通函数和主函数。

解析过程中会同步输出语法成分标记，以满足实验要求的语法输出格式。语法恢复主要通过 parser 辅助函数完成，对缺失分号、右括号、右中括号等错误进行补偿并继续分析。

### 4.3 符号表与作用域

符号表位于 `src/frontend/symTab`。整体结构是一棵作用域树：

```text
global
  -> function scope
      -> block scope
          -> nested block scope
```

`SymTab` 保存当前作用域中的标识符到 `Symbol` 的映射，并通过 `prev/next` 记录父子作用域。主要符号类型包括：

- `ValueSymbol`：变量、常量、数组、静态局部变量。
- `ParamSymbol`：函数形参，数组形参会记录其传递的是地址。
- `FuncSymbol`：函数符号，保存返回类型和参数列表。

前端解析时构建作用域树；IR 生成阶段通过 `resetTraversal`、`enterRecordedScope`、`leaveRecordedScope` 按照已记录的作用域顺序重新遍历，保证 AST 到 IR 时能够拿到与前端一致的符号绑定。

### 4.4 语义检查

错误处理集中在 `src/errorHandler`。`Error::raise(code)` 记录实验要求的错误码和行号，并通过 `std::set` 去重、排序输出。

当前支持的典型语义检查包括：

- 标识符重定义和未定义。
- 函数参数个数、类型、数组维度不匹配。
- `const` 对象被赋值。
- `break`、`continue` 出现在非法上下文。
- 非 void 函数缺少返回值，void 函数返回表达式。
- `printf` 格式串与实参数量/类型不匹配。
- 数组、标量、函数对象误用。
- `switch` 中 case/default 重复和类型错误。

## 5. 中间表示设计

IR 定义位于 `src/ir/IR.h`。IR 采用接近 LLVM IR 的三地址形式，基本单位为：

- `Module`：编译单元，保存全局变量、函数和内建声明。
- `Function`：函数，保存参数、基本块和临时编号。
- `BasicBlock`：基本块，保存线性指令序列。
- `Instruction`：指令，使用 `Opcode` 区分操作。
- `Operand`：操作数，记录 IR 类型和文本表示。

主要指令包括：

```text
alloca, load, store
binary, icmp, cast
br, condbr, ret, call
phi, getelementptr
```

IR 中的类型使用字符串表示为 `i32`、`i8`、`ptr`、`void` 等；源语言类型由 `Type` 枚举维护，二者通过 `typeToIR`、`ptrToValue`、`valueToPtr` 等工具函数转换。

`IRBuilder` 封装了临时变量创建、基本块创建和指令插入，避免 IRGenerator 直接操作底层容器。

## 6. IR 生成

IR 生成入口为 `IR::generateModule(const CompUnit&)`。生成器维护：

- `module_`：当前 IR 模块。
- `builder_`：当前函数和基本块的插入器。
- `storage_`：前端符号到 IR 存储位置的映射。
- `breakTargets_`、`continueTargets_`：控制流目标栈。
- `stringId_`：字符串常量编号。

### 6.1 全局对象和局部对象

全局变量直接生成为 `GlobalVar`，保存在 `Module::globals` 中。局部标量变量初始生成 `alloca + load/store` 形式，再交给 mem2reg 提升。数组变量保留在栈上，通过 `getelementptr` 计算元素地址。

静态局部变量使用唯一全局名字保存，既保留局部语义，又复用全局存储模型。

### 6.2 表达式

表达式按语法优先级递归生成：

```text
Exp -> AddExp -> MulExp -> UnaryExp -> PrimaryExp/LVal/Call/Cast
```

算术表达式生成 `binary`，关系和相等比较生成 `icmp`。`char` 与 `int` 之间通过 `coerce` 插入必要的 `zext/trunc` 转换。

函数调用会先生成实参，再生成 `call` 指令。为了匹配源语言求值顺序，实参生成时保留了与原语义一致的求值策略。

### 6.3 控制流

`if`、`while`、`for`、`switch` 都被显式翻译为基本块和分支指令。

短路逻辑不先计算成普通整数表达式，而是直接生成控制流：

- `LOrExp`：某一项为真时直接跳到 true 分支。
- `LAndExp`：某一项为假时直接跳到 false 分支。
- 当逻辑表达式需要作为值使用时，通过 true/false/end 三个基本块和 `phi` 合成 `0/1`。

这种设计可以保证带函数调用的短路表达式不会错误执行本应跳过的调用。

### 6.4 数组和指针

数组对象的 `storage_` 记录元素类型、维度和是否保存地址。数组形参被视为地址传递；局部数组和全局数组则通过基地址加线性下标访问。

线性下标根据维度展开，最终由 `getelementptr` 生成元素地址。数组作为函数参数传递时，传递的是首元素地址或指定子数组地址。

## 7. Mem2Reg 优化

`src/ir/Passes.cpp` 实现了标量 mem2reg。该 pass 只提升满足条件的局部标量 `alloca`：

- 分配对象不是数组。
- 类型为 `i32` 或 `i8`。
- 该地址只被 `load/store` 使用，没有被传递或取地址逃逸。

优化流程如下：

1. 收集 CFG 前驱关系。
2. 找出可提升的 `alloca`。
3. 按基本块顺序维护变量状态，删除对应的 `alloca/load/store`。
4. 在多前驱基本块需要合流时插入 `phi`。
5. 填充 `phi` 的 incoming value。
6. 删除 trivial phi，并将其使用点替换为唯一真实值。

trivial phi 删除对递归和复杂控制流样例很关键。它可以避免后端生成大量无意义的 phi 边复制，降低 MIPS 运行时间，并避免某些控制流边上出现退化的跳转链。

## 8. MIPS 后端设计

MIPS 后端位于 `src/backend/MIPS.cpp`。入口为：

```cpp
std::string MIPS::generate(const IR::Module &module);
```

后端整体分为三个部分：

1. `.data` 段：输出全局变量和字符串常量。
2. `.text` 段：输出运行时入口、内建 I/O stub 和用户函数。
3. 函数发射器：逐函数建立栈帧并翻译 IR 指令。

### 8.1 栈帧布局

每个函数使用一个固定大小栈帧。`Frame` 中记录：

- `valueSlots`：IR 临时值和参数的栈槽。
- `pointerSlots`：`alloca` 对象的栈槽。
- `labels`：IR 基本块名到 MIPS label 的映射。
- `phiTempOffset`：处理 phi 并行复制时使用的临时槽。
- `frameSize`：对齐后的栈帧大小。

函数入口保存 `$ra`，前四个参数从 `$a0-$a3` 落栈，更多参数从调用者栈上传入。

### 8.2 指令翻译

后端先为函数内 IR 临时值建立 liveness 信息，再使用图着色算法分配物理寄存器。未成功分配的值仍保留栈槽作为 spill 位置。

寄存器使用策略如下：

- `$t3-$t7`：caller-saved，可分配给不跨函数调用存活的 IR 值。
- `$s0-$s7`：callee-saved，可分配给跨函数调用仍然存活的 IR 值；函数入口保存实际使用到的 `$s` 寄存器，返回前恢复。
- `$t0-$t2`、`$t8`、`$t9`：保留为后端 scratch 寄存器，不参与图着色分配。`$t8` 主要用于 phi copy 和栈槽搬运，`$t9` 主要用于指针地址物化。

在指令发射时，如果操作数已经分配到物理寄存器，后端会直接使用该寄存器；只有立即数、全局对象、栈上 spill 值和复杂地址计算才 materialize 到 scratch 寄存器中。

主要翻译规则：

- `load/store`：根据类型选择 `lw/sw` 或 `lbu/sb`。
- `binary`：翻译为 `addu/subu/mul/div/mfhi` 等。
- `icmp`：翻译为 `slt/seq/sne/sle/sge` 等比较序列。
- `br/condbr`：生成 `j/bne`，并在跳转前插入目标块 phi 的边复制。
- `call`：前四个参数放入 `$a0-$a3`，其余参数压到调用栈，返回值从 `$v0` 取回。
- `ret`：将返回值放入 `$v0`，恢复 `$ra` 和 `$sp` 后 `jr $ra`。
- `getelementptr`：按元素大小计算地址偏移。
- `cast`：处理 `int`、`char` 之间的截断和零扩展。

### 8.3 Phi 消除

MIPS 不支持 SSA phi，因此后端在控制流边上执行 phi copy：

```text
pred -> target:
  target.phi = value_from_pred
```

后端会跳过自复制，并检测并行复制中的覆盖风险。存在环或读写冲突时，先把源值保存到临时栈槽，再统一写入目标，保证多个 phi 的赋值语义等价于同时发生。

对没有覆盖风险的 phi copy，后端会尽量生成 direct-copy：

- 寄存器到寄存器：直接 `move`。
- 寄存器到栈槽：直接 `sw`。
- 栈槽或立即数到寄存器：直接加载到目标寄存器。

这样可以减少固定 scratch 寄存器带来的内部搬运指令。

### 8.4 后端局部优化

后端包含少量 peephole 和模式优化：

- 删除跳到下一条 label 的冗余 `j`。
- 将 `li reg, 0` 改写为 `move reg, $zero`。
- 将连续 `sw` 后同地址 `lw` 合并为寄存器 `move`。
- 将只被分支使用的 `icmp + condbr` 融合为更短的分支序列。
- 将部分 `% 2^k == 0` 的分支判断改写为 `andi`。

这些优化不会改变 IR 语义，但可以明显减少复杂递归、短路表达式和循环样例中的指令数量。

## 9. 测试设计

测试脚本位于 `test/run_testcase_2026.py`。它可以发现 testcase-2026 的 generated 用例，并自动执行：

1. 调用编译器生成 `lexer/error/ir/mips`。
2. 对错误样例比对 `error.txt`。
3. 对正确样例调用 Mars 运行 MIPS，并比对 `ans.txt`。

常用命令：

```bash
cmake --build build
python3 test/run_testcase_2026.py --suite all -j 8
python3 test/run_testcase_2026.py --suite all --filter regression/saitewasreset/complex_recursive -j 1
```

脚本默认启用并行测试。由于 Mars 的运行时间受并发 CPU 抢占影响较大，脚本会在并行时按 worker 数自动放宽编译器和 Mars 的 timeout；单进程测试仍使用原始 timeout。

正确样例运行时默认会打开 Mars 指令统计，并汇总所有正确样例的 `Final Cycle`：

```text
Final Cycle Sum: <sum> (243 correct case(s))
```

该数值用于对比不同后端优化策略的整体效果；如果只想检查正确性，可以使用 `--no-cycles` 关闭统计。

当前全量测试覆盖：

- 2026 新增正确样例和错误样例。
- regression 正确样例和错误样例。
- 字符类型、数组、函数调用、短路逻辑、switch、递归、复杂控制流等场景。

## 10. 构建与运行

项目使用 CMake 构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

默认可执行文件：

```text
build/src/Compiler
```

手动运行单个测试：

```bash
./build/src/Compiler testfile.txt lexer.txt error.txt ir.txt mips.txt
java -jar test/vendor/Mars-2024.jar nc mips.txt < in.txt
```

## 11. 设计取舍

### 11.1 递归下降前端

递归下降实现直接对应文法，便于定位语法规则和语义动作；缺点是 AST 节点中混合了解析、检查和部分求值逻辑，后续如果继续扩展语言，最好逐步拆分 visitor。

### 11.2 图着色 MIPS 后端

当前后端从纯栈式值分配演进为图着色寄存器分配。为了避免破坏调用约定，寄存器分配会区分跨调用存活和值不跨调用存活两类情况：跨调用存活的值只使用 `$s0-$s7`，普通临时值可以使用 `$t3-$t7` 和 `$s0-$s7`。

这种设计比固定 scratch 生成方式显著减少了栈读写和内部 `move`，但仍保留 `$t0-$t2/$t8/$t9` 作为代码生成 scratch，使地址计算、phi 并行复制和 spill 回退路径保持简单可靠。

### 11.3 标量优先的优化策略

mem2reg 只处理非逃逸标量，不处理数组和指针对象。这样实现复杂度低，风险可控，也适合当前 MIPS 后端；后续可以在 IR 稳定后继续加入常量传播、死代码删除和循环优化。

## 12. 后续优化方向

可以继续推进的方向包括：

- 增加 IR 级 CFG 验证器，检查基本块终结指令、phi incoming 和前驱关系是否一致。
- 实现常量传播、死代码删除和简单公共子表达式消除。
- 继续减少固定 scratch 寄存器依赖，让更多 `$t` 寄存器可以进入图着色分配。
- 优化 spill 选择策略，降低高频循环中的内存访问。
- 将 AST 的 parse、semantic check、IR generation 进一步解耦。
- 为关键 bug 样例建立固定 regression 子集，避免后续重构破坏短路、phi、数组形参和递归调用。
