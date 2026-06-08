//
// Created by Steel_Shadow on 2023/10/15.
//

#ifndef COMPILER_SYMBOL_H
#define COMPILER_SYMBOL_H

#include "frontend/lexer/LexType.h"
#include <string>
#include <vector>


struct Symbol;

using Param = std::pair<std::string, Symbol *>;

enum class SymType {
    // const var
    Value,

    Func,
    Param
};

enum class Type {
    Void,
    Int,
    Char,
    IntPtr,
    CharPtr,
};

Type toType(LexType type);

int sizeOfType(Type type);
Type ptrToValue(Type type);
Type valueToPtr(Type type);
bool isPtrType(Type type);
bool isBasicType(Type type);

// all information in a Symbol (also redundant info)
// use SymType type to distinguish
struct Symbol {
    // Value -> a | a[...] | a[...][...]
    // Func -> func
    // Param -> p | p[] | p[][...]
    SymType symType;

    // Value's type | Func's return type
    Type type;

    // value & array. dims also share for param
    bool cons{false}; // const | var
    bool statik{false}; // static local var
    std::string storageName; // actual global label for static local var
    std::vector<int> dims; // At most 2 dimensions in our work.
    std::vector<int> initVal; // for const Value

    // func
    std::vector<Param> params;

    // const var
    Symbol(bool cons,
           Type type,
           const std::vector<int> &dims,
           const std::vector<int> &initVal,
           bool statik = false,
           std::string storageName = "");
    // func
    Symbol(Type reType, const std::vector<Param> &params);
    // param
    explicit Symbol(Type type, std::vector<int> dims);
};

std::string getStorageName(const Symbol *symbol, const std::string &ident);
int getStorageDepth(const Symbol *symbol, int lexicalDepth);

#endif
