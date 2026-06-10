//
// Created by Steel_Shadow on 2023/10/11.
//

#ifndef COMPILER_FUNC_H
#define COMPILER_FUNC_H

#include <memory>
#include <vector>

#include "AST/decl/Decl.h"
#include "frontend/lexer/LexType.h"


// FuncType → 'void' | 'int'
struct FuncType {
    LexType type;

    Type getType() const;

    static std::unique_ptr<FuncType> parse();
};

// FuncFParam → BType Ident ['[' ']' { '[' ConstExp ']' }]
struct FuncFParam {
    std::unique_ptr<Btype> type;
    std::string ident;
    std::vector<std::unique_ptr<Exp>> dims; // if [], dims[0]=nullptr

    static std::unique_ptr<FuncFParam> parse();

    const std::string &getId() const;

    std::vector<int> getDims() const;
};

// FuncFParams → FuncFParam { ',' FuncFParam }
struct FuncFParams {
    std::vector<std::unique_ptr<FuncFParam>> funcFParams;

    static std::unique_ptr<FuncFParams> parse();

    Params getParameters() const;
};

//FuncDef → FuncType Ident '(' [FuncFParams] ')' Block
struct FuncDef {
    std::unique_ptr<FuncType> funcType;
    std::string ident;
    std::unique_ptr<FuncFParams> funcFParams;
    std::unique_ptr<Block> block;

    static std::unique_ptr<FuncDef> parse();
};

// MainFuncDef→'int''main''('')'Block
struct MainFuncDef {
    std::unique_ptr<Block> block;

    static std::unique_ptr<MainFuncDef> parse();
};


// FuncRParams → Exp { ',' Exp }
struct FuncRParams {
    std::vector<std::unique_ptr<Exp>> params;

    static std::unique_ptr<FuncRParams> parse();
};

#endif
