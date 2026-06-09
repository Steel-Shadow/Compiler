//
// Created by Codex on 2026/06/09.
//

#include "IRGenUtil.h"

namespace {
const ValueSymbol *staticStorageValue(const Symbol *symbol) {
    const auto *value = symbol ? symbol->asValue() : nullptr;
    return value && value->isStatic() ? value : nullptr;
}

std::string storageName(const Symbol *symbol, const std::string &ident) {
    const auto *value = staticStorageValue(symbol);
    if (value && !value->getStaticStorageName().empty()) {
        return value->getStaticStorageName();
    }
    return ident;
}

int storageDepth(const Symbol *symbol, int lexicalDepth) {
    return staticStorageValue(symbol) ? 0 : lexicalDepth;
}

std::unique_ptr<IR::Var> makeIRVarImpl(const Symbol *storageSymbol,
                                       const std::string &ident,
                                       int depth,
                                       Type type,
                                       const Symbol *attributeSymbol) {
    const auto *object = attributeSymbol->asObject();
    const auto *value = attributeSymbol->asValue();
    return std::make_unique<IR::Var>(
            storageName(storageSymbol, ident),
            storageDepth(storageSymbol, depth),
            value && value->isConst(),
            object->getDims(),
            type,
            object->storesAddress());
}
} // namespace

std::unique_ptr<IR::Var> makeIRVar(const Symbol *symbol, const std::string &ident, int depth) {
    return makeIRVar(symbol, ident, depth, symbol->getType());
}

std::unique_ptr<IR::Var> makeIRVar(const Symbol *symbol, const std::string &ident, int depth, Type type) {
    return makeIRVarImpl(symbol, ident, depth, type, symbol);
}

std::unique_ptr<IR::Var> makeIRVar(const Symbol *storageSymbol, const std::string &ident, int depth, const Symbol *attributeSymbol) {
    return makeIRVarImpl(storageSymbol, ident, depth, attributeSymbol->getType(), attributeSymbol);
}
