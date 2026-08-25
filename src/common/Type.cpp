//
// Created by Steel_Shadow on 2023/10/15.
//

#include "Type.h"

#include "errorHandler/Error.h"

#include <utility>

int sizeOfType(Type type) {
    switch (type) {
        case Type::Void:
        case Type::Invalid:
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
        case Type::Invalid:
            return Type::Invalid;
        default:
            return Type::Void;
    }
}

Type valueToPtr(Type type) {
    switch (type) {
        case Type::Int:
            return Type::IntPtr;
        case Type::Char:
            return Type::CharPtr;
        case Type::Invalid:
            return Type::Invalid;
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

ParamInfo::ParamInfo(std::string name, Type type, std::vector<int> dims) :
    name(std::move(name)),
    type(type),
    dims(std::move(dims)) {}
