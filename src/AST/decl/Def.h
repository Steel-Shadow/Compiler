//
// Created by Steel_Shadow on 2023/10/11.
//

#ifndef COMPILER_DEF_H
#define COMPILER_DEF_H

#include "AST/expr/Exp.h"
#include "InitVal.h"

#include <memory>
#include <string>
#include <vector>

// ConstDef → Ident { '[' ConstExp ']' } '=' ConstInitVal
// VarDef → Ident { '[' ConstExp ']' } | Ident { '[' ConstExp ']' } '=' InitVal
struct Def {
    bool cons;
    bool statik{false};

    std::string ident;

    std::vector<std::unique_ptr<Exp>> dims; // dim is ConstExp
    std::unique_ptr<InitVal> initVal;

    // can be empty for VarDef(cons=false)
    const std::unique_ptr<InitVal> &getInitVal() const;

    static std::unique_ptr<Def> parse(bool cons, Type type, bool statik = false);

    const std::string &getIdent() const;

    int getArraySize() const;
};

#endif
