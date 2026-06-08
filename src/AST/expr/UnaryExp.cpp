//
// Created by Steel_Shadow on 2023/10/12.
//
#include "AST/decl/Decl.h"
#include "AST/func/Func.h"
#include "backend/Register.h"
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
        if (symbol && (symbol->symType == SymType::Func || n->dims.size() + 1 > symbol->dims.size())) {
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

std::unique_ptr<IR::Temp> LVal::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    auto [symbol, depth] = SymTab::findInGen(ident);
    auto var = std::make_unique<IR::Var>(
            getStorageName(symbol, ident),
            getStorageDepth(symbol, depth),
            symbol->cons,
            symbol->dims,
            symbol->type,
            symbol->symType);

    auto res = std::make_unique<Temp>(ptrToValue(symbol->type));

    if (isPtrType(symbol->type)) {
        auto addr = std::make_unique<Temp>(Type::Int);
        bBlocks.back()->addInst(Inst(IR::Op::Load,
                                     std::make_unique<Temp>(*addr),
                                     std::move(var),
                                     nullptr));
        int constOffset;
        std::unique_ptr<Temp> dynamicOffset;
        bool getNonConstIndex = getOffset(constOffset, dynamicOffset, bBlocks, symbol->dims, symbol->type);
        if (getNonConstIndex) {
            auto addrElem = std::make_unique<Temp>(Type::Int);
            bBlocks.back()->addInst(Inst(IR::Op::Add,
                                         std::make_unique<Temp>(*addrElem),
                                         std::move(addr),
                                         std::move(dynamicOffset)));
            bBlocks.back()->addInst(Inst(IR::Op::LoadPtr,
                                         std::make_unique<Temp>(*res),
                                         std::move(addrElem),
                                         nullptr));
        } else {
            bBlocks.back()->addInst(Inst(IR::Op::LoadPtr,
                                         std::make_unique<Temp>(*res),
                                         std::move(addr),
                                         std::make_unique<ConstVal>(constOffset, Type::Int)));
        }
        return res;
    } else {
        if (dims.empty()) {
            bBlocks.back()->addInst(Inst(IR::Op::Load,
                                         std::make_unique<Temp>(*res),
                                         std::move(var),
                                         nullptr));
        } else {
            int constOffset;
            std::unique_ptr<Temp> dynamicOffset;
            bool getNonConstIndex = getOffset(constOffset, dynamicOffset, bBlocks, symbol->dims, symbol->type);
            if (getNonConstIndex) {
                bBlocks.back()->addInst(Inst(IR::Op::LoadDynamic,
                                             std::make_unique<Temp>(*res),
                                             std::move(var),
                                             std::move(dynamicOffset)));
            } else {
                bBlocks.back()->addInst(Inst(IR::Op::Load,
                                             std::make_unique<Temp>(*res),
                                             std::move(var),
                                             std::make_unique<ConstVal>(constOffset, Type::Int)));
            }
        }
        return res;
    }
}

int LVal::evaluate() {
    auto sym = SymTab::find(ident);
    if (sym == nullptr) {
        Error::raise("LVal not found in evaluate()");
        return 0;
    } else if (!sym->cons) {
        Exp::getNonConstValueInEvaluate = true;
        // Non-const LVal in evaluate()
        return 0;
    } else if (sym->dims.empty()) {
        return sym->initVal[0];
    } else {
        Exp::getNonConstValueInEvaluate = true;
        // Const Array element in evaluate()
        return 0;
    }
}

