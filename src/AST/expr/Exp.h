//
// Created by Steel_Shadow on 2023/10/11.
//

#ifndef COMPILER_EXP_H
#define COMPILER_EXP_H

#include "frontend/lexer/LexType.h"


#include <memory>
#include <string>
#include <vector>

class Symbol;
class FuncSymbol;
struct FuncRParams;
struct Exp;

bool opProducesInt(LexType type);
Type resolveMultiExpType(Type firstType,
                         size_t firstRemainingRank,
                         const std::vector<Type> &elementTypes,
                         const std::vector<size_t> &elementRemainingRanks,
                         const std::vector<LexType> &ops);

struct BaseUnaryExp {
    virtual ~BaseUnaryExp() = default;

    // override in Number
    virtual int evaluate();

    virtual Type getType() = 0;
};

// PrimaryExp → '(' Exp ')' | LVal | Number
struct PrimaryExp : public BaseUnaryExp {
    static std::unique_ptr<PrimaryExp> parse();

    virtual size_t getRank();

    virtual std::string getIdent();
};

// LVal → Ident {'[' Exp ']'}
struct LVal : public PrimaryExp {
    std::string ident;
    std::vector<std::unique_ptr<Exp>> dims;

    static std::unique_ptr<LVal> parse();

    size_t getRank() override;

    std::string getIdent() override;

    int evaluate() override;

    Type getType() override;
};

size_t remainingRankOf(LVal *lVal);

// UnaryExp → PrimaryExp | Ident '(' [FuncRParams] ')' | UnaryOp UnaryExp
// change syntax to
// UnaryExp → {UnaryOp} ( PrimaryExp | Ident '(' [FuncRParams] ')' )
// Note: UnaryOp is not a separate class!
struct UnaryExp {
    std::vector<LexType> ops;
    std::unique_ptr<BaseUnaryExp> baseUnaryExp;

    static std::unique_ptr<UnaryExp> parse();

    int evaluate() const;

    size_t getRank() const;

    std::string getIdent() const;

    LVal *getLVal() const;

    Type getType() const;
};

// Ident '(' [FuncRParams] ')'
struct FuncCall : public BaseUnaryExp {
    std::string ident;
    std::unique_ptr<FuncRParams> funcRParams;

    const std::string &getIdent() const;

    static std::unique_ptr<FuncCall> parse();

    static void checkParams(const std::unique_ptr<FuncCall> &n, int row, const FuncSymbol *funcSym);

    Type getType() override;
};

// '(' BType ')' UnaryExp
struct CastExp : public BaseUnaryExp {
    Type targetType;
    std::unique_ptr<UnaryExp> unaryExp;

    static std::unique_ptr<CastExp> parse();

    int evaluate() override;
    Type getType() override;
};

struct PareExp : public PrimaryExp {
    // '(' Exp ')'
    std::unique_ptr<Exp> exp;

    static std::unique_ptr<PareExp> parse();

    int evaluate() override;
    Type getType() override;
};

// Number → IntConst
struct Number : public PrimaryExp {
    int intConst{};
    Type type{Type::Int};

    static std::unique_ptr<Number> parse();

    int evaluate() override;

    Type getType() override;
};

template<class T>
struct MultiExp {
    std::unique_ptr<T> first;
    std::vector<LexType> ops;
    std::vector<std::unique_ptr<T>> elements;

    std::string getIdent() {
        if (!elements.empty()) {
            return "";
        }
        return first->getIdent();
    }

    LVal *getLVal() const {
        if (!elements.empty()) {
            return nullptr;
        }
        return first->getLVal();
    }

    size_t getRank() {
        if (!elements.empty()) {
            return 0;
        }
        return first->getRank();
    }

    Type getType() {
        Type firstType = first->getType();
        size_t firstRemainingRank = remainingRankOf(first->getLVal());
        std::vector<Type> elementTypes;
        std::vector<size_t> elementRemainingRanks;
        elementTypes.reserve(elements.size());
        elementRemainingRanks.reserve(elements.size());
        for (const auto &element: elements) {
            elementTypes.push_back(element->getType());
            elementRemainingRanks.push_back(remainingRankOf(element->getLVal()));
        }
        return resolveMultiExpType(firstType, firstRemainingRank, elementTypes, elementRemainingRanks, ops);
    }
};

// MulExp → UnaryExp | MulExp ('*' | '/' | '%') UnaryExp
struct MulExp : MultiExp<UnaryExp> {
    static std::unique_ptr<MulExp> parse();

    int evaluate() const;
};

// AddExp → MulExp | AddExp ('+' | '−') MulExp
struct AddExp : MultiExp<MulExp> {
    static std::unique_ptr<AddExp> parse();

    int evaluate() const;
};

// RelExp → AddExp | RelExp ('<' | '>' | '<=' | '>=') AddExp
struct RelExp : MultiExp<AddExp> {
    static std::unique_ptr<RelExp> parse();
};

// EqExp → RelExp | EqExp ('==' | '!=') RelExp
struct EqExp : MultiExp<RelExp> {
    static std::unique_ptr<EqExp> parse();
};

// LAndExp → EqExp | LAndExp '&&' EqExp
struct LAndExp : MultiExp<EqExp> {
    static std::unique_ptr<LAndExp> parse();
};

// LOrExp → LAndExp | LOrExp '||' LAndExp
struct LOrExp : MultiExp<LAndExp> {
    static std::unique_ptr<LOrExp> parse();
};

// Cond → LOrExp
struct Cond {
    std::unique_ptr<LOrExp> lorExp;

    static std::unique_ptr<Cond> parse();
};

// Exp → AddExp
// ConstExp → AddExp
// Ident in ConstExp must be const.
struct Exp {
    bool cons;

    std::unique_ptr<AddExp> addExp;

    static std::unique_ptr<Exp> parse(bool cons);

    static bool getNonConstValueInEvaluate;
    int evaluate() const;

    // get the indexRank of LVal
    // 0 if the Exp is not a single LVal
    size_t getRank() const;

    // get remaining array rank after indexing
    // 0 if the Exp is not a single array LVal
    size_t getRemainingRank() const;

    // get ident of LVal or FuncCall
    // if the Exp is not a single LVal or FuncCall, return ""
    std::string getIdent() const;

    Type getType() const;

    // FuncCall rParam is an array
    LVal *getLVal() const;
};

#endif
