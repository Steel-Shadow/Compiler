//
// Created by Steel_Shadow on 2023/10/15.
//

#include "Symbol.h"

#include "errorHandler/Error.h"

#include <utility>

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

int sizeOfType(Type type) {
    switch (type) {
        case Type::Void:
            return 0;
        case Type::Char:
            return 1;
        case Type::Int:
            return 4;
        case Type::IntPtr:
        case Type::CharPtr:
            return 4;
        default:
            Error::raise("Bad Type in sizeOfType(...)");
            return 0;
    }
}

Type ptrToValue(Type type) {
    switch (type) {
        case Type::IntPtr:
            return Type::Int;
        case Type::CharPtr:
            return Type::Char;
        case Type::Int:
        case Type::Char:
            return type;
        default:
            // Error::raise("Bad Type in toPtr");
            return Type::Void;
    }
}

Type valueToPtr(Type type) {
    switch (type) {
        case Type::Int:
            return Type::IntPtr;
        case Type::Char:
            return Type::CharPtr;
        default:
            Error::raise("Bad Type in valueToPtr");
            return Type::Void;
    }
}

bool isPtrType(Type type) {
    return type == Type::IntPtr || type == Type::CharPtr;
}

bool isBasicType(Type type) {
    return type == Type::Int || type == Type::Char;
}

Symbol::Symbol(bool cons,
               Type type,
               const std::vector<int> &dims,
               const std::vector<int> &initVal,
               bool statik,
               std::string storageName) :
    symType(SymType::Value),
    type(type),
    cons(cons),
    statik(statik),
    storageName(std::move(storageName)),
    dims(dims),
    initVal(initVal) {}

Symbol::Symbol(Type reType, const std::vector<Param> &params) :
    symType(SymType::Func),
    type(reType),
    params(params) {}

Symbol::Symbol(Type type, std::vector<int> dims) :
    symType(SymType::Param),
    type(type),
    dims(std::move(dims)) {}

std::string getStorageName(const Symbol *symbol, const std::string &ident) {
    if (symbol && symbol->statik && !symbol->storageName.empty()) {
        return symbol->storageName;
    }
    return ident;
}

int getStorageDepth(const Symbol *symbol, int lexicalDepth) {
    return symbol && symbol->statik ? 0 : lexicalDepth;
}
