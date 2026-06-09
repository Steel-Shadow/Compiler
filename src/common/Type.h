//
// Created by Steel_Shadow on 2023/10/15.
//

#ifndef COMPILER_TYPE_H
#define COMPILER_TYPE_H

#include <string>
#include <vector>

enum class Type {
    Void,
    Int,
    Char,
    IntPtr,
    CharPtr,
};

struct ParamInfo {
    std::string name;
    Type type;
    std::vector<int> dims;

    ParamInfo(std::string name, Type type, std::vector<int> dims);
};

using Params = std::vector<ParamInfo>;

int sizeOfType(Type type);
Type ptrToValue(Type type);
Type valueToPtr(Type type);
bool isPtrType(Type type);
bool isBasicType(Type type);

#endif
