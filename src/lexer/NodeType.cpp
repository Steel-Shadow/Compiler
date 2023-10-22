//
// Created by Steel_Shadow on 2023/10/12.
//

#include "NodeType.h"
#include "error/Error.h"

std::string typeToStr(NodeType type) {
    switch (type) {
        case NodeType::LEX_END:
            return "LEX_END";
        case NodeType::LEX_EMPTY:
            return "LEX_EMPTY";
        case NodeType::IDENFR:
            return "IDENFR";
        case NodeType::INTCON:
            return "INTCON";
        case NodeType::STRCON:
            return "STRCON";
        case NodeType::MAINTK:
            return "MAINTK";
        case NodeType::CONSTTK:
            return "CONSTTK";
        case NodeType::INTTK:
            return "INTTK";
        case NodeType::BREAKTK:
            return "BREAKTK";
        case NodeType::CONTINUETK:
            return "CONTINUETK";
        case NodeType::IFTK:
            return "IFTK";
        case NodeType::ELSETK:
            return "ELSETK";
        case NodeType::AND:
            return "AND";
        case NodeType::OR:
            return "OR";
        case NodeType::FORTK:
            return "FORTK";
        case NodeType::GETINTTK:
            return "GETINTTK";
        case NodeType::PRINTFTK:
            return "PRINTFTK";
        case NodeType::RETURNTK:
            return "RETURNTK";
        case NodeType::PLUS:
            return "PLUS";
        case NodeType::MINU:
            return "MINU";
        case NodeType::VOIDTK:
            return "VOIDTK";
        case NodeType::MULT:
            return "MULT";
        case NodeType::DIV:
            return "DIV";
        case NodeType::MOD:
            return "MOD";
        case NodeType::LEQ:
            return "LEQ";
        case NodeType::LSS:
            return "LSS";
        case NodeType::GEQ:
            return "GEQ";
        case NodeType::GRE:
            return "GRE";
        case NodeType::EQL:
            return "EQL";
        case NodeType::NEQ:
            return "NEQ";
        case NodeType::NOT:
            return "NOT";
        case NodeType::ASSIGN:
            return "ASSIGN";
        case NodeType::SEMICN:
            return "SEMICN";
        case NodeType::COMMA:
            return "COMMA";
        case NodeType::LPARENT:
            return "LPARENT";
        case NodeType::RPARENT:
            return "RPARENT";
        case NodeType::LBRACK:
            return "LBRACK";
        case NodeType::RBRACK:
            return "RBRACK";
        case NodeType::LBRACE:
            return "LBRACE";
        case NodeType::RBRACE:
            return "RBRACE";

        case NodeType::CompUnit:
            return "CompUnit";
        case NodeType::Decl:
            return "Decl";
        case NodeType::FuncDef:
            return "FuncDef";
        case NodeType::MainFuncDef:
            return "MainFuncDef";
        case NodeType::FuncType:
            return "FuncType";
        case NodeType::ConstDecl:
            return "ConstDecl";
        case NodeType::VarDecl:
            return "VarDecl";
        case NodeType::ConstDef:
            return "ConstDef";
        case NodeType::ConstExp:
            return "ConstExp";
        case NodeType::ConstInitVal:
            return "ConstInitVal";
        case NodeType::VarDef:
            return "VarDef";
        case NodeType::InitVal:
            return "InitVal";
        case NodeType::FuncFParams:
            return "FuncFParams";
        case NodeType::FuncFParam:
            return "FuncFParam";
        case NodeType::Btype:
            return "Btype";
        case NodeType::Block:
            return "Block";
        case NodeType::BlockItem:
            return "BlockItem";
        case NodeType::Stmt:
            return "Stmt";
        case NodeType::ForStmt:
            return "ForStmt";
        case NodeType::Exp:
            return "Exp";
        case NodeType::Cond:
            return "Cond";
        case NodeType::LVal:
            return "LVal";
        case NodeType::PrimaryExp:
            return "PrimaryExp";
        case NodeType::Number:
            return "Number";
        case NodeType::UnaryExp:
            return "UnaryExp";
        case NodeType::UnaryOp:
            return "UnaryOp";
        case NodeType::FuncRParams:
            return "FuncRParams";
        case NodeType::MulExp:
            return "MulExp";
        case NodeType::AddExp:
            return "AddExp";
        case NodeType::RelExp:
            return "RelExp";
        case NodeType::EqExp:
            return "EqExp";
        case NodeType::LAndExp:
            return "LAndExp";
        case NodeType::LOrExp:
            return "LOrExp";
        default:
            // bad type
            Error::raise();
            return "";
    }
}
