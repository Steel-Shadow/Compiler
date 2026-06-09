//
// Created by Steel_Shadow on 2023/10/26.
//

#ifndef COMPILER_IR_H
#define COMPILER_IR_H

#include "frontend/lexer/LexType.h"
#include "frontend/symTab/Symbol.h"

#include <fstream>
#include <memory>
#include <vector>

// Intermediate Representation
// like the Parser, specific genIR method is distributed in respective AST node struct.
// IR generation is the 2nd pass (1st pass builds the AST).
namespace IR {
extern std::ofstream IRFileStream;

// @formatter:off
enum class Op {
    // not a valid Op, only for init
    Empty,

    // only for showing step into a new contiguous stack memory
    // IfStmt BigForStmt BlockStmt
    InStack,
    OutStack,

    // allocates memory on the stack frame.
    // arg1: Var
    // arg2: size (number of element, times sizeof(type) in backend)
    Alloca,

    // store res to arg1 with offset arg2
    // *(&arg1[Var]+ arg2[ConstVal]) = res[Temp]
    Store,

    // store res to arg1 with offset arg2
    // *(&arg1[Var]+ arg2[Temp]) = res[Temp]
    StoreDynamic,

    // index doesn't consider sizeof(type)
    Load,

    LoadPtr,

    LoadDynamic,

    // res[Temp] = arg1[Temp] op arg2[Temp]
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    And,
    Or,
    Leq,
    Lss,
    Geq,
    Gre,
    Eql,
    Neq,

    LoadImd,
    MulImd,

    // res[Temp] = -arg1[Temp]
    Neg,
    // res[Temp] = arg1[Temp] << 2
    Mult4,

    // res[Temp] = arg1[Temp]
    NewMove,

    // res[Var] = getint()
    GetInt,
    GetChar,
    GetString,
    // arg1[Temp]
    PrintInt,
    PrintChar,
    // arg1[Label]
    PrintStr,

    // no condition jump to arg1[Label]
    Br,
    // if arg1[Temp]==0, jump to arg2[Label]
    Bif0,
    Bif1,

    Call,
    Ret,
    RetMain,

    // arg1: value
    // arg2: nullptr
    PushParam,

    // arg1: base address of array Var
    // arg2: offset of array Var
    PushAddressParam,
    Not,
}; // @formatter:on

// Var Temp ConstVal Str
struct Element {
    virtual ~Element() = default;
    virtual std::string toString() const = 0;
};

struct Var : public Element {
    std::string name;
    int depth;

    bool cons; // const | var
    std::vector<int> dims; // At most 2 dimensions in our work.
    Type type;
    SymType symType = SymType::Value;

    Var(std::string name, int depth, bool cons, const std::vector<int> &dims, Type type, SymType symType = SymType::Value);
    Var(std::string name, int depth);

    friend bool operator==(const Var &lhs, const Var &rhs);
    friend std::size_t hash_value(const Var &obj);

    bool operator<(const Var &other) const;

    std::string toString() const override;
};

// temp register
// if id<0, getReg(this) = static_cast<Register>(-id);
struct Temp : public Element {
    int id;
    Type type;

    // function return Temp
    // id must be -2 -3
    explicit Temp(int id, Type type);
    explicit Temp(Type type);
    Temp(Temp const &other);

    std::string toString() const override;
};

struct ConstVal : public Element {
    int value;
    Type type;

    ConstVal(int value, Type type);

    std::string toString() const override;
};

// No need to save the address of the string,
// it is done by the assembler.
// str_{id}
struct Str : public Element {
    static std::vector<std::string> MIPS_strings;
    int id;

    Str();

    std::string toString() const override;
};

struct Inst {
    Op op;
    std::unique_ptr<Element> res;
    std::unique_ptr<Element> arg1;
    std::unique_ptr<Element> arg2;

    Inst(Op op,
         std::unique_ptr<Element> res,
         std::unique_ptr<Element> arg1,
         std::unique_ptr<Element> arg2);

    void outputIR() const;

private:
    static std::string opToStr(Op anOperator);
};

// like llvm, Label and Temp share id allocator
// nameAndId: Function has no id
struct Label : public Element {
    std::string nameAndId; // nameAndId of BasicBlock
    explicit Label(std::string name, bool isFunc = false);

    std::string toString() const override;
};

struct BasicBlock {
    // go to next BasicBlock
    Label label; // string is good for debugging
    std::vector<Inst> instructions;

    // if isFunc, no suffix number
    explicit BasicBlock(std::string labelName, bool isFunc = false);

    void addInst(Inst inst);

    void outputIR() const;
};

using BasicBlocks = std::vector<std::unique_ptr<BasicBlock>>;

// main is a Function, whose name is "main"
// Function init with a basicBlocks has an empty BasicBlock (can be optimized)
class Function {
    std::string name;
    Type returnType;
    std::vector<Param> params; // Param -> p | p[] | p[][...]
    BasicBlocks basicBlocks;

public:
    // id for BasicBlock & Temp
    // reset to 0 at start of Function
    static int idAllocator;

    Function(std::string name, Type returnType, const std::vector<Param> &params);

    void moveBasicBlocks(BasicBlocks &&bBlocks);

    const BasicBlocks &getBasicBlocks() const;

    Type getReturnType() const;

    std::vector<Param> getParams() const;
};

// backend CodeGen should not rely on SymTab
// we should store information in IR
// opt: const GlobVar can be simplified to ConstVal
struct GlobVar {
    bool cons; // const | var
    Type type;
    std::vector<int> dims; // At most 2 dimensions in our work.

    // filled with 0 if not initialized
    std::vector<int> initVal;

    // the teaching team guarantees that
    // "whenever an array initialization exists, a value must be assigned to each array member."
    GlobVar(bool cons, Type type, std::vector<int> dims, std::vector<int> initVal);

    GlobVar(bool cons, Type type, const std::vector<int> &dims);
};

// only one module in our work
// address of globVars start at $gp
class Module {
    std::string name;
    std::vector<std::pair<std::string, GlobVar>> globVars;
    std::vector<std::unique_ptr<Function>> functions;
    std::unique_ptr<Function> mainFunction;

public:
    explicit Module(std::string name);

    void outputIR() const;

    const std::vector<std::pair<std::string, GlobVar>> &getGlobVars() const;
    const std::vector<std::unique_ptr<Function>> &getFunctions() const;

    const Function &getMainFunction() const;
    void setMainFunction(std::unique_ptr<Function> main_function);

    void addFunction(std::unique_ptr<Function> function);
    void addGlobVar(std::string name, GlobVar globVar);
};

Op LexTypeToIROp(LexType n);
} // namespace IR

template<>
struct std::hash<IR::Var> {
    size_t operator()(const IR::Var &key) const noexcept {
        return hash_value(key);
    }
};

#endif
