## 数组开发进度

数组检查维度没有开启错误处理？
中间代码生成 PushParam 没有使用 a0-a3

## 数据类型系统

完善代码生成的变量类型系统：
前端Parser需要将类型录入 SymTab，还需要添加函数参数和函数返回值(变量已经实现)，再将类型录入IR::Module

## 中间表示

输入 AST（或者四元式） ，输出一个 Module（便于继续生成到MIPS）

数据结构：  
Module{Global}{Function}
Function{BasicBlock}  
BasicBlock{Instruction}

将 glob 变量和 const 变量初值存储在符号表中， evaluate 访问符号表获取 const var。
CompUnit::genIR()中，使用符号表的常量信息优化。

## 中间代码生成

如果单独建类 Visitor，在遍历语法树时，
为了区分不同的语法成分，需要多次使用 dynamic_cast(cpp)/instanceof(java) 判断多态性，这么做极其低效。

```java
SplStmt simple = stmt.getSimpleStmt();
if (simple instanceof AssignStmt) {
    analyseAssignStmt((AssignStmt) simple);
} else if (simple instanceof BreakStmt) {
    analyseBreakStmt((BreakStmt) simple);
} else if (simple instanceof ContinueStmt) {
    ...
}
```

下面修改设计：使用多态性，为基类提供纯虚函数，让派生类重载基类的中间代码生成方法。这样显著提高效率，更加优雅。
这么做，就需要将代码生成方法放入到 AST 节点类中，无需单独建立中间代码生成的类。

```c++

```

## 目标代码生成

生成指令用于维护 Activation Record(Stack Frame)

编译器内部存有AR(SF)，变量名->AR地址

## 栈内存分配

遇到特殊 IR (InStack outStack) 时，将当前的 sp 当前偏移量 curOffset 入栈 stack<int> offsetStack，在结束时出栈恢复相应的curOffset。

## 临时寄存器分配策略

后端对 IR Temp 做指令级活跃性分析，建立冲突图并着色到 `$t0-$t6`。着色失败的 Temp 使用预分配栈槽，常量优先用 `li` 重新物化。`$t7-$t9` 仅作为单条指令内的 scratch 池，不承载跨指令活跃值。

可寄存器化的局部标量单独建立冲突图并着色到 `$s0-$s7`；数组、全局对象和取地址对象保留在内存。

## 函数调用

调用帧为 `$ra`、`$t0-$t6` 和 `$s0-$s7` 保留固定槽位，使参数和保存槽偏移不依赖实际着色结果。调用者只保存调用点之后仍活跃的 `$t` 颜色；被调函数保存实际使用的 `$s` 颜色，非叶函数额外保存 `$ra`。至多四个标量参数的叶函数仅在全部调用点都可安全直传时使用 `$a0-$a3`，否则整函数统一采用栈参数约定。

## 优化

已实现：尾递归消除、常量/复制传播、基于 dominance frontier 的 mem2reg、pruned phi 与 SSA 重命名、GVN-GCM、活跃变量驱动的死存储删除、图着色寄存器分配、调用点活跃寄存器保存、强度削弱和 MIPS peephole。

后续可继续实现 SSA verifier、PRE、Memory SSA、别名分析、memory GVN 和通用函数内联。
