//
// Created by Steel_Shadow on 2023/10/12.
//

#include "LexType.h"
#include "errorHandler/Error.h"

std::string toString(LexType type) {
    switch (type) {
        case LexType::LEX_END:
            return "LEX_END";
        case LexType::LEX_EMPTY:
            return "LEX_EMPTY";
        case LexType::IDENFR:
            return "IDENFR";
        case LexType::INTCON:
            return "INTCON";
        case LexType::CHARCON:
            return "CHRCON";
        case LexType::STRCON:
            return "STRCON";
        case LexType::MAINTK:
            return "MAINTK";
        case LexType::CONSTTK:
            return "CONSTTK";
        case LexType::INTTK:
            return "INTTK";
        case LexType::CHARTK:
            return "CHARTK";
        case LexType::STATICTK:
            return "STATICTK";
        case LexType::BREAKTK:
            return "BREAKTK";
        case LexType::CONTINUETK:
            return "CONTINUETK";
        case LexType::IFTK:
            return "IFTK";
        case LexType::ELSETK:
            return "ELSETK";
        case LexType::WHILETK:
            return "WHILETK";
        case LexType::SWITCHTK:
            return "SWITCHTK";
        case LexType::CASETK:
            return "CASETK";
        case LexType::DEFAULTTK:
            return "DEFAULTTK";
        case LexType::AND:
            return "AND";
        case LexType::OR:
            return "OR";
        case LexType::FORTK:
            return "FORTK";
        case LexType::GETINTTK:
            return "GETINTTK";
        case LexType::PRINTFTK:
            return "PRINTFTK";
        case LexType::RETURNTK:
            return "RETURNTK";
        case LexType::PLUS:
            return "PLUS";
        case LexType::MINU:
            return "MINU";
        case LexType::VOIDTK:
            return "VOIDTK";
        case LexType::MULT:
            return "MULT";
        case LexType::DIV:
            return "DIV";
        case LexType::MOD:
            return "MOD";
        case LexType::LEQ:
            return "LEQ";
        case LexType::LSS:
            return "LSS";
        case LexType::GEQ:
            return "GEQ";
        case LexType::GRE:
            return "GRE";
        case LexType::EQL:
            return "EQL";
        case LexType::NEQ:
            return "NEQ";
        case LexType::NOT:
            return "NOT";
        case LexType::ASSIGN:
            return "ASSIGN";
        case LexType::SEMICN:
            return "SEMICN";
        case LexType::COMMA:
            return "COMMA";
        case LexType::COLON:
            return "COLON";
        case LexType::LPARENT:
            return "LPARENT";
        case LexType::RPARENT:
            return "RPARENT";
        case LexType::LBRACK:
            return "LBRACK";
        case LexType::RBRACK:
            return "RBRACK";
        case LexType::LBRACE:
            return "LBRACE";
        case LexType::RBRACE:
            return "RBRACE";
        default:
            // bad type
            Error::raise();
            return "";
    }
}

Type toType(LexType type) {
    switch (type) {
        case LexType::INTTK:
            return Type::Int;
        case LexType::CHARTK:
            return Type::Char;
        case LexType::VOIDTK:
            return Type::Void;
        default:
            Error::raise("Bad Btype to IR");
            return Type::Void;
    }
}
