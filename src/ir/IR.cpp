#include "ir/IR.h"

#include <sstream>

namespace IR {
namespace {
std::string sanitizeHint(const std::string &hint) {
    std::string out;
    out.reserve(hint.size());
    for (char ch: hint) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_') {
            out += ch;
        }
    }
    return out.empty() ? "v" : out;
}

std::string arrayType(Type elementType, const std::vector<int> &dims) {
    std::string text = typeToIR(elementType);
    for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
        text = "[" + std::to_string(*it) + " x " + text + "]";
    }
    return text;
}

std::string joinOperands(const std::vector<Operand> &operands) {
    std::ostringstream out;
    for (size_t i = 0; i < operands.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << operands[i].typed();
    }
    return out.str();
}
} // namespace

std::string typeToIR(Type type) {
    switch (type) {
        case Type::Void:
            return "void";
        case Type::Int:
            return "i32";
        case Type::Char:
            return "i8";
        case Type::IntPtr:
        case Type::CharPtr:
            return "ptr";
    }
    return "void";
}

std::string typeToMIPSLoad(Type type) {
    return type == Type::Char ? "lb" : "lw";
}

std::string typeToMIPSStore(Type type) {
    return type == Type::Char ? "sb" : "sw";
}

Operand::Operand(std::string type, std::string text) :
    type(std::move(type)), text(std::move(text)) {}

Operand Operand::constant(Type type, int value) {
    return Operand(typeToIR(type), std::to_string(value));
}

Operand Operand::local(Type type, std::string name) {
    if (!name.empty() && name.front() != '%') {
        name = "%" + name;
    }
    return Operand(typeToIR(type), std::move(name));
}

Operand Operand::global(Type type, std::string name) {
    if (!name.empty() && name.front() != '@') {
        name = "@" + name;
    }
    return Operand(typeToIR(type), std::move(name));
}

Operand Operand::label(std::string name) {
    if (!name.empty() && name.front() == '%') {
        name.erase(name.begin());
    }
    return Operand("label", "%" + name);
}

bool Operand::empty() const {
    return text.empty();
}

std::string Operand::typed() const {
    return type + " " + text;
}

Instruction Instruction::alloc(std::string result, Type allocatedType, int count) {
    Instruction inst;
    inst.opcode = Opcode::Alloca;
    inst.result = std::move(result);
    inst.type = typeToIR(allocatedType);
    if (count > 1) {
        inst.operands.push_back(Operand::constant(Type::Int, count));
    }
    return inst;
}

Instruction Instruction::load(std::string result, Type valueType, Operand ptr) {
    Instruction inst;
    inst.opcode = Opcode::Load;
    inst.result = std::move(result);
    inst.type = typeToIR(valueType);
    inst.operands.push_back(std::move(ptr));
    return inst;
}

Instruction Instruction::store(Operand value, Operand ptr) {
    Instruction inst;
    inst.opcode = Opcode::Store;
    inst.operands.push_back(std::move(value));
    inst.operands.push_back(std::move(ptr));
    return inst;
}

Instruction Instruction::binary(std::string result, std::string op, Type resultType, Operand lhs, Operand rhs) {
    Instruction inst;
    inst.opcode = Opcode::Binary;
    inst.result = std::move(result);
    inst.op = std::move(op);
    inst.type = typeToIR(resultType);
    inst.operands.push_back(std::move(lhs));
    inst.operands.push_back(std::move(rhs));
    return inst;
}

Instruction Instruction::icmp(std::string result, std::string predicate, Operand lhs, Operand rhs) {
    Instruction inst;
    inst.opcode = Opcode::ICmp;
    inst.result = std::move(result);
    inst.op = std::move(predicate);
    inst.type = "i1";
    inst.operands.push_back(std::move(lhs));
    inst.operands.push_back(std::move(rhs));
    return inst;
}

Instruction Instruction::br(std::string target) {
    Instruction inst;
    inst.opcode = Opcode::Br;
    inst.operands.push_back(Operand::label(std::move(target)));
    return inst;
}

Instruction Instruction::condBr(Operand cond, std::string trueTarget, std::string falseTarget) {
    Instruction inst;
    inst.opcode = Opcode::CondBr;
    inst.operands.push_back(std::move(cond));
    inst.operands.push_back(Operand::label(std::move(trueTarget)));
    inst.operands.push_back(Operand::label(std::move(falseTarget)));
    return inst;
}

Instruction Instruction::ret(Operand value) {
    Instruction inst;
    inst.opcode = Opcode::Ret;
    inst.operands.push_back(std::move(value));
    return inst;
}

Instruction Instruction::retVoid() {
    Instruction inst;
    inst.opcode = Opcode::Ret;
    return inst;
}

