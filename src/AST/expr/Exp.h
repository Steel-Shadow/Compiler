//
// Created by Steel_Shadow on 2023/10/11.
//

#ifndef COMPILER_EXP_H
#define COMPILER_EXP_H

#include "errorHandler/Error.h"
#include "frontend/lexer/LexType.h"
#include "frontend/symTab/Symbol.h"
#include "middle/IR.h"


#include <memory>
#include <vector>

struct FuncRParams;
struct Exp;

struct BaseUnaryExp {
    virtual ~BaseUnaryExp() = default;

    // override in Number
    virtual int evaluate();

    virtual std::unique_ptr<IR::Temp> genIR(IR::BasicBlocks &bBlocks) = 0;
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

    // Load a Var to Temp
    std::unique_ptr<IR::Temp> genIR(IR::BasicBlocks &bBlocks) override;

    int evaluate() override;

    // return whether getNonConstIndex in LVal's indexes
    bool getOffset(int &constOffset, std::unique_ptr<IR::Temp> &dynamicOffset, IR::BasicBlocks &bBlocks, const std::vector<int> &symDims, Type type = Type::Int) const;

    Type getType() override;
};

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

    std::unique_ptr<IR::Temp> genIR(IR::BasicBlocks &bBlocks) const;

    LVal *getLVal() const;

    Type getType() const;
};

// Ident '(' [FuncRParams] ')'
struct FuncCall : public BaseUnaryExp {
    std::string ident;
    std::unique_ptr<FuncRParams> funcRParams;

    const std::string &getIdent() const;

    static std::unique_ptr<FuncCall> parse();

    static void checkParams(const std::unique_ptr<FuncCall> &n, int row, const Symbol *funcSym);

    std::unique_ptr<IR::Temp> genIR(IR::BasicBlocks &bBlocks) override;

    Type getType() override;
};

// '(' BType ')' UnaryExp
struct CastExp : public BaseUnaryExp {
    Type targetType;
    std::unique_ptr<UnaryExp> unaryExp;

    static std::unique_ptr<CastExp> parse();

    int evaluate() override;
    std::unique_ptr<IR::Temp> genIR(IR::BasicBlocks &bBlocks) override;
    Type getType() override;
};

struct PareExp : public PrimaryExp {
    // '(' Exp ')'
    std::unique_ptr<Exp> exp;

    static std::unique_ptr<PareExp> parse();

    std::unique_ptr<IR::Temp> genIR(IR::BasicBlocks &bBlocks) override;

    int evaluate() override;
    Type getType() override;
};

// Number → IntConst
struct Number : public PrimaryExp {
    int intConst{};
    Type type{Type::Int};

    static std::unique_ptr<Number> parse();

    int evaluate() override;

    // load immediate number in a separate MIPS instruction
    // can be optimized into calculate instruction (backend)
    std::unique_ptr<IR::Temp> genIR(IR::BasicBlocks &bBlocks) override;

    Type getType() override;
};

template<class T>
struct MultiExp {
    std::unique_ptr<T> first;
    std::vector<LexType> ops;
    std::vector<std::unique_ptr<T>> elements;

    std::unique_ptr<IR::Temp> genIR(IR::BasicBlocks &bBlocks) const {
        using namespace IR;
        auto lastRes = first->genIR(bBlocks);
        for (int i = 0; i < ops.size(); i++) {
            auto t = elements[i]->genIR(bBlocks);
            Type resultType = ptrToValue(lastRes->type);
            if (ops[i] == LexType::LSS || ops[i] == LexType::GRE || ops[i] == LexType::LEQ || ops[i] == LexType::GEQ
                || ops[i] == LexType::EQL || ops[i] == LexType::NEQ || ops[i] == LexType::AND || ops[i] == LexType::OR) {
                resultType = Type::Int;
            }
            auto res = std::make_unique<Temp>(resultType);
            bBlocks.back()->addInst(Inst(
                    LexTypeToIROp(ops[i]),
                    std::make_unique<Temp>(*res),
                    std::move(lastRes),
                    std::move(t)));
            lastRes = std::move(res);
        }
        return lastRes;
    }

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
        Type type = first->getType();
        for (int i = 0; i < elements.size(); i++) {
            if (ptrToValue(elements[i]->getType()) != ptrToValue(type)) {
                // Type check here is too simple
                // func(t[2]+arr[0]);
                Error::raise("Not same Type in Exp");
                return Type::Void;
            }
        }
        if (!ops.empty()
            && (ops.back() == LexType::LSS || ops.back() == LexType::GRE || ops.back() == LexType::LEQ || ops.back() == LexType::GEQ
                || ops.back() == LexType::EQL || ops.back() == LexType::NEQ || ops.back() == LexType::AND || ops.back() == LexType::OR)) {
            return Type::Int;
        }
        return type;
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
    void genIR(IR::BasicBlocks &basicBlocks, IR::Label &trueBranch, IR::Label &falseBranch) const;
};

// LAndExp → EqExp | LAndExp '&&' EqExp
struct LAndExp : MultiExp<EqExp> {
    static std::unique_ptr<LAndExp> parse();
    void genIR(IR::BasicBlocks &basicBlocks, IR::Label &trueBranch, IR::Label &falseBranch) const;
};

// LOrExp → LAndExp | LOrExp '||' LAndExp
struct LOrExp : MultiExp<LAndExp> {
    static std::unique_ptr<LOrExp> parse();
    void genIR(IR::BasicBlocks &basicBlocks, IR::Label &trueBranch, IR::Label &falseBranch) const;
};

// Cond → LOrExp
struct Cond {
    std::unique_ptr<LOrExp> lorExp;

    static std::unique_ptr<Cond> parse();

    void genIR(IR::BasicBlocks &basicBlocks, IR::Label &trueBranch, IR::Label &falseBranch) const;
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

    // get ident of LVal or FuncCall
    // if the Exp is not a single LVal or FuncCall, return ""
    std::string getIdent() const;

    std::unique_ptr<IR::Temp> genIR(IR::BasicBlocks &bBlocks) const;

    Type getType() const;

    // FuncCall rParam is an array
    LVal *getLVal() const;
};

#endif
