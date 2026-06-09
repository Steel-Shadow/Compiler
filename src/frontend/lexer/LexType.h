//
// Created by Steel_Shadow on 2023/9/26.
//

#ifndef COMPILER_NODETYPE_H
#define COMPILER_NODETYPE_H

#include "common/Type.h"

#include <string>

enum class LexType {
    // LexType
    LEX_EMPTY,
    COMMENT,

    IDENFR,
    INTCON,
    CHARCON,
    STRCON,

    MAINTK,
    CONSTTK,
    INTTK,
    CHARTK,
    STATICTK,
    BREAKTK,
    CONTINUETK,
    IFTK,
    ELSETK,
    WHILETK,
    SWITCHTK,
    CASETK,
    DEFAULTTK,
    AND,
    OR,
    FORTK,
    GETINTTK,
    PRINTFTK,
    RETURNTK,
    PLUS,
    MINU,
    VOIDTK,
    MULT,
    DIV,
    MOD,
    LEQ,
    LSS,
    GEQ,
    GRE,
    EQL,
    NEQ,
    NOT,
    ASSIGN,
    SEMICN,
    COMMA,
    COLON,
    LPARENT,
    RPARENT,
    LBRACK,
    RBRACK,
    LBRACE,
    RBRACE,

    LEX_END,
};

std::string toString(LexType type);
Type toType(LexType type);

#endif