bool LVal::getOffset(int &constOffset, std::unique_ptr<IR::Temp> &dynamicOffset, IR::BasicBlocks &bBlocks, const std::vector<int> &symDims, Type type) const {
    constOffset = 0;
    auto product = 1;
    bool getNonConstIndex = false;
    for (int i = static_cast<int>(symDims.size()) - 1; i >= 0; --i) {
        if (i != static_cast<int>(symDims.size()) - 1) {
            product *= symDims[i + 1];
        }

        int constIndex = 0;
        std::unique_ptr<IR::Temp> dynamicIndex;

        if (i < dims.size()) {
            constIndex = dims[i]->evaluate();
            if (Exp::getNonConstValueInEvaluate) {
                getNonConstIndex = true;
            }
            if (getNonConstIndex) {
                dynamicIndex = dims[i]->genIR(bBlocks);
            }
        }

        if (!getNonConstIndex) {
            constOffset += constIndex * product;
        } else {
            if (dynamicOffset == nullptr) {
                dynamicOffset = std::make_unique<IR::Temp>(Type::Int);
                bBlocks.back()->addInst(IR::Inst(
                        IR::Op::LoadImd,
                        std::make_unique<IR::Temp>(*dynamicOffset),
                        std::make_unique<IR::ConstVal>(constOffset, Type::Int),
                        nullptr));
            }
            auto dynamicIndexTimesProduct = std::make_unique<IR::Temp>(Type::Int);

            if (product == 1) {
                dynamicIndexTimesProduct = std::move(dynamicIndex);
            } else {
                bBlocks.back()->addInst(IR::Inst(
                        IR::Op::MulImd,
                        std::make_unique<IR::Temp>(*dynamicIndexTimesProduct),
                        std::move(dynamicIndex),
                        std::make_unique<IR::ConstVal>(product, Type::Int)));
            }

            auto res = std::make_unique<IR::Temp>(Type::Int);
            bBlocks.back()->addInst(IR::Inst(
                    IR::Op::Add,
                    std::make_unique<IR::Temp>(*res),
                    std::move(dynamicOffset),
                    std::move(dynamicIndexTimesProduct)));
            dynamicOffset = std::move(res);
        }
    }

    if (getNonConstIndex) {
        int elementSize = sizeOfType(ptrToValue(type));
        if (elementSize == 4) {
            bBlocks.back()->addInst(IR::Inst(IR::Op::Mult4,
                                             std::make_unique<IR::Temp>(*dynamicOffset),
                                             std::make_unique<IR::Temp>(*dynamicOffset),
                                             nullptr));
        } else if (elementSize != 1) {
            bBlocks.back()->addInst(IR::Inst(IR::Op::MulImd,
                                             std::make_unique<IR::Temp>(*dynamicOffset),
                                             std::make_unique<IR::Temp>(*dynamicOffset),
                                             std::make_unique<IR::ConstVal>(elementSize, Type::Int)));
        }
    }

    return getNonConstIndex;
}

