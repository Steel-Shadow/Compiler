#ifndef COMPILER_IR_H
#define COMPILER_IR_H

#include "common/Type.h"

#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace IR {

std::string typeToIR(Type type);
std::string typeToMIPSLoad(Type type);
std::string typeToMIPSStore(Type type);

struct Operand {
    std::string type;
    std::string text;

    Operand() = default;
    Operand(std::string type, std::string text);

    static Operand constant(Type type, int value);
    static Operand local(Type type, std::string name);
    static Operand global(Type type, std::string name);
    static Operand label(std::string name);

    bool empty() const;
    std::string typed() const;
};

enum class Opcode {
    Alloca,
    Load,
    Store,
    Binary,
    ICmp,
    Br,
    CondBr,
    Ret,
    Call,
    Phi,
    GetElementPtr,
    Cast,
    Comment,
};

struct PhiIncoming {
    Operand value;
    std::string block;
};

struct Instruction {
    Opcode opcode{Opcode::Comment};
    std::string result;
    std::string op;
    std::string type;
    std::vector<Operand> operands;
    std::vector<PhiIncoming> incoming;
    std::string note;

    static Instruction alloc(std::string result, Type allocatedType, int count = 1);
    static Instruction load(std::string result, Type valueType, Operand ptr);
    static Instruction store(Operand value, Operand ptr);
    static Instruction binary(std::string result, std::string op, Type resultType, Operand lhs, Operand rhs);
    static Instruction icmp(std::string result, std::string predicate, Operand lhs, Operand rhs);
    static Instruction br(std::string target);
    static Instruction condBr(Operand cond, std::string trueTarget, std::string falseTarget);
    static Instruction ret(Operand value);
    static Instruction retVoid();
    static Instruction call(std::string result, Type returnType, std::string callee, std::vector<Operand> args);
    static Instruction phi(std::string result, Type type, std::vector<PhiIncoming> incoming);
    static Instruction cast(std::string result, std::string op, Type targetType, Operand value);
    static Instruction makeComment(std::string text);

    bool hasResult() const;
    bool isTerminator() const;
    std::string toString() const;
};

struct Parameter {
    Type type{Type::Int};
    std::string name;
    std::vector<int> dims;
    bool storesAddress{false};

    Parameter() = default;
    Parameter(Type type, std::string name, std::vector<int> dims = {}, bool storesAddress = false);
};

struct BasicBlock {
    std::string name;
    std::vector<Instruction> instructions;

    explicit BasicBlock(std::string name);
    bool terminated() const;
    void add(Instruction inst);
};

struct Function {
    std::string name;
    Type returnType{Type::Void};
    std::vector<Parameter> params;
    std::vector<std::unique_ptr<BasicBlock>> blocks;

    int tempId{0};
    int blockId{0};

    Function(std::string name, Type returnType, std::vector<Parameter> params = {});

    BasicBlock &createBlock(const std::string &hint);
    std::string newTemp(const std::string &hint = "v");
    std::string newBlockName(const std::string &hint);
    std::string signature() const;
};

struct GlobalVar {
    std::string name;
    Type elementType{Type::Int};
    std::vector<int> dims;
    std::vector<int> init;
    bool constant{false};
};

struct Module {
    std::string name;
    std::vector<GlobalVar> globals;
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<std::string> declarations;

    explicit Module(std::string name = "module");

    Function &createFunction(std::string name, Type returnType, std::vector<Parameter> params = {});
    void addGlobal(GlobalVar global);
    void addBuiltinDeclarations();
    void write(std::ostream &out) const;
    std::string toString() const;
};

class IRBuilder {
public:
    explicit IRBuilder(Module &module);

    Module &module();
    Function *function();
    BasicBlock *block();

    Function &startFunction(std::string name, Type returnType, std::vector<Parameter> params = {});
    BasicBlock &createBlock(const std::string &hint);
    void setInsertPoint(BasicBlock &basicBlock);

    Operand emitAlloca(Type type, const std::string &hint, int count = 1);
    Operand emitLoad(Type type, Operand ptr, const std::string &hint = "load");
    void emitStore(Operand value, Operand ptr);
    Operand emitBinary(const std::string &op, Type type, Operand lhs, Operand rhs, const std::string &hint = "tmp");
    Operand emitICmp(const std::string &predicate, Operand lhs, Operand rhs, const std::string &hint = "cmp");
    Operand emitCast(const std::string &op, Type targetType, Operand value, const std::string &hint = "cast");
    void emitBr(const std::string &target);
    void emitCondBr(Operand cond, const std::string &trueTarget, const std::string &falseTarget);
    void emitRet(Operand value);
    void emitRetVoid();
    Operand emitCall(Type returnType, const std::string &callee, std::vector<Operand> args, const std::string &hint = "call");
    void emitComment(const std::string &text);

private:
    Module *module_;
    Function *function_{nullptr};
    BasicBlock *block_{nullptr};

    std::string makeTemp(const std::string &hint);
};

} // namespace IR

#endif
