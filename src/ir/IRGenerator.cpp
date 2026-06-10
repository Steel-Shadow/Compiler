#include "ir/IRGenerator.h"

#include "AST/decl/Decl.h"
#include "AST/decl/InitVal.h"
#include "AST/expr/Exp.h"
#include "AST/func/Func.h"
#include "AST/stmt/Stmt.h"
#include "errorHandler/Error.h"
#include "frontend/symTab/SymTab.h"

#include <algorithm>
#include <stack>
#include <unordered_map>
#include <unordered_set>

namespace IR {
namespace {

struct Storage {
    Operand ptr;
    Type valueType{Type::Int};
    std::vector<int> dims;
    bool storesAddress{false};
};

class Generator {
public:
    Module generate(const CompUnit &compUnit) {
        module_.addBuiltinDeclarations();
        SymTab::resetTraversal();
        genGlobalDecls(compUnit);
        for (const auto &func: compUnit.funcDefs) {
            genFunc(*func);
        }
        genMain(*compUnit.mainFuncDef);
        return std::move(module_);
    }

private:
    Module module_{"Compiler"};
    IRBuilder builder_{module_};
    std::unordered_map<const Symbol *, Storage> storage_;
    std::unordered_set<std::string> emittedStaticGlobals_;
    std::stack<std::string> breakTargets_;
    std::stack<std::string> continueTargets_;

    static int arraySize(const std::vector<int> &dims) {
        int size = 1;
        for (int dim: dims) {
            size *= dim;
        }
        return size;
    }

    static Type objectValueType(const Symbol *symbol) {
        if (!symbol) {
            return Type::Int;
        }
        return ptrToValue(symbol->getType());
    }

    static std::string binOp(LexType op) {
        switch (op) {
            case LexType::PLUS:
                return "add";
            case LexType::MINU:
                return "sub";
            case LexType::MULT:
                return "mul";
            case LexType::DIV:
                return "sdiv";
            case LexType::MOD:
                return "srem";
            case LexType::AND:
                return "and";
            case LexType::OR:
                return "or";
            default:
                Error::raise("unsupported binary op");
                return "add";
        }
    }

    static std::string cmpOp(LexType op) {
        switch (op) {
            case LexType::LSS:
                return "slt";
            case LexType::GRE:
                return "sgt";
            case LexType::LEQ:
                return "sle";
            case LexType::GEQ:
                return "sge";
            case LexType::EQL:
                return "eq";
            case LexType::NEQ:
                return "ne";
            default:
                Error::raise("unsupported compare op");
                return "ne";
        }
    }

    static std::vector<Parameter> makeParams(const Params &params) {
        std::vector<Parameter> irParams;
        irParams.reserve(params.size());
        for (const auto &param: params) {
            bool storesAddress = !param.dims.empty();
            irParams.emplace_back(ptrToValue(param.type), param.name, param.dims, storesAddress);
        }
        return irParams;
    }

    void genGlobalDecls(const CompUnit &compUnit) {
        SymTab::resetToGlobal();
        for (const auto &decl: compUnit.decls) {
            Type type = toType(decl->btype->type);
            for (const auto &def: decl->defs) {
                auto *symbol = SymTab::find(def->ident);
                auto *value = symbol ? symbol->asValue() : nullptr;
                if (!value) {
                    continue;
                }
                GlobalVar global;
                global.name = def->ident;
                global.elementType = type;
                global.dims = value->getDims();
                global.init = value->getInitVal();
                global.constant = value->isConst();
                if (global.init.empty()) {
                    global.init.assign(std::max(1, arraySize(global.dims)), 0);
                }
                module_.addGlobal(std::move(global));
                storage_[symbol] = {Operand("ptr", "@" + def->ident), type, value->getDims(), false};
            }
        }
    }

