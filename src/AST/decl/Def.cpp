//
// Created by Steel_Shadow on 2023/10/11.
//
#include "Def.h"

#include "Decl.h"
#include "errorHandler/Error.h"
#include "frontend/parser/Parser.h"
#include "frontend/symTab/SymTab.h"


using namespace Parser;

std::unique_ptr<Def> Def::parse(bool cons, Type type, bool statik) {
    auto n = std::make_unique<Def>();
    n->cons = cons;
    n->statik = statik;

    int row = Lexer::curRow;
    n->ident = Ident::parse();

    if (SymTab::reDefine(n->ident)) {
        Error::raise('b', row);
    }

    std::vector<int> dims; // SymTab dims

    while (Lexer::curLexType == LexType::LBRACK) {
        Lexer::next();

        row = Lexer::curRow;
        auto exp = Exp::parse(true);
        dims.push_back(exp->evaluate());
        n->dims.push_back(std::move(exp));

        singleLex(LexType::RBRACK, row);
    }

    if (cons || Lexer::curLexType == LexType::ASSIGN) {
        Lexer::next();
        n->initVal = InitVal::parse(cons);
    }

    std::string storageName;
    if (statik) {
        static int staticId = 0;
        storageName = "__static_" + n->ident + "_" + std::to_string(staticId++);
    }

    if (cons || statik || SymTab::cur->getDepth() == 0) {
        int size = 1;
        for (auto &i: dims) {
            size *= i;
        }
        if (n->initVal) {
            auto values = n->initVal->evaluate();
            if (values.size() < size) {
                values.resize(size, 0);
            } else if (values.size() > size) {
                values.resize(size);
            }
            if (type == Type::Char) {
                for (auto &value: values) {
                    value &= 0xFF;
                }
            }
            SymTab::add(n->ident, Symbol(n->cons, type, dims, values, n->statik, storageName));
        } else {
            SymTab::add(n->ident, Symbol(n->cons, type, dims, std::vector<int>(size), n->statik, storageName));
        }
    } else {
        SymTab::add(n->ident, Symbol(n->cons, type, dims, {}, n->statik));
    }

    if (cons) {
        output(AST::ConstDef);
    } else {
        output(AST::VarDef);
    }
    return n;
}

void Def::genIR(IR::BasicBlocks &bBlocks, Type type) const {
    using namespace IR;

    auto var = std::make_unique<Var>(
            getStorageName(SymTab::find(ident), ident),
            getStorageDepth(SymTab::find(ident), SymTab::cur->getDepth()),
            cons,
            SymTab::find(ident)->dims,
            type);
    auto pVar = var.get();

    SymTab::knownVars.back().emplace(ident, SymTab::cur->getDepth());
    if (statik) {
        return;
    }

    auto size = std::make_unique<ConstVal>(getArraySize(), Type::Int);
    bBlocks.back()->addInst(Inst(
            Op::Alloca,
            nullptr,
            std::move(var),
            std::move(size)));

    if (initVal) {
        if (dims.empty()) {
            // single value assign
            auto *p = dynamic_cast<ExpInitVal *>(initVal.get());
            auto value = p->exp->genIR(bBlocks);

            bBlocks.back()->addInst(Inst(
                    Op::Store,
                    std::move(value),
                    std::make_unique<Var>(*pVar),
                    nullptr));
        } else {
            // array init
            auto array = dynamic_cast<ArrayInitVal *>(initVal.get());

            int index = 0;
            for (auto &expInit: array->getFlatten()) {
                auto value = expInit->exp->genIR(bBlocks);
                bBlocks.back()->addInst(Inst(
                        Op::Store,
                        std::move(value),
                        std::make_unique<Var>(*pVar),
                        std::make_unique<ConstVal>(index++, Type::Int)));
            }
        }
    }
}

const std::string &Def::getIdent() const {
    return ident;
}

const std::unique_ptr<InitVal> &Def::getInitVal() const {
    return initVal;
}
