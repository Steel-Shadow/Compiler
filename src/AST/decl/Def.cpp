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
    int defRow = row;
    n->ident = Ident::parse();

    bool redefined = SymTab::reDefine(n->ident);
    if (redefined) {
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

    std::string storageName;
    if (statik && !redefined) {
        static int staticId = 0;
        storageName = "__static_" + n->ident + "_" + std::to_string(staticId++);
    }

    std::vector<int>::size_type size = 1;
    for (auto &i: dims) {
        size *= i;
    }

    ValueSymbol *declaredValue = nullptr;
    if (!redefined) {
        auto symbol = std::make_unique<ValueSymbol>(n->cons, type, dims, std::vector<int>(size), n->statik, storageName);
        declaredValue = symbol.get();
        SymTab::add(n->ident, std::move(symbol));
    }

    if (cons || Lexer::curLexType == LexType::ASSIGN) {
        Lexer::next();
        n->initVal = InitVal::parse(cons);
    }

    if (n->initVal) {
        if (dims.empty()) {
            if (auto expInit = dynamic_cast<ExpInitVal *>(n->initVal.get())) {
                if (expInit->exp->getType() != type) {
                    Error::raise('e', defRow);
                }
            } else {
                Error::raise('e', defRow);
            }
        } else if (auto array = dynamic_cast<ArrayInitVal *>(n->initVal.get())) {
            for (auto &expInit: array->getFlatten()) {
                if (expInit->exp->getType() != type) {
                    Error::raise('e', defRow);
                    break;
                }
            }
        } else if (dynamic_cast<StringInitVal *>(n->initVal.get())) {
            if (type != Type::Char) {
                Error::raise('e', defRow);
            }
        }
    }

    if (declaredValue && (cons || statik || SymTab::currentDepth() == 0)) {
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
            declaredValue->setInitVal(std::move(values));
        } else {
            declaredValue->setInitVal(std::vector<int>(size));
        }
    } else if (declaredValue) {
        declaredValue->setInitVal({});
    }

    if (cons) {
        output(AST::ConstDef);
    } else {
        output(AST::VarDef);
    }
    return n;
}

const std::string &Def::getIdent() const {
    return ident;
}

const std::unique_ptr<InitVal> &Def::getInitVal() const {
    return initVal;
}