    void emitStaticGlobal(const ValueSymbol *value) {
        if (!value || !value->isStatic() || !emittedStaticGlobals_.insert(value->getStaticStorageName()).second) {
            return;
        }
        GlobalVar global;
        global.name = value->getStaticStorageName();
        global.elementType = ptrToValue(value->getType());
        global.dims = value->getDims();
        global.init = value->getInitVal();
        if (global.init.empty()) {
            global.init.assign(std::max(1, arraySize(global.dims)), 0);
        }
        module_.addGlobal(std::move(global));
    }

    void genFunc(const FuncDef &funcDef) {
        auto *funcSym = SymTab::find(funcDef.ident)->asFunc();
        builder_.startFunction(funcDef.ident, funcSym->getType(), makeParams(funcSym->getParams()));
        SymTab::enterRecordedScope();
        bindParams(funcSym->getParams());
        genBlock(*funcDef.block);
        finishFunction(funcSym->getType());
        SymTab::leaveRecordedScope();
    }

    void genMain(const MainFuncDef &mainFuncDef) {
        builder_.startFunction("main", Type::Int, {});
        SymTab::enterRecordedScope();
        genBlock(*mainFuncDef.block);
        finishFunction(Type::Int);
        SymTab::leaveRecordedScope();
    }

    void bindParams(const Params &params) {
        for (const auto &param: params) {
            auto *symbol = SymTab::find(param.name);
            if (!symbol) {
                continue;
            }
            Type valueType = ptrToValue(param.type);
            Operand incoming = param.dims.empty()
                                       ? Operand(typeToIR(valueType), "%" + param.name)
                                       : Operand("ptr", "%" + param.name);
            if (param.dims.empty()) {
                Operand slot = builder_.emitAlloca(valueType, param.name);
                builder_.emitStore(incoming, slot);
                storage_[symbol] = {slot, valueType, {}, false};
            } else {
                storage_[symbol] = {incoming, valueType, param.dims, true};
            }
        }
    }

    void finishFunction(Type returnType) {
        auto *block = builder_.block();
        if (!block || block->terminated()) {
            return;
        }
        if (returnType == Type::Void) {
            builder_.emitRetVoid();
        } else {
            builder_.emitRet(Operand::constant(returnType, 0));
        }
    }

    void genBlock(const Block &block) {
        for (const auto &item: block.blockItems) {
            if (auto *decl = dynamic_cast<Decl *>(item.get())) {
                genDecl(*decl);
            } else if (auto *stmt = dynamic_cast<Stmt *>(item.get())) {
                genStmt(*stmt);
            }
        }
    }

    void genDecl(const Decl &decl) {
        Type type = toType(decl.btype->type);
        for (const auto &def: decl.defs) {
            genDef(*def, type);
        }
    }

    void genDef(const Def &def, Type type) {
        auto *symbol = SymTab::find(def.ident);
        auto *value = symbol ? symbol->asValue() : nullptr;
        if (!value) {
            return;
        }

        if (value->isStatic()) {
            emitStaticGlobal(value);
            storage_[symbol] = {Operand("ptr", "@" + value->getStaticStorageName()), type, value->getDims(), false};
            return;
        }

        if (!value->getDims().empty()) {
            Operand slot = builder_.emitAlloca(type, def.ident, arraySize(value->getDims()));
            storage_[symbol] = {slot, type, value->getDims(), false};
            builder_.emitComment("array initializer lowering pending for " + def.ident);
            return;
        }

        Operand slot = builder_.emitAlloca(type, def.ident);
        storage_[symbol] = {slot, type, {}, false};
        if (def.initVal) {
            if (auto *expInit = dynamic_cast<ExpInitVal *>(def.initVal.get())) {
                builder_.emitStore(coerce(genExp(*expInit->exp), type), slot);
            }
        }
    }

