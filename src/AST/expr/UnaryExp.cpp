//
// Created by Steel_Shadow on 2023/10/12.
//
#include "AST/decl/Decl.h"
#include "AST/func/Func.h"
#include "errorHandler/Error.h"
#include "Exp.h"
#include "frontend/parser/Parser.h"
#include "frontend/symTab/SymTab.h"


using namespace Parser;

int decodeCharConst(const std::string &token) {
    if (token.size() >= 4 && token[1] == '\\') {
        switch (token[2]) {
            case 'n':
                return '\n';
            case 't':
                return '\t';
            case 'r':
                return '\r';
            case '0':
                return '\0';
            case '\\':
                return '\\';
            case '\'':
                return '\'';
            default:
                Error::raise('a');
                return token[2];
        }
    }
    return token.size() >= 3 ? static_cast<unsigned char>(token[1]) : 0;
}

bool canStartExp(LexType type) {
    switch (type) {
        case LexType::PLUS:
        case LexType::MINU:
        case LexType::NOT:
        case LexType::LPARENT:
        case LexType::INTCON:
        case LexType::CHARCON:
        case LexType::IDENFR:
            return true;
        default:
            return false;
    }
}

std::unique_ptr<LVal> LVal::parse() {
    auto n = std::make_unique<LVal>();

    n->ident = Ident::parse();
    Symbol *symbol = SymTab::find(n->ident);
    const auto *object = symbol ? symbol->asObject() : nullptr;
    if (!symbol) {
        Error::raise('c');
    }

    while (Lexer::curLexType == LexType::LBRACK) {
        Lexer::next();
        int row = Lexer::curRow;
        auto index = Exp::parse(false);
        if (index->getType() != Type::Int) {
            Error::raise('e', row);
        }
        if (symbol && (!object || n->dims.size() + 1 > object->getDims().size())) {
            Error::raise('e', row);
        }
        n->dims.push_back(std::move(index));
        singleLex(LexType::RBRACK, row);
    }

    output(AST::LVal);
    return n;
}

size_t LVal::getRank() {
    return dims.size();
}

std::string LVal::getIdent() {
    return ident;
}

int LVal::evaluate() {
    auto sym = SymTab::find(ident);
    auto *value = sym ? sym->asValue() : nullptr;
    if (sym == nullptr) {
        Error::raise("LVal not found in evaluate()");
        return 0;
    } else if (!value || !value->isConst()) {
        Exp::getNonConstValueInEvaluate = true;
        // Non-const LVal in evaluate()
        return 0;
    } else if (value->getDims().empty()) {
        return value->getInitVal()[0];
    } else {
        Exp::getNonConstValueInEvaluate = true;
        // Const Array element in evaluate()
        return 0;
    }
}

Type LVal::getType() {
    auto sym = SymTab::find(ident);
    if (!sym) {
        return Type::Void;
    }
    auto *object = sym->asObject();
    if (!object) {
        return Type::Void;
    }
    if (isPtrType(object->getType()) && !dims.empty()) {
        return ptrToValue(object->getType());
    }
    return object->getType();
}

std::unique_ptr<PrimaryExp> PrimaryExp::parse() {
    std::unique_ptr<PrimaryExp> n;
    if (Lexer::curLexType == LexType::LPARENT) {
        n = PareExp::parse();
    } else if (Lexer::curLexType == LexType::IDENFR) {
        n = LVal::parse();
    } else if (Lexer::curLexType == LexType::INTCON || Lexer::curLexType == LexType::CHARCON) {
        n = Number::parse();
    }
    output(AST::PrimaryExp);
    return n;
}

size_t PrimaryExp::getRank() {
    if (auto p = dynamic_cast<LVal *>(this)) {
        return p->getRank();
    }
    return 0;
}

std::string PrimaryExp::getIdent() {
    if (auto p = dynamic_cast<LVal *>(this)) {
        return p->getIdent();
    }
    return "";
}

std::unique_ptr<PareExp> PareExp::parse() {
    auto n = std::make_unique<PareExp>();

    Lexer::next();
    int row = Lexer::curRow;
    n->exp = Exp::parse(false);
    singleLex(LexType::RPARENT, row);

    return n;
}

int PareExp::evaluate() {
    return exp->evaluate();
}

Type PareExp::getType() {
    return exp->getType();
}

std::unique_ptr<Number> Number::parse() {
    auto n = std::make_unique<Number>();

    if (Lexer::curLexType == LexType::INTCON) {
        n->intConst = std::stoi(Lexer::curToken);
        n->type = Type::Int;
        Lexer::next();
    } else if (Lexer::curLexType == LexType::CHARCON) {
        n->intConst = decodeCharConst(Lexer::curToken);
        n->type = Type::Char;
        Lexer::next();
    } else {
        Error::raise();
    }

    output(AST::Number);
    return n;
}

int Number::evaluate() {
    return intConst;
}

Type Number::getType() {
    return type;
}

