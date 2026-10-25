//
// Created by Steel_Shadow on 2023/11/29.
//

#include "AST.h"

std::string toString(AST e) {
    switch (e) {
        case AST::CompUnit:
            return "CompUnit";
        case AST::Decl:
            return "Decl";
        case AST::FuncDef:
            return "FuncDef";
        case AST::MainFuncDef:
            return "MainFuncDef";
        case AST::FuncType:
            return "FuncType";
        case AST::ConstDecl:
            return "ConstDecl";
        case AST::VarDecl:
            return "VarDecl";
        case AST::ConstDef:
            return "ConstDef";
        case AST::ConstExp:
            return "ConstExp";
        case AST::ConstInitVal:
            return "ConstInitVal";
        case AST::VarDef:
            return "VarDef";
        case AST::InitVal:
            return "InitVal";
        case AST::FuncFParams:
            return "FuncFParams";
        case AST::FuncFParam:
            return "FuncFParam";
        case AST::Btype:
            return "Btype";
        case AST::Block:
            return "Block";
        case AST::BlockItem:
            return "BlockItem";
        case AST::Stmt:
            return "Stmt";
        case AST::CaseStmt:
            return "CaseStmt";
        case AST::ForStmt:
            return "ForStmt";
        case AST::Exp:
            return "Exp";
        case AST::Cond:
            return "Cond";
        case AST::LVal:
            return "LVal";
        case AST::PrimaryExp:
            return "PrimaryExp";
        case AST::Number:
            return "Number";
        case AST::UnaryExp:
            return "UnaryExp";
        case AST::UnaryOp:
            return "UnaryOp";
        case AST::FuncRParams:
            return "FuncRParams";
        case AST::MulExp:
            return "MulExp";
        case AST::AddExp:
            return "AddExp";
        case AST::RelExp:
            return "RelExp";
        case AST::EqExp:
            return "EqExp";
        case AST::LAndExp:
            return "LAndExp";
        case AST::LOrExp:
            return "LOrExp";
        case AST::AnonFunc:
            return "AnonFunc";
        default:
            return "unknown";
    }
}