Instruction Instruction::call(std::string result, Type returnType, std::string callee, std::vector<Operand> args) {
    Instruction inst;
    inst.opcode = Opcode::Call;
    inst.result = std::move(result);
    inst.type = typeToIR(returnType);
    inst.op = std::move(callee);
    inst.operands = std::move(args);
    return inst;
}

Instruction Instruction::phi(std::string result, Type type, std::vector<PhiIncoming> incoming) {
    Instruction inst;
    inst.opcode = Opcode::Phi;
    inst.result = std::move(result);
    inst.type = typeToIR(type);
    inst.incoming = std::move(incoming);
    return inst;
}

Instruction Instruction::makeComment(std::string text) {
    Instruction inst;
    inst.opcode = Opcode::Comment;
    inst.note = std::move(text);
    return inst;
}

bool Instruction::hasResult() const {
    return !result.empty();
}

bool Instruction::isTerminator() const {
    return opcode == Opcode::Br || opcode == Opcode::CondBr || opcode == Opcode::Ret;
}

std::string Instruction::toString() const {
    std::ostringstream out;
    switch (opcode) {
        case Opcode::Alloca:
            out << result << " = alloca " << type;
            if (!operands.empty()) {
                out << ", " << operands.front().typed();
            }
            break;
        case Opcode::Load:
            out << result << " = load " << type << ", " << operands.front().typed();
            break;
        case Opcode::Store:
            out << "store " << operands[0].typed() << ", " << operands[1].typed();
            break;
        case Opcode::Binary:
            out << result << " = " << op << " " << type << " " << operands[0].text << ", " << operands[1].text;
            break;
        case Opcode::ICmp:
            out << result << " = icmp " << op << " " << operands[0].type << " " << operands[0].text << ", " << operands[1].text;
            break;
        case Opcode::Br:
            out << "br " << operands.front().typed();
            break;
        case Opcode::CondBr:
            out << "br " << operands[0].typed() << ", " << operands[1].typed() << ", " << operands[2].typed();
            break;
        case Opcode::Ret:
            out << "ret";
            if (!operands.empty()) {
                out << " " << operands.front().typed();
            } else {
                out << " void";
            }
            break;
        case Opcode::Call:
            if (!result.empty()) {
                out << result << " = ";
            }
            out << "call " << type << " @" << op << "(" << joinOperands(operands) << ")";
            break;
        case Opcode::Phi:
            out << result << " = phi " << type << " ";
            for (size_t i = 0; i < incoming.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << "[ " << incoming[i].value.text << ", %" << incoming[i].block << " ]";
            }
            break;
        case Opcode::GetElementPtr:
            out << result << " = getelementptr " << note;
            break;
        case Opcode::Cast:
            out << result << " = " << op << " " << operands.front().typed() << " to " << type;
            break;
        case Opcode::Comment:
            out << "; " << note;
            break;
    }
    return out.str();
}

Parameter::Parameter(Type type, std::string name, std::vector<int> dims, bool storesAddress) :
    type(type), name(std::move(name)), dims(std::move(dims)), storesAddress(storesAddress) {}

BasicBlock::BasicBlock(std::string name) :
    name(std::move(name)) {}

bool BasicBlock::terminated() const {
    return !instructions.empty() && instructions.back().isTerminator();
}

void BasicBlock::add(Instruction inst) {
    instructions.push_back(std::move(inst));
}

Function::Function(std::string name, Type returnType, std::vector<Parameter> params) :
    name(std::move(name)), returnType(returnType), params(std::move(params)) {}

BasicBlock &Function::createBlock(const std::string &hint) {
    auto block = std::make_unique<BasicBlock>(newBlockName(hint));
    auto *raw = block.get();
    blocks.push_back(std::move(block));
    return *raw;
}

std::string Function::newTemp(const std::string &hint) {
    return "%" + sanitizeHint(hint) + "." + std::to_string(tempId++);
}

std::string Function::newBlockName(const std::string &hint) {
    return sanitizeHint(hint) + "." + std::to_string(blockId++);
}

std::string Function::signature() const {
    std::ostringstream out;
    out << typeToIR(returnType) << " @" << name << "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << (params[i].storesAddress ? "ptr" : typeToIR(params[i].type)) << " %" << params[i].name;
    }
    out << ")";
    return out.str();
}

Module::Module(std::string name) :
    name(std::move(name)) {}

Function &Module::createFunction(std::string name, Type returnType, std::vector<Parameter> params) {
    auto function = std::make_unique<Function>(std::move(name), returnType, std::move(params));
    auto *raw = function.get();
    functions.push_back(std::move(function));
    return *raw;
}

void Module::addGlobal(GlobalVar global) {
    globals.push_back(std::move(global));
}

