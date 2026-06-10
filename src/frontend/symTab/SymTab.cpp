//
// Created by Steel_Shadow on 2023/10/15.
//

#include "SymTab.h"
#include "errorHandler/Error.h"

SymTab SymTab::global{nullptr};

SymTab *SymTab::cur = &global;

std::vector<SymTab *> SymTab::traversalOrder;
size_t SymTab::traversalCursor = 0;

void SymTab::resetToGlobal() {
    cur = &global;
}

int SymTab::currentDepth() {
    return cur->depth;
}

SymTab *SymTab::currentParent() {
    return cur->prev;
}

bool SymTab::reDefine(const std::string &ident) {
    if (cur->symbols.find(ident) != cur->symbols.end()) {
        return true;
    }
    return false;
}

Symbol *SymTab::find(const std::string &ident) {
    for (auto p = cur; p != nullptr; p = p->prev) {
        auto it = p->symbols.find(ident);
        if (it != p->symbols.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

std::pair<Symbol *, int> SymTab::findWithDepth(const std::string &ident) {
    for (auto p = cur; p != nullptr; p = p->prev) {
        auto it = p->symbols.find(ident);
        if (it != p->symbols.end()) {
            return {it->second.get(), p->depth};
        }
    }
    return {nullptr, -1};
}

std::vector<std::pair<Symbol *, int>> SymTab::findAllWithDepth(const std::string &ident) {
    std::vector<std::pair<Symbol *, int>> matches;
    for (auto p = cur; p != nullptr; p = p->prev) {
        auto it = p->symbols.find(ident);
        if (it != p->symbols.end()) {
            matches.emplace_back(it->second.get(), p->depth);
        }
    }
    return matches;
}

void SymTab::add(const std::string &ident, std::unique_ptr<Symbol> symbol, SymTab *where) {
    where->symbols.emplace(ident, std::move(symbol));
}

void SymTab::addBuiltins() {
    auto addBuiltin = [](const std::string &ident, Type retType, Params params) {
        if (global.symbols.find(ident) == global.symbols.end()) {
            SymTab::add(ident, std::make_unique<FuncSymbol>(retType, std::move(params)), &global);
        }
    };

    addBuiltin("get_int", Type::Int, {});
    addBuiltin("get_char", Type::Char, {});
    addBuiltin("get_string", Type::Void, {{"buffer", Type::CharPtr, {0}}, {"max_len", Type::Int, {}}});
    addBuiltin("put_int", Type::Void, {{"a", Type::Int, {}}});
    addBuiltin("put_char", Type::Void, {{"a", Type::Char, {}}});
    addBuiltin("put_string", Type::Void, {{"str", Type::CharPtr, {0}}});
    addBuiltin("put_str", Type::Void, {{"str", Type::CharPtr, {0}}});
}


void SymTab::deepIn() {
    auto &newSymTab = cur->next.emplace_back(std::make_unique<SymTab>(cur));
    cur = newSymTab.get();
    traversalOrder.push_back(cur);
}

void SymTab::deepOut() {
    cur = cur->prev;
}

void SymTab::resetTraversal() {
    cur = &global;
    traversalCursor = 0;
}

void SymTab::enterRecordedScope() {
    if (traversalCursor < traversalOrder.size()) {
        cur = traversalOrder[traversalCursor++];
    }
}

void SymTab::leaveRecordedScope() {
    cur = cur->prev;
}

SymTab::SymTab(SymTab *prev) :
    prev(prev) {
    depth = prev == nullptr ? 0 : prev->depth + 1;
}

int SymTab::getDepth() const {
    return depth;
}