    void genStmt(const Stmt &stmt) {
        if (auto *assign = dynamic_cast<const AssignStmt *>(&stmt)) {
            storeLVal(*assign->lVal, genExp(*assign->exp));
        } else if (auto *expStmt = dynamic_cast<const ExpStmt *>(&stmt)) {
            if (expStmt->exp) {
                (void) genExp(*expStmt->exp);
            }
        } else if (auto *blockStmt = dynamic_cast<const BlockStmt *>(&stmt)) {
            SymTab::enterRecordedScope();
            genBlock(*blockStmt->block);
            SymTab::leaveRecordedScope();
        } else if (auto *ifStmt = dynamic_cast<const IfStmt *>(&stmt)) {
            genIf(*ifStmt);
        } else if (auto *whileStmt = dynamic_cast<const WhileStmt *>(&stmt)) {
            genWhile(*whileStmt);
        } else if (auto *forStmt = dynamic_cast<const BigForStmt *>(&stmt)) {
            genFor(*forStmt);
        } else if (dynamic_cast<const BreakStmt *>(&stmt)) {
            if (!breakTargets_.empty()) {
                builder_.emitBr(breakTargets_.top());
            }
        } else if (dynamic_cast<const ContinueStmt *>(&stmt)) {
            if (!continueTargets_.empty()) {
                builder_.emitBr(continueTargets_.top());
            }
        } else if (auto *ret = dynamic_cast<const ReturnStmt *>(&stmt)) {
            if (ret->exp) {
                builder_.emitRet(coerce(genExp(*ret->exp), Stmt::retType));
            } else {
                builder_.emitRetVoid();
            }
        } else if (auto *getInt = dynamic_cast<const GetIntStmt *>(&stmt)) {
            Operand value = builder_.emitCall(Type::Int, "get_int", {});
            storeLVal(*getInt->lVal, value);
        } else if (auto *print = dynamic_cast<const PrintStmt *>(&stmt)) {
            genPrint(*print);
        } else if (auto *switchStmt = dynamic_cast<const SwitchStmt *>(&stmt)) {
            builder_.emitComment("switch lowering pending");
            (void) genExp(*switchStmt->exp);
        }
    }

    void genIf(const IfStmt &stmt) {
        SymTab::enterRecordedScope();
        auto &thenBlock = builder_.createBlock("if.then");
        auto &elseBlock = builder_.createBlock("if.else");
        auto &endBlock = builder_.createBlock("if.end");
        builder_.emitCondBr(asBool(genCond(*stmt.cond)), thenBlock.name, elseBlock.name);

        builder_.setInsertPoint(thenBlock);
        genStmt(*stmt.ifStmt);
        builder_.emitBr(endBlock.name);

        builder_.setInsertPoint(elseBlock);
        if (stmt.elseStmt) {
            genStmt(*stmt.elseStmt);
        }
        builder_.emitBr(endBlock.name);

        builder_.setInsertPoint(endBlock);
        SymTab::leaveRecordedScope();
    }

    void genWhile(const WhileStmt &stmt) {
        SymTab::enterRecordedScope();
        auto &condBlock = builder_.createBlock("while.cond");
        auto &bodyBlock = builder_.createBlock("while.body");
        auto &endBlock = builder_.createBlock("while.end");
        builder_.emitBr(condBlock.name);

        builder_.setInsertPoint(condBlock);
        builder_.emitCondBr(asBool(genCond(*stmt.cond)), bodyBlock.name, endBlock.name);

        breakTargets_.push(endBlock.name);
        continueTargets_.push(condBlock.name);
        builder_.setInsertPoint(bodyBlock);
        genStmt(*stmt.stmt);
        builder_.emitBr(condBlock.name);
        breakTargets_.pop();
        continueTargets_.pop();

        builder_.setInsertPoint(endBlock);
        SymTab::leaveRecordedScope();
    }

