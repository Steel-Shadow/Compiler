//
// Created by Steel_Shadow on 2023/10/11.
//

#ifndef COMPILER_DECL_H
#define COMPILER_DECL_H

#include "AST/stmt/Stmt.h"
#include "Def.h"
#include "frontend/lexer/LexType.h"


#include <memory>
#include <vector>

struct Btype {
    LexType type;

    static std::unique_ptr<Btype> parse();
};

// Decl → ConstDecl | VarDecl
// ConstDecl → 'const' BType ConstDef {','ConstDef}';'
// VarDecl → BType VarDef { ',' VarDef } ';'
struct Decl : public BlockItem {
    bool cons;
    bool statik{false};

    std::unique_ptr<Btype> btype;
    std::vector<std::unique_ptr<Def>> defs;

    const std::vector<std::unique_ptr<Def>> &getDefs() const;

    static std::unique_ptr<Decl> parse();

    // local decl (global decl is evaluated in IR::Module)
    void genIR(IR::BasicBlocks &bBlocks) override;
};

// wrap string
struct Ident {
    static std::string parse();
};

#endif
