//
// Created by Steel_Shadow on 2023/10/15.
//

#include "Symbol.h"

#include <utility>

Symbol::Symbol(Type type) :
    type(type) {}

Type Symbol::getType() const {
    return type;
}

ObjectSymbol::ObjectSymbol(Type type, std::vector<int> dims) :
    Symbol(type),
    dims(std::move(dims)) {}

const std::vector<int> &ObjectSymbol::getDims() const {
    return dims;
}

bool ObjectSymbol::storesAddress() const {
    return false;
}

ValueSymbol::ValueSymbol(bool cons,
                         Type type,
                         std::vector<int> dims,
                         std::vector<int> initVal,
                         bool statik,
                         std::string storageName) :
    ObjectSymbol(type, std::move(dims)),
    cons(cons),
    statik(statik),
    storageName(std::move(storageName)),
    initVal(std::move(initVal)) {}

bool ValueSymbol::isConst() const {
    return cons;
}

bool ValueSymbol::isStatic() const {
    return statik;
}

const std::string &ValueSymbol::getStaticStorageName() const {
    return storageName;
}

const std::vector<int> &ValueSymbol::getInitVal() const {
    return initVal;
}

void ValueSymbol::setInitVal(std::vector<int> values) {
    initVal = std::move(values);
}

ParamSymbol::ParamSymbol(Type type, std::vector<int> dims) :
    ObjectSymbol(type, std::move(dims)) {}

bool ParamSymbol::storesAddress() const {
    return !getDims().empty();
}

FuncSymbol::FuncSymbol(Type returnType, Params params) :
    Symbol(returnType),
    params(std::move(params)) {}

const Params &FuncSymbol::getParams() const {
    return params;
}

const ObjectSymbol *Symbol::asObject() const {
    return dynamic_cast<const ObjectSymbol *>(this);
}

ObjectSymbol *Symbol::asObject() {
    return dynamic_cast<ObjectSymbol *>(this);
}

const ValueSymbol *Symbol::asValue() const {
    return dynamic_cast<const ValueSymbol *>(this);
}

ValueSymbol *Symbol::asValue() {
    return dynamic_cast<ValueSymbol *>(this);
}

const ParamSymbol *Symbol::asParam() const {
    return dynamic_cast<const ParamSymbol *>(this);
}

ParamSymbol *Symbol::asParam() {
    return dynamic_cast<ParamSymbol *>(this);
}

const FuncSymbol *Symbol::asFunc() const {
    return dynamic_cast<const FuncSymbol *>(this);
}

FuncSymbol *Symbol::asFunc() {
    return dynamic_cast<FuncSymbol *>(this);
}