    void genFor(const BigForStmt &stmt) {
        SymTab::enterRecordedScope();
        if (stmt.init) {
            storeLVal(*stmt.init->lVal, genExp(*stmt.init->exp));
        }
        auto &condBlock = builder_.createBlock("for.cond");
        auto &bodyBlock = builder_.createBlock("for.body");
        auto &iterBlock = builder_.createBlock("for.iter");
        auto &endBlock = builder_.createBlock("for.end");
        builder_.emitBr(condBlock.name);

        builder_.setInsertPoint(condBlock);
        if (stmt.cond) {
            builder_.emitCondBr(asBool(genCond(*stmt.cond)), bodyBlock.name, endBlock.name);
        } else {
            builder_.emitBr(bodyBlock.name);
        }

        breakTargets_.push(endBlock.name);
        continueTargets_.push(iterBlock.name);
        builder_.setInsertPoint(bodyBlock);
        genStmt(*stmt.stmt);
        builder_.emitBr(iterBlock.name);

        builder_.setInsertPoint(iterBlock);
        if (stmt.iter) {
            storeLVal(*stmt.iter->lVal, genExp(*stmt.iter->exp));
        }
        builder_.emitBr(condBlock.name);
        breakTargets_.pop();
        continueTargets_.pop();

        builder_.setInsertPoint(endBlock);
        SymTab::leaveRecordedScope();
    }

    void genPrint(const PrintStmt &stmt) {
        for (size_t i = 0; i < stmt.exps.size() && i < stmt.formatTypes.size(); ++i) {
            Operand value = genExp(*stmt.exps[i]);
            if (stmt.formatTypes[i] == 'd') {
                builder_.emitCall(Type::Void, "put_int", {coerce(value, Type::Int)});
            } else if (stmt.formatTypes[i] == 'c') {
                builder_.emitCall(Type::Void, "put_char", {coerce(value, Type::Char)});
            } else {
                builder_.emitComment("string printf argument lowering pending");
            }
        }
    }

    Operand genCond(const Cond &cond) {
        return genLOr(*cond.lorExp);
    }

    Operand genExp(const Exp &exp) {
        return genAdd(*exp.addExp);
    }

    Operand genAdd(const AddExp &exp) {
        Operand value = genMul(*exp.first);
        for (size_t i = 0; i < exp.ops.size(); ++i) {
            Operand rhs = genMul(*exp.elements[i]);
            value = builder_.emitBinary(binOp(exp.ops[i]), Type::Int, asInt(value), asInt(rhs), "add");
        }
        return value;
    }

    Operand genMul(const MulExp &exp) {
        Operand value = genUnary(*exp.first);
        for (size_t i = 0; i < exp.ops.size(); ++i) {
            Operand rhs = genUnary(*exp.elements[i]);
            value = builder_.emitBinary(binOp(exp.ops[i]), Type::Int, asInt(value), asInt(rhs), "mul");
        }
        return value;
    }

    Operand genRel(const RelExp &exp) {
        Operand value = genAdd(*exp.first);
        for (size_t i = 0; i < exp.ops.size(); ++i) {
            Operand rhs = genAdd(*exp.elements[i]);
            value = builder_.emitICmp(cmpOp(exp.ops[i]), asInt(value), asInt(rhs), "rel");
        }
        return value;
    }

    Operand genEq(const EqExp &exp) {
        Operand value = genRel(*exp.first);
        for (size_t i = 0; i < exp.ops.size(); ++i) {
            Operand rhs = genRel(*exp.elements[i]);
            value = builder_.emitICmp(cmpOp(exp.ops[i]), asInt(value), asInt(rhs), "eq");
        }
        return value;
    }

    Operand genLAnd(const LAndExp &exp) {
        Operand value = asBool(genEq(*exp.first));
        for (size_t i = 0; i < exp.ops.size(); ++i) {
            Operand rhs = asBool(genEq(*exp.elements[i]));
            value = builder_.emitBinary("and", Type::Int, asInt(value), asInt(rhs), "and");
        }
        return value;
    }

    Operand genLOr(const LOrExp &exp) {
        Operand value = asBool(genLAnd(*exp.first));
        for (size_t i = 0; i < exp.ops.size(); ++i) {
            Operand rhs = asBool(genLAnd(*exp.elements[i]));
            value = builder_.emitBinary("or", Type::Int, asInt(value), asInt(rhs), "or");
        }
        return value;
    }

