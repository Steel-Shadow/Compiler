//
// Created by Steel_Shadow on 2023/10/11.
//

#ifndef COMPILER_INITVAL_H
#define COMPILER_INITVAL_H

#include <memory>
#include <string>
#include <vector>

struct Exp;

// ConstInitVal → ConstExp | '{' [ ConstInitVal { ',' ConstInitVal } ] '}'
// InitVal → Exp | '{' [ InitVal { ',' InitVal } ] '}'
// Both InitVal & ConstInitVal. Distinguish with `bool cons`
struct InitVal {
protected:
    bool cons{false};

public:
    virtual ~InitVal() = default;

    static std::unique_ptr<InitVal> parse(bool cons);

    virtual std::vector<int> evaluate() = 0;
};

struct ExpInitVal : public InitVal {
    std::unique_ptr<Exp> exp;

    static std::unique_ptr<ExpInitVal> parse(bool cons);

    std::vector<int> evaluate() override;
};

struct ArrayInitVal : public InitVal {
    std::vector<std::unique_ptr<InitVal>> array;

    static std::unique_ptr<ArrayInitVal> parse(bool cons);

    std::vector<int> evaluate() override;

    std::vector<ExpInitVal *> getFlatten() const;
};

struct StringInitVal : public InitVal {
    std::string stringConst;

    static std::unique_ptr<StringInitVal> parse(bool cons);

    std::vector<int> evaluate() override;
};

#endif
