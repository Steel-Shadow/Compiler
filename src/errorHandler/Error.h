//
// Created by Steel_Shadow on 2023/9/23.
//

#ifndef COMPILER_ERROR_H
#define COMPILER_ERROR_H

#include <fstream>
#include <set>
#include <string>
#include <utility>


class Error {
public:
    static bool hasError;

    static std::ofstream errorFileStream;
    static std::set<std::pair<int, char>> raisedErrors;

    static void reset();

    static void raise(char code);
    static void raise(char code, int row);

    static void raise(const std::string &mes = "unnamed");
};


#endif
