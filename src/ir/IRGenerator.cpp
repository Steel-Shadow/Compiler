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
    int stringId_{0};

    static int arraySize(const std::vector<int> &dims) {
        int size = 1;
        for (int dim: dims) {
            size *= dim;
        }
        return size;
    }

    static std::vector<ExpInitVal *> flattenInit(const InitVal *initVal) {
        if (auto *exp = dynamic_cast<const ExpInitVal *>(initVal)) {
            return {const_cast<ExpInitVal *>(exp)};
        }
        if (auto *array = dynamic_cast<const ArrayInitVal *>(initVal)) {
            return array->getFlatten();
        }
        return {};
    }

    static std::vector<int> stringBytes(const std::string &token, bool nullTerminate) {
        std::vector<int> bytes;
        for (size_t i = 1; i + 1 < token.length(); ++i) {
            if (token[i] == '\\' && i + 2 < token.length()) {
                ++i;
                if (token[i] == 'n') {
                    bytes.push_back('\n');
                } else {
                    bytes.push_back(static_cast<unsigned char>(token[i]));
                }
            } else {
                bytes.push_back(static_cast<unsigned char>(token[i]));
            }
        }
        if (nullTerminate) {
            bytes.push_back(0);
        }
        return bytes;
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
            if (builder_.block() && builder_.block()->terminated()) {
                consumeScopes(*item);
                continue;
            }
            if (auto *decl = dynamic_cast<Decl *>(item.get())) {
                genDecl(*decl);
            } else if (auto *stmt = dynamic_cast<Stmt *>(item.get())) {
                genStmt(*stmt);
            }
        }
    }

    void consumeScopes(const BlockItem &item) {
        auto *stmt = dynamic_cast<const Stmt *>(&item);
        if (!stmt) {
            return;
        }
        consumeStmtScopes(*stmt);
    }

    void consumeBlockScopes(const Block &block) {
        for (const auto &item: block.blockItems) {
            consumeScopes(*item);
        }
    }

    void consumeStmtScopes(const Stmt &stmt) {
        if (auto *blockStmt = dynamic_cast<const BlockStmt *>(&stmt)) {
            SymTab::enterRecordedScope();
            consumeBlockScopes(*blockStmt->block);
            SymTab::leaveRecordedScope();
        } else if (auto *ifStmt = dynamic_cast<const IfStmt *>(&stmt)) {
            SymTab::enterRecordedScope();
            consumeStmtScopes(*ifStmt->ifStmt);
            if (ifStmt->elseStmt) {
                consumeStmtScopes(*ifStmt->elseStmt);
            }
            SymTab::leaveRecordedScope();
        } else if (auto *whileStmt = dynamic_cast<const WhileStmt *>(&stmt)) {
            SymTab::enterRecordedScope();
            consumeStmtScopes(*whileStmt->stmt);
            SymTab::leaveRecordedScope();
        } else if (auto *forStmt = dynamic_cast<const BigForStmt *>(&stmt)) {
            SymTab::enterRecordedScope();
            consumeStmtScopes(*forStmt->stmt);
            SymTab::leaveRecordedScope();
        } else if (auto *switchStmt = dynamic_cast<const SwitchStmt *>(&stmt)) {
            for (const auto &caseStmt: switchStmt->cases) {
                for (const auto &caseBodyStmt: caseStmt->stmts) {
                    consumeStmtScopes(*caseBodyStmt);
                }
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
            initArray(def, type, *value);
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

    void initArray(const Def &def, Type type, const ValueSymbol &symbol) {
        if (!def.initVal) {
            return;
        }
        int index = 0;
        int total = arraySize(symbol.getDims());
        if (auto *str = dynamic_cast<StringInitVal *>(def.initVal.get())) {
            for (int value: str->evaluate()) {
                if (index >= total) {
                    break;
                }
                storeArrayElement(def.ident, index++, Operand::constant(type, value), type);
            }
        } else {
            for (auto *expInit: flattenInit(def.initVal.get())) {
                if (index >= total) {
                    break;
                }
                storeArrayElement(def.ident, index++, genExp(*expInit->exp), type);
            }
        }
    }

    void storeArrayElement(const std::string &ident, int index, Operand value, Type type) {
        auto *storage = lookupStorage(ident);
        if (!storage) {
            return;
        }
        Operand ptr = builder_.emitGetElementPtr(type, storage->ptr, Operand::constant(Type::Int, index), ident + ".init");
        builder_.emitStore(coerce(std::move(value), type), std::move(ptr));
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
            genSwitch(*switchStmt);
        }
    }

    void genIf(const IfStmt &stmt) {
        SymTab::enterRecordedScope();
        auto &thenBlock = builder_.createBlock("if.then");
        auto &elseBlock = builder_.createBlock("if.else");
        auto &endBlock = builder_.createBlock("if.end");
        genCondBr(*stmt.cond, thenBlock.name, elseBlock.name);

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
        genCondBr(*stmt.cond, bodyBlock.name, endBlock.name);

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
            genCondBr(*stmt.cond, bodyBlock.name, endBlock.name);
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

    void genSwitch(const SwitchStmt &stmt) {
        Operand switchValue = asInt(genExp(*stmt.exp));
        std::vector<BasicBlock *> caseBlocks;
        caseBlocks.reserve(stmt.cases.size());
        BasicBlock *defaultBlock = nullptr;
        for (size_t i = 0; i < stmt.cases.size(); ++i) {
            auto &block = builder_.createBlock(stmt.cases[i]->isDefault ? "switch.default" : "switch.case");
            if (stmt.cases[i]->isDefault) {
                defaultBlock = &block;
            }
            caseBlocks.push_back(&block);
        }
        auto &endBlock = builder_.createBlock("switch.end");

        for (size_t i = 0; i < stmt.cases.size(); ++i) {
            if (stmt.cases[i]->isDefault) {
                continue;
            }
            Operand caseValue = Operand::constant(stmt.cases[i]->number->getType(), stmt.cases[i]->number->evaluate());
            Operand matched = builder_.emitICmp("eq", switchValue, asInt(std::move(caseValue)), "switchcmp");
            auto &nextCheck = builder_.createBlock("switch.next");
            builder_.emitCondBr(std::move(matched), caseBlocks[i]->name, nextCheck.name);
            builder_.setInsertPoint(nextCheck);
        }
        builder_.emitBr(defaultBlock ? defaultBlock->name : endBlock.name);

        breakTargets_.push(endBlock.name);
        for (size_t i = 0; i < stmt.cases.size(); ++i) {
            builder_.setInsertPoint(*caseBlocks[i]);
            for (const auto &caseStmt: stmt.cases[i]->stmts) {
                genStmt(*caseStmt);
            }
            if (!builder_.block()->terminated()) {
                if (i + 1 < caseBlocks.size()) {
                    builder_.emitBr(caseBlocks[i + 1]->name);
                } else {
                    builder_.emitBr(endBlock.name);
                }
            }
        }
        breakTargets_.pop();
        builder_.setInsertPoint(endBlock);
    }

    void genPrint(const PrintStmt &stmt) {
        std::vector<Operand> args;
        args.reserve(stmt.exps.size());
        for (const auto &exp: stmt.exps) {
            args.push_back(genExp(*exp));
        }

        std::string literal;
        size_t argIndex = 0;
        for (size_t i = 1; i + 1 < stmt.formatString.size(); ++i) {
            if (stmt.formatString[i] == '%') {
                emitStringSegment(literal);
                char fmt = stmt.formatString[++i];
                if (argIndex >= args.size()) {
                    continue;
                }
                Operand arg = args[argIndex++];
                if (fmt == 'd') {
                    builder_.emitCall(Type::Void, "put_int", {coerce(std::move(arg), Type::Int)});
                } else if (fmt == 'c') {
                    builder_.emitCall(Type::Void, "put_char", {coerce(std::move(arg), Type::Char)});
                } else if (fmt == 's') {
                    builder_.emitCall(Type::Void, "put_str", {std::move(arg)});
                }
            } else {
                if (stmt.formatString[i] == '\\' && i + 2 < stmt.formatString.size()) {
                    literal += '\\';
                    literal += stmt.formatString[++i];
                } else {
                    literal += stmt.formatString[i];
                }
            }
        }
        emitStringSegment(literal);
    }

    void emitStringSegment(std::string &literal) {
        if (literal.empty()) {
            return;
        }
        std::string token = "\"" + literal + "\"";
        GlobalVar global;
        global.name = "__str_" + std::to_string(stringId_++);
        global.elementType = Type::Char;
        global.dims = {static_cast<int>(stringBytes(token, true).size())};
        global.init = stringBytes(token, true);
        module_.addGlobal(global);
        builder_.emitCall(Type::Void, "put_str", {Operand("ptr", "@" + global.name)});
        literal.clear();
    }

    Operand genCond(const Cond &cond) {
        auto &trueBlock = builder_.createBlock("cond.true");
        auto &falseBlock = builder_.createBlock("cond.false");
        auto &endBlock = builder_.createBlock("cond.end");
        genCondBr(cond, trueBlock.name, falseBlock.name);
        return boolPhi("cond", trueBlock, falseBlock, endBlock);
    }

    void genCondBr(const Cond &cond, const std::string &trueTarget, const std::string &falseTarget) {
        genLOrBr(*cond.lorExp, trueTarget, falseTarget);
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
        if (exp.elements.empty()) {
            return asBool(genEq(*exp.first));
        }

        auto &falseBlock = builder_.createBlock("land.false");
        auto &trueBlock = builder_.createBlock("land.true");
        auto &endBlock = builder_.createBlock("land.end");
        genLAndBr(exp, trueBlock.name, falseBlock.name);
        return boolPhi("land", trueBlock, falseBlock, endBlock);
    }

    Operand genLOr(const LOrExp &exp) {
        if (exp.elements.empty()) {
            return genLAnd(*exp.first);
        }

        auto &trueBlock = builder_.createBlock("lor.true");
        auto &falseBlock = builder_.createBlock("lor.false");
        auto &endBlock = builder_.createBlock("lor.end");
        genLOrBr(exp, trueBlock.name, falseBlock.name);
        return boolPhi("lor", trueBlock, falseBlock, endBlock);
    }

    void genLOrBr(const LOrExp &exp, const std::string &trueTarget, const std::string &falseTarget) {
        if (exp.elements.empty()) {
            genLAndBr(*exp.first, trueTarget, falseTarget);
            return;
        }

        auto &firstNext = builder_.createBlock("lor.next");
        genLAndBr(*exp.first, trueTarget, firstNext.name);
        builder_.setInsertPoint(firstNext);
        for (size_t i = 0; i + 1 < exp.elements.size(); ++i) {
            auto &next = builder_.createBlock("lor.next");
            genLAndBr(*exp.elements[i], trueTarget, next.name);
            builder_.setInsertPoint(next);
        }
        genLAndBr(*exp.elements.back(), trueTarget, falseTarget);
    }

    void genLAndBr(const LAndExp &exp, const std::string &trueTarget, const std::string &falseTarget) {
        if (exp.elements.empty()) {
            builder_.emitCondBr(asBool(genEq(*exp.first)), trueTarget, falseTarget);
            return;
        }

        auto &firstNext = builder_.createBlock("land.next");
        builder_.emitCondBr(asBool(genEq(*exp.first)), firstNext.name, falseTarget);
        builder_.setInsertPoint(firstNext);
        for (size_t i = 0; i + 1 < exp.elements.size(); ++i) {
            auto &next = builder_.createBlock("land.next");
            builder_.emitCondBr(asBool(genEq(*exp.elements[i])), next.name, falseTarget);
            builder_.setInsertPoint(next);
        }
        builder_.emitCondBr(asBool(genEq(*exp.elements.back())), trueTarget, falseTarget);
    }

    Operand boolPhi(const std::string &hint, BasicBlock &trueBlock, BasicBlock &falseBlock, BasicBlock &endBlock) {
        builder_.setInsertPoint(trueBlock);
        builder_.emitBr(endBlock.name);
        builder_.setInsertPoint(falseBlock);
        builder_.emitBr(endBlock.name);

        builder_.setInsertPoint(endBlock);
        std::string result = builder_.function()->newTemp(hint);
        builder_.block()->add(Instruction::phi(result, Type::Int, {
                {Operand::constant(Type::Int, 1), trueBlock.name},
                {Operand::constant(Type::Int, 0), falseBlock.name},
        }));
        return Operand(typeToIR(Type::Int), result);
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
            const auto &params = call.funcRParams->params;
            args.resize(params.size());
            for (size_t i = params.size(); i-- > 0;) {
                args[i] = genExp(*params[i]);
            }
        }
        Type returnType = call.getType();
        return builder_.emitCall(returnType, call.ident, std::move(args), call.ident);
    }

    Storage *lookupStorage(const std::string &ident) {
        for (auto [symbol, depth]: SymTab::findAllWithDepth(ident)) {
            auto it = storage_.find(symbol);
            if (it != storage_.end()) {
                return &it->second;
            }

            auto *value = symbol->asValue();
            if (value && depth == 0) {
                storage_[symbol] = {Operand("ptr", "@" + ident), ptrToValue(value->getType()), value->getDims(), false};
                return &storage_[symbol];
            }
        }
        return nullptr;
    }

    Operand loadLVal(const LVal &lVal) {
        auto *storage = lookupStorage(lVal.ident);
        if (!storage) {
            return Operand::constant(Type::Int, 0);
        }
        if (!storage->dims.empty()) {
            if (lVal.dims.empty()) {
                return storage->ptr;
            }
            return builder_.emitLoad(storage->valueType, addressOfLVal(lVal, *storage), lVal.ident);
        }
        return builder_.emitLoad(storage->valueType, storage->ptr, lVal.ident);
    }

    void storeLVal(const LVal &lVal, Operand value) {
        auto *storage = lookupStorage(lVal.ident);
        if (!storage) {
            return;
        }
        if (!storage->dims.empty()) {
            builder_.emitStore(coerce(std::move(value), storage->valueType), addressOfLVal(lVal, *storage));
            return;
        }
        builder_.emitStore(coerce(std::move(value), storage->valueType), storage->ptr);
    }

    Operand addressOfLVal(const LVal &lVal, const Storage &storage) {
        if (lVal.dims.empty()) {
            return storage.ptr;
        }
        Operand index = linearIndex(lVal, storage.dims);
        return builder_.emitGetElementPtr(storage.valueType, storage.ptr, std::move(index), lVal.ident + ".elem");
    }

    Operand linearIndex(const LVal &lVal, const std::vector<int> &dims) {
        Operand total;
        bool hasTerm = false;
        int product = 1;
        for (size_t i = dims.size(); i-- > 0;) {
            if (i + 1 < dims.size()) {
                product *= dims[i + 1];
            }
            if (i >= lVal.dims.size()) {
                continue;
            }
            Operand part = asInt(genExp(*lVal.dims[i]));
            if (product != 1) {
                part = builder_.emitBinary("mul", Type::Int, std::move(part), Operand::constant(Type::Int, product), "idxmul");
            }
            if (!hasTerm) {
                total = std::move(part);
                hasTerm = true;
            } else {
                total = builder_.emitBinary("add", Type::Int, std::move(total), std::move(part), "idx");
            }
        }
        return hasTerm ? total : Operand::constant(Type::Int, 0);
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
