//
// Created by Steel_Shadow on 2023/11/29.
//

#ifndef AST_H
#define AST_H
#include <string>

enum class AST {
    CompUnit,
    Decl,
    FuncDef,
    MainFuncDef,
    FuncType,
    ConstDecl,
    VarDecl,
    ConstDef,
    ConstExp,
    ConstInitVal,
    VarDef,
    InitVal,
    FuncFParams,
    FuncFParam,
    Btype,
    Block,
    BlockItem,
    Stmt,
    CaseStmt,
    ForStmt,
    Exp,
    Cond,
    LVal,
    PrimaryExp,
    Number,
    UnaryExp,
    UnaryOp,
    FuncRParams,
    MulExp,
    AddExp,
    RelExp,
    EqExp,
    LAndExp,
    LOrExp,
};

// do not use it for <BlockItem>, <Decl>, <BType>
std::string toString(AST e);

#endif
