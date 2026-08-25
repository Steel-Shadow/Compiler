//
// Created by Steel_Shadow on 2023/10/11.
//
#include "Exp.h"
#include "errorHandler/Error.h"
#include "frontend/parser/Parser.h"
#include "frontend/symTab/SymTab.h"

#include <algorithm>


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

IR::Op binaryOpToIROp(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:
            return IR::Op::Add;
        case BinaryOp::Sub:
            return IR::Op::Sub;
        case BinaryOp::Mul:
            return IR::Op::Mul;
        case BinaryOp::Div:
            return IR::Op::Div;
        case BinaryOp::Mod:
            return IR::Op::Mod;
        case BinaryOp::And:
            return IR::Op::And;
        case BinaryOp::Or:
            return IR::Op::Or;
        case BinaryOp::Leq:
            return IR::Op::Leq;
        case BinaryOp::Lss:
            return IR::Op::Lss;
        case BinaryOp::Geq:
            return IR::Op::Geq;
        case BinaryOp::Gre:
            return IR::Op::Gre;
        case BinaryOp::Eql:
            return IR::Op::Eql;
        case BinaryOp::Neq:
            return IR::Op::Neq;
        default:
            Error::raise("Bad IR Operator");
            return IR::Op::Empty;
    }
}

bool binaryOpProducesInt(BinaryOp op) {
    switch (op) {
        case BinaryOp::And:
        case BinaryOp::Or:
        case BinaryOp::Leq:
        case BinaryOp::Lss:
        case BinaryOp::Geq:
        case BinaryOp::Gre:
        case BinaryOp::Eql:
        case BinaryOp::Neq:
            return true;
        default:
            return false;
    }
}

Type resolveMultiExpType(Type firstType,
                         size_t firstRemainingRank,
                         const std::vector<Type> &elementTypes,
                         const std::vector<size_t> &elementRemainingRanks,
                         const std::vector<BinaryOp> &ops) {
    if (firstType == Type::Invalid
        || std::find(elementTypes.begin(), elementTypes.end(), Type::Invalid) != elementTypes.end()) {
        return Type::Invalid;
    }

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
    if (!ops.empty() && binaryOpProducesInt(ops.back())) {
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

std::unique_ptr<IR::Temp> Exp::genIR(IR::BasicBlocks &bBlocks) const {
    return addExp->genIR(bBlocks);
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