Type LVal::getType() {
    auto sym = SymTab::find(ident);
    if (!sym) {
        return Type::Void;
    }
    if (sym->symType == SymType::Func) {
        return Type::Void;
    }
    if (isPtrType(sym->type) && !dims.empty()) {
        return ptrToValue(sym->type);
    }
    return sym->type;
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

std::unique_ptr<IR::Temp> PareExp::genIR(IR::BasicBlocks &bBlocks) {
    return exp->genIR(bBlocks);
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

std::unique_ptr<IR::Temp> Number::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    auto n = std::make_unique<Temp>(type);

    bBlocks.back()->addInst(Inst(Op::LoadImd,
                                 std::make_unique<Temp>(*n),
                                 std::make_unique<ConstVal>(intConst, type),
                                 nullptr));
    return n;
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

    for (int i = 0; i < n->ops.size(); i++) {
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

std::unique_ptr<IR::Temp> UnaryExp::genIR(IR::BasicBlocks &bBlocks) const {
    using namespace IR;
    auto res = baseUnaryExp->genIR(bBlocks);

    for (LexType op: ops) {
        if (op == LexType::MINU) {
            auto negRes = std::make_unique<Temp>(res->type);
            bBlocks.back()->addInst(Inst(
                    Op::Neg,
                    std::make_unique<Temp>(*negRes),
                    std::move(res),
                    nullptr));
            res = std::move(negRes);
        } else if (op == LexType::NOT) {
            auto notRes = std::make_unique<Temp>(Type::Int);
            bBlocks.back()->addInst(Inst(
                    Op::Not,
                    std::make_unique<Temp>(*notRes),
                    std::move(res),
                    nullptr));
            res = std::move(notRes);
        }
    }

    return res;
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

    Symbol *funcSym = SymTab::find(n->ident);
    if (!funcSym) {
        Error::raise('c', row);
    } else if (funcSym->symType != SymType::Func) {
        Error::raise('e', row);
    }

    singleLex(LexType::LPARENT);

    if (canStartExp(Lexer::curLexType)) {
        n->funcRParams = FuncRParams::parse();
    }

    if (funcSym && funcSym->symType == SymType::Func) {
        checkParams(n, row, funcSym); // SymTab error handle
    }

    singleLex(LexType::RPARENT, row);
    return n;
}

void FuncCall::checkParams(const std::unique_ptr<FuncCall> &n, int row, const Symbol *funcSym) {
    if (n->funcRParams == nullptr) {
        if (!funcSym->params.empty()) {
            Error::raise('d', row);
        }
        return;
    }

    auto &realParams = n->funcRParams->params;
    // check number of realParams
    if (realParams.size() != funcSym->params.size()) {
        Error::raise('d', row);
    } else {
        // check type of params
        for (int i = 0; i < realParams.size(); i++) {
            auto &rParam = realParams[i];

            size_t formalRank = funcSym->params[i].second->dims.size();
            size_t symRank;

            size_t indexRank = 0;
            std::string ident;
            if (LVal *lVal = rParam->getLVal()) {
                indexRank = lVal->getRank();
                ident = lVal->getIdent();
            }

            // only consider value Type, no dimentions
            if (ptrToValue(rParam->getType()) != ptrToValue(funcSym->params[i].second->type)) {
                Error::raise('e', row);
                continue;
            }

            if (ident.empty()) {
                // rParam is Exp (neither LVal nor FuncCall)
                symRank = 0;
            } else {
                auto sym = SymTab::find(ident);
                if (sym->symType == SymType::Func) {
                    // FuncCall
                    // void | int
                    symRank = sym->type == Type::Void ? -1 : 0;
                } else {
                    // LVal
                    symRank = sym->dims.size();
                }
            }

            if (symRank - indexRank != formalRank) {
                Error::raise('e', row);
            }
        }
    }
}

std::unique_ptr<IR::Temp> FuncCall::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    auto funcSym = SymTab::find(ident);

    auto makeVarFromLVal = [](LVal *lVal) {
        auto [symbol, depth] = SymTab::findInGen(lVal->ident);
        return std::make_unique<Var>(
                getStorageName(symbol, lVal->ident),
                getStorageDepth(symbol, depth),
                symbol->cons,
                symbol->dims,
                symbol->type,
                symbol->symType);
    };

    if (ident == "get_int" || ident == "get_char") {
        bBlocks.back()->addInst(Inst(ident == "get_int" ? Op::GetInt : Op::GetChar,
                                     nullptr,
                                     nullptr,
                                     nullptr));
        auto temp = std::make_unique<Temp>(funcSym->type);
        bBlocks.back()->addInst(Inst(Op::NewMove,
                                     std::make_unique<Temp>(*temp),
                                     std::make_unique<Temp>(-static_cast<int>(MIPS::Register::v0), funcSym->type),
                                     nullptr));
        return temp;
    }

    if (ident == "put_int" || ident == "put_char") {
        if (funcRParams && !funcRParams->params.empty()) {
            auto value = funcRParams->params[0]->genIR(bBlocks);
            bBlocks.back()->addInst(Inst(ident == "put_int" ? Op::PrintInt : Op::PrintChar,
                                         nullptr,
                                         std::move(value),
                                         nullptr));
        }
        return nullptr;
    }

    if (ident == "put_string" || ident == "put_str") {
        if (funcRParams && !funcRParams->params.empty()) {
            if (auto lVal = funcRParams->params[0]->getLVal()) {
                auto [symbol, depth] = SymTab::findInGen(lVal->ident);
                auto var = makeVarFromLVal(lVal);
                int constOffset = 0;
                std::unique_ptr<Temp> dynamicOffset;
                bool getNonConstIndex = lVal->getOffset(constOffset, dynamicOffset, bBlocks, symbol->dims, symbol->type);
                bBlocks.back()->addInst(Inst(Op::PrintStr,
                                             nullptr,
                                             std::move(var),
                                             getNonConstIndex
                                                     ? std::unique_ptr<Element>(std::move(dynamicOffset))
                                                     : std::unique_ptr<Element>(std::make_unique<ConstVal>(constOffset, Type::Int))));
            }
        }
        return nullptr;
    }

    if (ident == "get_string") {
        if (funcRParams && funcRParams->params.size() >= 2) {
            if (auto lVal = funcRParams->params[0]->getLVal()) {
                auto [symbol, depth] = SymTab::findInGen(lVal->ident);
                auto var = makeVarFromLVal(lVal);
                int constOffset = 0;
                std::unique_ptr<Temp> dynamicOffset;
                bool getNonConstIndex = lVal->getOffset(constOffset, dynamicOffset, bBlocks, symbol->dims, symbol->type);
                auto maxLen = funcRParams->params[1]->genIR(bBlocks);
                bBlocks.back()->addInst(Inst(Op::GetString,
                                             std::move(maxLen),
                                             std::move(var),
                                             getNonConstIndex
                                                     ? std::unique_ptr<Element>(std::move(dynamicOffset))
                                                     : std::unique_ptr<Element>(std::make_unique<ConstVal>(constOffset, Type::Int))));
            }
        }
        return nullptr;
    }

    bBlocks.back()->addInst(Inst(Op::InStack, nullptr, nullptr, nullptr));

    if (funcRParams) {
        for (int i = static_cast<int>(funcRParams->params.size()) - 1; i >= 0; --i) {
            auto &rParam = funcRParams->params[i];
            auto name = rParam->getIdent();

            auto symbol = SymTab::find(name); // LVal / FuncCall
            size_t formalRank = funcSym->params[i].second->dims.size();

            if (formalRank == 0) {
                // single LVal (not array)
                // load Var from memory to Temp
                auto t = rParam->genIR(bBlocks);
                bBlocks.back()->addInst(Inst(Op::PushParam,
                                             nullptr,
                                             std::move(t),
                                             nullptr));
            } else {
                // array (pass param by address)
                auto [paramSymbol, paramDepth] = SymTab::findInGen(name);
                auto var = std::make_unique<Var>(
                        getStorageName(paramSymbol, name),
                        getStorageDepth(paramSymbol, paramDepth),
                        symbol->cons,
                        symbol->dims,
                        symbol->type,
                        symbol->symType);

                int constOffset;
                std::unique_ptr<Temp> dynamicOffset;
                bool getNonConstIndex = rParam->getLVal()->getOffset(
                        constOffset, dynamicOffset, bBlocks, symbol->dims, symbol->type);

                if (getNonConstIndex) {
                    bBlocks.back()->addInst(Inst(
                            Op::PushAddressParam,
                            nullptr,
                            std::move(var),
                            std::move(dynamicOffset)));
                } else {
                    bBlocks.back()->addInst(Inst(
                            Op::PushAddressParam,
                            nullptr,
                            std::move(var),
                            std::make_unique<ConstVal>(constOffset, Type::Int)));
                }
            }
        }
    }

    bBlocks.back()->addInst(Inst(IR::Op::Call,
                                 nullptr,
                                 std::make_unique<Label>(ident, true),
                                 nullptr));
    bBlocks.back()->addInst(Inst(Op::OutStack, nullptr, nullptr, nullptr));

    if (funcSym->type == Type::Int || funcSym->type == Type::Char) {
        auto temp = std::make_unique<Temp>(funcSym->type);
        bBlocks.back()->addInst(Inst(IR::Op::NewMove,
                                     std::make_unique<Temp>(*temp),
                                     std::make_unique<Temp>(-static_cast<int>(MIPS::Register::v0), funcSym->type),
                                     nullptr));
        return temp;
    } else {
        // reType == LexType::VOIDTK
        return nullptr;
    }
}

Type FuncCall::getType() {
    auto sym = SymTab::find(ident);
    if (!sym || sym->symType != SymType::Func) {
        return Type::Void;
    }
    return sym->type;
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

std::unique_ptr<IR::Temp> CastExp::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    auto value = unaryExp->genIR(bBlocks);
    auto res = std::make_unique<Temp>(targetType);

    if (targetType == Type::Char) {
        auto mask = std::make_unique<Temp>(Type::Int);
        bBlocks.back()->addInst(Inst(Op::LoadImd,
                                     std::make_unique<Temp>(*mask),
                                     std::make_unique<ConstVal>(0xFF, Type::Int),
                                     nullptr));
        bBlocks.back()->addInst(Inst(Op::And,
                                     std::make_unique<Temp>(*res),
                                     std::move(value),
                                     std::move(mask)));
    } else {
        bBlocks.back()->addInst(Inst(Op::NewMove,
                                     std::make_unique<Temp>(*res),
                                     std::move(value),
                                     nullptr));
    }
    return res;
}

Type CastExp::getType() {
    return targetType;
}
