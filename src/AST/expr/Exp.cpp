//
// Created by Steel_Shadow on 2023/10/11.
//
#include "Exp.h"
#include "errorHandler/Error.h"
#include "frontend/parser/Parser.h"
#include "frontend/symTab/SymTab.h"


using namespace Parser;

std::unique_ptr<Exp> Exp::parse(bool cons) {
    auto n = std::make_unique<Exp>();
    n->cons = cons;
    n->addExp = AddExp::parse();

    if (cons) {
        output(AST::ConstExp);
    } else {
        output(AST::Exp);
    }
    return n;
}

bool Exp::getNonConstValueInEvaluate = false;

bool opProducesInt(LexType type) {
    return type == LexType::LSS || type == LexType::GRE || type == LexType::LEQ || type == LexType::GEQ
           || type == LexType::EQL || type == LexType::NEQ || type == LexType::AND || type == LexType::OR;
}

Type resolveMultiExpType(Type firstType,
                         size_t firstRemainingRank,
                         const std::vector<Type> &elementTypes,
                         const std::vector<size_t> &elementRemainingRanks,
                         const std::vector<LexType> &ops) {
    bool typeMismatch = false;
    for (size_t i = 0; i < elementTypes.size(); ++i) {
        if (firstRemainingRank > 0 || elementRemainingRanks[i] > 0
            || firstType == Type::Void || elementTypes[i] == Type::Void
            || isPtrType(firstType) || isPtrType(elementTypes[i])
            || elementTypes[i] != firstType) {
            typeMismatch = true;
        }
    }
    if (typeMismatch) {
        Error::raise('e');
        return Type::Void;
    }
    if (!ops.empty() && opProducesInt(ops.back())) {
        return Type::Int;
    }
    return firstType;
}

size_t remainingRankOf(LVal *lVal) {
    if (!lVal) {
        return 0;
    }
    auto *sym = SymTab::find(lVal->getIdent());
    auto *object = sym ? sym->asObject() : nullptr;
    if (!object || object->getDims().size() <= lVal->getRank()) {
        return 0;
    }
    return object->getDims().size() - lVal->getRank();
}

int Exp::evaluate() const {
    Exp::getNonConstValueInEvaluate = false;
    return addExp->evaluate();
}

size_t Exp::getRank() const {
    return addExp->getRank();
}

size_t Exp::getRemainingRank() const {
    return remainingRankOf(getLVal());
}

std::string Exp::getIdent() const {
    return addExp->getIdent();
}

Type Exp::getType() const {
    return addExp->getType();
}

LVal *Exp::getLVal() const {
    return addExp->getLVal();
}

int BaseUnaryExp::evaluate() {
    Exp::getNonConstValueInEvaluate = true;
    return 0;
}