    Operand genUnary(const UnaryExp &exp) {
        Operand value = genBase(*exp.baseUnaryExp);
        for (LexType op: exp.ops) {
            if (op == LexType::MINU) {
                value = builder_.emitBinary("sub", Type::Int, Operand::constant(Type::Int, 0), asInt(value), "neg");
            } else if (op == LexType::NOT) {
                value = builder_.emitICmp("eq", asInt(value), Operand::constant(Type::Int, 0), "not");
            }
        }
        return value;
    }

    Operand genBase(BaseUnaryExp &base) {
        if (auto *number = dynamic_cast<Number *>(&base)) {
            return Operand::constant(number->getType(), number->intConst);
        }
        if (auto *lVal = dynamic_cast<LVal *>(&base)) {
            return loadLVal(*lVal);
        }
        if (auto *paren = dynamic_cast<PareExp *>(&base)) {
            return genExp(*paren->exp);
        }
        if (auto *call = dynamic_cast<FuncCall *>(&base)) {
            return genCall(*call);
        }
        if (auto *cast = dynamic_cast<CastExp *>(&base)) {
            return coerce(genUnary(*cast->unaryExp), cast->targetType);
        }
        Error::raise("unsupported expression node");
        return Operand::constant(Type::Int, 0);
    }

    Operand genCall(FuncCall &call) {
        std::vector<Operand> args;
        if (call.funcRParams) {
            args.reserve(call.funcRParams->params.size());
            for (const auto &param: call.funcRParams->params) {
                args.push_back(genExp(*param));
            }
        }
        Type returnType = call.getType();
        return builder_.emitCall(returnType, call.ident, std::move(args), call.ident);
    }

    Storage *lookupStorage(const std::string &ident) {
        auto [symbol, depth] = SymTab::findWithDepth(ident);
        (void) depth;
        if (!symbol) {
            return nullptr;
        }
        auto it = storage_.find(symbol);
        if (it != storage_.end()) {
            return &it->second;
        }
        auto *value = symbol->asValue();
        if (value) {
            storage_[symbol] = {Operand("ptr", "@" + ident), ptrToValue(value->getType()), value->getDims(), false};
            return &storage_[symbol];
        }
        return nullptr;
    }

    Operand loadLVal(const LVal &lVal) {
        auto *storage = lookupStorage(lVal.ident);
        if (!storage) {
            return Operand::constant(Type::Int, 0);
        }
        if (!lVal.dims.empty() || !storage->dims.empty()) {
            builder_.emitComment("array load lowering pending for " + lVal.ident);
            return Operand::constant(storage->valueType, 0);
        }
        return builder_.emitLoad(storage->valueType, storage->ptr, lVal.ident);
    }

    void storeLVal(const LVal &lVal, Operand value) {
        auto *storage = lookupStorage(lVal.ident);
        if (!storage) {
            return;
        }
        if (!lVal.dims.empty() || !storage->dims.empty()) {
            builder_.emitComment("array store lowering pending for " + lVal.ident);
            return;
        }
        builder_.emitStore(coerce(std::move(value), storage->valueType), storage->ptr);
    }

    Operand asInt(Operand value) {
        if (value.type == "i32") {
            return value;
        }
        if (value.type == "i8") {
            return builder_.emitCast("zext", Type::Int, std::move(value), "zext");
        }
        if (value.type == "i1") {
            return builder_.emitCast("zext", Type::Int, std::move(value), "bool");
        }
        return value;
    }

    Operand asBool(Operand value) {
        if (value.type == "i1") {
            return value;
        }
        return builder_.emitICmp("ne", asInt(std::move(value)), Operand::constant(Type::Int, 0), "tobool");
    }

    Operand coerce(Operand value, Type targetType) {
        std::string target = typeToIR(targetType);
        if (value.type == target) {
            return value;
        }
        if (targetType == Type::Char) {
            return builder_.emitCast("trunc", Type::Char, asInt(std::move(value)), "trunc");
        }
        if (targetType == Type::Int) {
            return asInt(std::move(value));
        }
        return value;
    }
};

} // namespace

Module generateModule(const CompUnit &compUnit) {
    return Generator().generate(compUnit);
}

} // namespace IR
