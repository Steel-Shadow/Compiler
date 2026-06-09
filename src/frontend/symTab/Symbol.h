//
// Created by Steel_Shadow on 2023/10/15.
//

#ifndef COMPILER_SYMBOL_H
#define COMPILER_SYMBOL_H

#include "common/Type.h"

#include <string>
#include <vector>


class ObjectSymbol;
class ValueSymbol;
class ParamSymbol;
class FuncSymbol;

class Symbol {
public:
    virtual ~Symbol() = default;

    const ObjectSymbol *asObject() const;
    ObjectSymbol *asObject();
    const ValueSymbol *asValue() const;
    ValueSymbol *asValue();
    const ParamSymbol *asParam() const;
    ParamSymbol *asParam();
    const FuncSymbol *asFunc() const;
    FuncSymbol *asFunc();
    Type getType() const;

protected:
    explicit Symbol(Type type);

private:
    Type type;
};

class ObjectSymbol : public Symbol {
public:
    ObjectSymbol(Type type, std::vector<int> dims);

    const std::vector<int> &getDims() const;
    virtual bool storesAddress() const;

protected:
    std::vector<int> dims; // At most 2 dimensions in our work.
};

class ValueSymbol final : public ObjectSymbol {
public:
    ValueSymbol(bool cons,
                Type type,
                std::vector<int> dims,
                std::vector<int> initVal,
                bool statik = false,
                std::string storageName = "");

    bool isConst() const;
    bool isStatic() const;
    const std::string &getStaticStorageName() const;
    const std::vector<int> &getInitVal() const;
    void setInitVal(std::vector<int> values);

private:
    bool cons; // const | var
    bool statik; // static local var
    std::string storageName; // actual global label for static local var
    std::vector<int> initVal; // for const Value
};

class ParamSymbol final : public ObjectSymbol {
public:
    ParamSymbol(Type type, std::vector<int> dims);

    bool storesAddress() const override;
};

class FuncSymbol final : public Symbol {
public:
    FuncSymbol(Type returnType, Params params);

    const Params &getParams() const;

private:
    Params params;
};

#endif
