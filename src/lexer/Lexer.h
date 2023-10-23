//
// Created by Steel_Shadow on 2023/9/21.
//

#ifndef COMPILER_LEXER_H
#define COMPILER_LEXER_H

#include <fstream>
#include <string>

#include "NodeType.h"

typedef std::string Token;
typedef std::pair<NodeType, Token> Word;

// commented var and func are private
namespace Lexer {
    void init(const std::string &inFile, const std::string &outFile);

    extern std::ofstream outFileStream;
    extern std::string fileContents;

    // pre-reading deep.
    // Change deep and must change row & column initialization!
    static constexpr size_t deep = 3;

    // char c;            // c = fileContents[posTemp - 1]
    extern int pos[deep];     // count from 1
    extern int column[deep];  // count from 1
    extern int row[deep];     // count from 1
    extern int &curRow; // row[0]

    extern Word words[deep];
    extern NodeType &curLexType;
    extern Token &curToken;

    // LinkedHashMap<std::string, NodeType> buildReserveWords();
    // LinkedHashMap<std::string, NodeType> reserveWords;

    char nextChar();

    void reserve(const Token &t, NodeType &l);

    // void retract();

    void output();

    void updateWords(NodeType l, Token t);

    Word next();

    // n is the depth of pre-reading
    Word peek(int n = 0);

    bool findAssignBeforeSemicolon();
}  // namespace Lexer

#endif  // COMPILER_LEXER_H
