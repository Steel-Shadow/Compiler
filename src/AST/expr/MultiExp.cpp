//
// Created by Steel_Shadow on 2023/10/12.
//
#include "Exp.h"
#include "frontend/parser/Parser.h"

using namespace Parser;

std::unique_ptr<Cond> Cond::parse() {
    auto n = std::make_unique<Cond>();

    n->lorExp = LOrExp::parse();
    n->lorExp->getType();

    output(AST::Cond);
    return n;
}

std::unique_ptr<MulExp> MulExp::parse() {
    auto n = std::make_unique<MulExp>();

    n->first = UnaryExp::parse();
    output(AST::MulExp);

    while (Lexer::curLexType == LexType::MULT || Lexer::curLexType == LexType::DIV || Lexer::curLexType == LexType::MOD) {
        n->ops.push_back(Lexer::curLexType);
        Lexer::next();
        n->elements.push_back(UnaryExp::parse());
        output(AST::MulExp);
    }

    return n;
}

int MulExp::evaluate() const {
    int val = first->evaluate();
    if (Exp::getNonConstValueInEvaluate) {
        return 0;
    }
    for (size_t i = 0; i < ops.size(); ++i) {
        auto op = ops[i];
        auto e = elements[i]->evaluate();
        if (Exp::getNonConstValueInEvaluate) {
            return 0;
        }
        if (op == LexType::MULT) {
            val *= e;
        } else if (op == LexType::DIV) {
            if (e == 0) {
                return 0;
            }
            val /= e;
        } else if (op == LexType::MOD) {
            if (e == 0) {
                return 0;
            }
            val %= e;
        }
    }
    return val;
}

std::unique_ptr<AddExp> AddExp::parse() {
    auto n = std::make_unique<AddExp>();

    n->first = MulExp::parse();
    output(AST::AddExp);

    while (Lexer::curLexType == LexType::PLUS || Lexer::curLexType == LexType::MINU) {
        n->ops.push_back(Lexer::curLexType);
        Lexer::next();
        n->elements.push_back(MulExp::parse());
        output(AST::AddExp);
    }

    return n;
}

int AddExp::evaluate() const {
    int val = first->evaluate();
    if (Exp::getNonConstValueInEvaluate) {
        return 0;
    }
    for (size_t i = 0; i < ops.size(); ++i) {
        auto op = ops[i];
        auto e = elements[i]->evaluate();
        if (Exp::getNonConstValueInEvaluate) {
            return 0;
        }
        if (op == LexType::PLUS) {
            val += e;
        } else if (op == LexType::MINU) {
            val -= e;
        }
    }
    return val;
}

std::unique_ptr<RelExp> RelExp::parse() {
    auto n = std::make_unique<RelExp>();

    n->first = AddExp::parse();
    output(AST::RelExp);

    while (Lexer::curLexType == LexType::LSS || Lexer::curLexType == LexType::GRE || Lexer::curLexType == LexType::LEQ || Lexer::curLexType == LexType::GEQ) {
        n->ops.push_back(Lexer::curLexType);
        Lexer::next();
        n->elements.push_back(AddExp::parse());
        output(AST::RelExp);
    }

    return n;
}

std::unique_ptr<EqExp> EqExp::parse() {
    auto n = std::make_unique<EqExp>();

    n->first = RelExp::parse();
    output(AST::EqExp);

    while (Lexer::curLexType == LexType::EQL || Lexer::curLexType == LexType::NEQ) {
        n->ops.push_back(Lexer::curLexType);
        Lexer::next();
        n->elements.push_back(RelExp::parse());
        output(AST::EqExp);
    }

    return n;
}

std::unique_ptr<LAndExp> LAndExp::parse() {
    auto n = std::make_unique<LAndExp>();

    n->first = EqExp::parse();
    output(AST::LAndExp);

    while (Lexer::curLexType == LexType::AND) {
        n->ops.push_back(Lexer::curLexType);
        Lexer::next();
        n->elements.push_back(EqExp::parse());
        output(AST::LAndExp);
    }

    return n;
}

std::unique_ptr<LOrExp> LOrExp::parse() {
    auto n = std::make_unique<LOrExp>();

    n->first = LAndExp::parse();
    output(AST::LOrExp);

    while (Lexer::curLexType == LexType::OR) {
        n->ops.push_back(Lexer::curLexType);
        Lexer::next();
        n->elements.push_back(LAndExp::parse());
        output(AST::LOrExp);
    }

    return n;
}
