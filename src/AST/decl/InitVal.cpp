//
// Created by Steel_Shadow on 2023/10/11.
//

#include "InitVal.h"

#include "AST/expr/Exp.h"
#include "frontend/lexer/Lexer.h"
#include "frontend/parser/Parser.h"

using namespace Parser;

std::unique_ptr<InitVal> InitVal::parse(bool cons) {
    std::unique_ptr<InitVal> n;

    if (Lexer::curLexType == LexType::LBRACE) {
        n = ArrayInitVal::parse(cons);
    } else if (Lexer::curLexType == LexType::STRCON) {
        n = StringInitVal::parse(cons);
    } else {
        n = ExpInitVal::parse(cons);
    }

    if (cons) {
        output(AST::ConstInitVal);
    } else {
        output(AST::InitVal);
    }

    return n;
}

std::unique_ptr<ExpInitVal> ExpInitVal::parse(bool cons) {
    auto n = std::make_unique<ExpInitVal>();
    n->cons = cons;

    n->exp = Exp::parse(cons);
    return n;
}

std::vector<int> ExpInitVal::evaluate() {
    return std::vector{exp->evaluate()};
}

std::unique_ptr<ArrayInitVal> ArrayInitVal::parse(bool cons) {
    auto n = std::make_unique<ArrayInitVal>();
    n->cons = cons;
    singleLex(LexType::LBRACE);

    if (Lexer::curLexType != LexType::RBRACE) {
        n->array.push_back(ExpInitVal::parse(cons));

        while (Lexer::curLexType == LexType::COMMA) {
            Lexer::next();
            n->array.push_back(ExpInitVal::parse(cons));
        }
    }

    singleLex(LexType::RBRACE);
    return n;
}

std::vector<int> ArrayInitVal::evaluate() {
    std::vector<int> res;

    for (auto &i: array) {
        auto t = i->evaluate();
        res.insert(res.end(), t.begin(), t.end());
    }

    return res;
}

std::vector<ExpInitVal *> ArrayInitVal::getFlatten() const {
    std::vector<ExpInitVal *> flatten;
    flatten.reserve(array.size());
    for (auto &i: array) {
        flatten.push_back(i.get());
    }
    return flatten;
}

std::unique_ptr<StringInitVal> StringInitVal::parse(bool cons) {
    auto n = std::make_unique<StringInitVal>();
    n->cons = cons;
    n->stringConst = Lexer::curToken;
    Lexer::next();
    return n;
}

std::vector<int> StringInitVal::evaluate() {
    std::vector<int> bytes;
    for (size_t i = 1; i + 1 < stringConst.length(); ++i) {
        if (stringConst[i] == '\\' && i + 2 < stringConst.length()) {
            ++i;
            if (stringConst[i] == 'n') {
                bytes.push_back('\n');
            } else {
                bytes.push_back(stringConst[i]);
            }
        } else {
            bytes.push_back(static_cast<unsigned char>(stringConst[i]));
        }
    }
    return bytes;
}
