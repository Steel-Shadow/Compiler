//
// Created by Steel_Shadow on 2023/10/11.
//

#ifndef COMPILER_PARSER_H
#define COMPILER_PARSER_H

#include "AST/AST.h"
#include "frontend/lexer/LexType.h"

// Specific parser method is distributed in respective class.
namespace Parser {
// check the current token and advance on match
void singleLex(LexType type);
void singleLex(LexType type, int row);

void output(AST type);
} // namespace Parser

#endif
