//
// Created by Steel_Shadow on 2023/10/11.
//

#include "Parser.h"

#include "config.h"
#include "errorHandler/Error.h"
#include "frontend/lexer/Lexer.h"

void Parser::singleLex(LexType type) {
    singleLex(type, Lexer::curRow);
}

void Parser::singleLex(LexType type, int row) {
    if (Lexer::curLexType == type) {
        Lexer::next();
    } else {
        int errorRow = Lexer::lastRow > 0 ? Lexer::lastRow : row;
        if (type == LexType::SEMICN) {
            Error::raise('i', errorRow);
        } else if (type == LexType::RPARENT) {
            Error::raise('j', errorRow);
        } else if (type == LexType::RBRACK) {
            Error::raise('k', errorRow);
        } else {
            Error::raise(std::string("Miss singleLex ") + toString(type));
        }
    }
}

void Parser::output([[maybe_unused]] AST type) {
#ifdef STDOUT_PARSER
    std::cout << "<" << toString(type) << ">" << '\n';
#endif
#ifdef FILEOUT_PARSER
    Lexer::outFileStream << "<" << toString(type) << ">" << '\n';
#endif
}
