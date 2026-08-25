//
// Created by Steel_Shadow on 2023/9/23.
//

#include "Error.h"

#include <iostream>

#include "config.h"
#include "frontend/lexer/Lexer.h"

bool Error::hasError = false;
std::ofstream Error::errorFileStream;
std::set<std::pair<int, char>> Error::raisedErrors;

void Error::reset() {
    hasError = false;
    raisedErrors.clear();
}

void Error::raise(char code) {
    raise(code, Lexer::curRow);
}

void Error::raise(char code, int row) {
    auto [_, inserted] = raisedErrors.emplace(row, code);
    // Nested parenthesized productions can each be missing their own ')'.
    if (!inserted && code != 'j') {
        return;
    }
    hasError = true;
#ifdef STDOUT_ERROR
    std::cout << row << " " << code << '\n';
#endif
#ifdef FILEOUT_ERROR
    errorFileStream << row << " " << code << '\n';
#endif
}

// My error, which is not defined in course tasks.
void Error::raise([[maybe_unused]] const std::string &mes) {
    hasError = true;
#ifdef STDOUT_ERROR
    std::cout << "error: " << mes << " "
              << "---------------------------------------\n";
#endif
    // exit(-1);
}