void Module::addBuiltinDeclarations() {
    declarations = {
            "declare i32 @get_int()",
            "declare i8 @get_char()",
            "declare void @get_string(ptr, i32)",
            "declare void @put_int(i32)",
            "declare void @put_char(i8)",
            "declare void @put_string(ptr)",
            "declare void @put_str(ptr)",
    };
}

void Module::write(std::ostream &out) const {
    out << "; ModuleID = '" << name << "'\n";
    for (const auto &decl: declarations) {
        out << decl << "\n";
    }
    if (!declarations.empty() && (!globals.empty() || !functions.empty())) {
        out << "\n";
    }
    for (const auto &global: globals) {
        out << "@" << global.name << " = " << (global.constant ? "constant " : "global ")
            << arrayType(global.elementType, global.dims) << " ";
        if (global.init.empty()) {
            out << "zeroinitializer";
        } else if (global.dims.empty()) {
            out << global.init.front();
        } else {
            out << "[";
            for (size_t i = 0; i < global.init.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << typeToIR(global.elementType) << " " << global.init[i];
            }
            out << "]";
        }
        out << "\n";
    }
    if (!globals.empty() && !functions.empty()) {
        out << "\n";
    }
    for (const auto &function: functions) {
        out << "define " << function->signature() << " {\n";
        for (const auto &block: function->blocks) {
            out << block->name << ":\n";
            for (const auto &inst: block->instructions) {
                out << "  " << inst.toString() << "\n";
            }
        }
        out << "}\n\n";
    }
}

std::string Module::toString() const {
    std::ostringstream out;
    write(out);
    return out.str();
}

IRBuilder::IRBuilder(Module &module) :
    module_(&module) {}

Module &IRBuilder::module() {
    return *module_;
}

Function *IRBuilder::function() {
    return function_;
}

BasicBlock *IRBuilder::block() {
    return block_;
}

Function &IRBuilder::startFunction(std::string name, Type returnType, std::vector<Parameter> params) {
    function_ = &module_->createFunction(std::move(name), returnType, std::move(params));
    block_ = &function_->createBlock("entry");
    return *function_;
}

BasicBlock &IRBuilder::createBlock(const std::string &hint) {
    return function_->createBlock(hint);
}

void IRBuilder::setInsertPoint(BasicBlock &basicBlock) {
    block_ = &basicBlock;
}

Operand IRBuilder::emitAlloca(Type type, const std::string &hint, int count) {
    const std::string name = makeTemp(hint + ".addr");
    block_->add(Instruction::alloc(name, type, count));
    return Operand("ptr", name);
}

Operand IRBuilder::emitLoad(Type type, Operand ptr, const std::string &hint) {
    const std::string name = makeTemp(hint);
    block_->add(Instruction::load(name, type, std::move(ptr)));
    return Operand(typeToIR(type), name);
}

void IRBuilder::emitStore(Operand value, Operand ptr) {
    block_->add(Instruction::store(std::move(value), std::move(ptr)));
}

Operand IRBuilder::emitBinary(const std::string &op, Type type, Operand lhs, Operand rhs, const std::string &hint) {
    const std::string name = makeTemp(hint);
    block_->add(Instruction::binary(name, op, type, std::move(lhs), std::move(rhs)));
    return Operand(typeToIR(type), name);
}

Operand IRBuilder::emitICmp(const std::string &predicate, Operand lhs, Operand rhs, const std::string &hint) {
    const std::string name = makeTemp(hint);
    block_->add(Instruction::icmp(name, predicate, std::move(lhs), std::move(rhs)));
    return Operand("i1", name);
}

void IRBuilder::emitBr(const std::string &target) {
    if (!block_->terminated()) {
        block_->add(Instruction::br(target));
    }
}

void IRBuilder::emitCondBr(Operand cond, const std::string &trueTarget, const std::string &falseTarget) {
    if (!block_->terminated()) {
        block_->add(Instruction::condBr(std::move(cond), trueTarget, falseTarget));
    }
}

void IRBuilder::emitRet(Operand value) {
    if (!block_->terminated()) {
        block_->add(Instruction::ret(std::move(value)));
    }
}

void IRBuilder::emitRetVoid() {
    if (!block_->terminated()) {
        block_->add(Instruction::retVoid());
    }
}

Operand IRBuilder::emitCall(Type returnType, const std::string &callee, std::vector<Operand> args, const std::string &hint) {
    std::string result;
    if (returnType != Type::Void) {
        result = makeTemp(hint);
    }
    block_->add(Instruction::call(result, returnType, callee, std::move(args)));
    if (returnType == Type::Void) {
        return {};
    }
    return Operand(typeToIR(returnType), result);
}

void IRBuilder::emitComment(const std::string &text) {
    block_->add(Instruction::makeComment(text));
}

std::string IRBuilder::makeTemp(const std::string &hint) {
    return function_->newTemp(hint);
}

} // namespace IR