std::unique_ptr<UnaryExp> UnaryExp::parse() {
    auto n = std::make_unique<UnaryExp>();

    bool getBaseUnaryExp = false;
    while (!getBaseUnaryExp) {
        // UnaryExp → {UnaryOp} ( PrimaryExp | Ident '(' [FuncRParams] ')' )
        switch (Lexer::curLexType) {
            case LexType::PLUS:
            case LexType::MINU:
            case LexType::NOT:
                // UnaryOp → '+' | '−' | '!'
                n->ops.push_back(Lexer::curLexType);
                Lexer::next();

                output(AST::UnaryOp);
                break;

            case LexType::LPARENT:
                if ((Lexer::peek(1).first == LexType::INTTK || Lexer::peek(1).first == LexType::CHARTK)
                    && Lexer::peek(2).first == LexType::RPARENT) {
                    n->baseUnaryExp = CastExp::parse();
                    getBaseUnaryExp = true;

                    output(AST::UnaryExp);
                    break;
                }
                [[fallthrough]];
            case LexType::INTCON:
            case LexType::CHARCON:
                // PrimaryExp → '(' Exp ')' | LVal | Number
                n->baseUnaryExp = PrimaryExp::parse();
                getBaseUnaryExp = true;

                output(AST::UnaryExp);
                break;

            case LexType::IDENFR:
                // PrimaryExp → '(' Exp ')' | LVal | Number
                // LVal → Ident {'[' Exp ']'}

                // Ident '(' [FuncRParams] ')'
                if (Lexer::peek(1).first == LexType::LPARENT) {
                    n->baseUnaryExp = FuncCall::parse();
                } else {
                    n->baseUnaryExp = PrimaryExp::parse();
                }
                getBaseUnaryExp = true;

                output(AST::UnaryExp);
                break;

            default:
                Error::raise();
        }
    }

    for (size_t i = 0; i < n->ops.size(); ++i) {
        output(AST::UnaryExp);
    }

    return n;
}

int UnaryExp::evaluate() const {
    int val = baseUnaryExp->evaluate();

    for (auto op: ops) {
        if (op == LexType::MINU) {
            val = -val;
        }
    }

    return val;
}

size_t UnaryExp::getRank() const {
    if (auto p = dynamic_cast<PrimaryExp *>(baseUnaryExp.get())) {
        return p->getRank();
    }
    return 0;
}

std::string UnaryExp::getIdent() const {
    if (auto p = dynamic_cast<PrimaryExp *>(baseUnaryExp.get())) {
        return p->getIdent();
    }
    if (auto p = dynamic_cast<FuncCall *>(baseUnaryExp.get())) {
        return p->getIdent();
    }
    return "";
}

LVal *UnaryExp::getLVal() const {
    if (auto lVal = dynamic_cast<LVal *>(baseUnaryExp.get())) {
        return lVal;
    }
    return nullptr;
}

Type UnaryExp::getType() const {
    for (LexType op: ops) {
        if (op == LexType::NOT) {
            return Type::Int;
        }
    }
    return baseUnaryExp->getType();
}

std::unique_ptr<FuncCall> FuncCall::parse() {
    auto n = std::make_unique<FuncCall>();

    int row = Lexer::curRow;
    n->ident = Ident::parse();

    Symbol *symbol = SymTab::find(n->ident);
    auto *funcSym = symbol ? symbol->asFunc() : nullptr;
    if (!symbol) {
        Error::raise('c', row);
    } else if (!funcSym) {
        Error::raise('e', row);
    }

    singleLex(LexType::LPARENT);

    if (canStartExp(Lexer::curLexType)) {
        n->funcRParams = FuncRParams::parse();
    }

    if (funcSym) {
        checkParams(n, row, funcSym); // SymTab error handle
    }

    singleLex(LexType::RPARENT, row);
    return n;
}

void FuncCall::checkParams(const std::unique_ptr<FuncCall> &n, int row, const FuncSymbol *funcSym) {
    if (n->funcRParams == nullptr) {
        if (!funcSym->getParams().empty()) {
            Error::raise('d', row);
        }
        return;
    }

    auto &realParams = n->funcRParams->params;
    const auto &formalParams = funcSym->getParams();
    // check number of realParams
    if (realParams.size() != formalParams.size()) {
        Error::raise('d', row);
    } else {
        // check type of params
        for (size_t i = 0; i < realParams.size(); ++i) {
            auto &rParam = realParams[i];

            size_t formalRank = formalParams[i].dims.size();
            size_t symRank = 0;
            bool invalidRank = false;

            size_t indexRank = 0;
            std::string ident;
            if (LVal *lVal = rParam->getLVal()) {
                indexRank = lVal->getRank();
                ident = lVal->getIdent();
            }

            // only consider value Type, no dimentions
            if (ptrToValue(rParam->getType()) != ptrToValue(formalParams[i].type)) {
                Error::raise('e', row);
                continue;
            }

            if (ident.empty()) {
                // rParam is Exp (neither LVal nor FuncCall)
                symRank = 0;
            } else {
                auto *sym = SymTab::find(ident);
                if (!sym) {
                    invalidRank = true;
                } else if (sym->asFunc()) {
                    // FuncCall
                    // void | int
                    invalidRank = sym->getType() == Type::Void;
                } else {
                    // LVal
                    symRank = sym->asObject()->getDims().size();
                }
            }

            if (invalidRank || symRank < indexRank || symRank - indexRank != formalRank) {
                Error::raise('e', row);
            }
        }
    }
}

Type FuncCall::getType() {
    auto sym = SymTab::find(ident);
    auto *funcSym = sym ? sym->asFunc() : nullptr;
    if (!funcSym) {
        return Type::Void;
    }
    return funcSym->getType();
}

const std::string &FuncCall::getIdent() const {
    return ident;
}

std::unique_ptr<CastExp> CastExp::parse() {
    auto n = std::make_unique<CastExp>();

    Lexer::next(); // (
    auto type = Btype::parse();
    n->targetType = toType(type->type);

    int row = Lexer::curRow;
    singleLex(LexType::RPARENT, row);

    n->unaryExp = UnaryExp::parse();
    return n;
}

int CastExp::evaluate() {
    int value = unaryExp->evaluate();
    if (targetType == Type::Char) {
        return value & 0xFF;
    }
    return value;
}

Type CastExp::getType() {
    return targetType;
}
