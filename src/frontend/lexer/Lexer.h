//
// Created by Steel_Shadow on 2023/9/21.
//

#ifndef COMPILER_LEXER_H
#define COMPILER_LEXER_H

#include <fstream>
#include <string>

#include "LexType.h"

using Token = std::string;
using Word = std::pair<LexType, Token>;

// commented var and func are private
namespace Lexer {
void init(const std::string &inFile, const std::string &outFile);

extern std::ofstream outFileStream;
extern std::string fileContents;

// pre-reading deep.
static constexpr size_t deep = 3;

// char c;            // c = fileContents[posTemp - 1]
extern size_t pos[deep]; // count from 1
extern int column[deep]; // count from 1
extern int row[deep]; // count from 1
extern int &curRow; // row[0]
extern int lastRow;

Word peek(int n = 0);

// Word words[deep];
extern LexType &curLexType;
extern Token &curToken;

// LinkedHashMap<std::string, LexType> buildReserveWords();
// LinkedHashMap<std::string, LexType> reserveWords;
// char nextChar();
// void reserve(const Token &t, LexType &l);
// void output();
// void updateWords(LexType l, Token t);

Word next();

bool findAssignBeforeSemicolon();
} // namespace Lexer

#endif
