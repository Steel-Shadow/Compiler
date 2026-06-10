//
// Created by Steel_Shadow on 2023/10/15.
//

#include "SymTab.h"
#include "errorHandler/Error.h"

SymTab SymTab::global{nullptr};

SymTab *SymTab::cur = &global;

std::list<SymTab *> SymTab::symTabs;
std::vector<std::set<std::pair<std::string, int>>> SymTab::generatedVars;
std::vector<ValueSymbol *> SymTab::staticVars;
int SymTab::staticStorageId = 0;

void SymTab::reset() {
    cur = &global;
    generatedVars.clear();
    staticVars.clear();
    symTabs.clear();
    staticStorageId = 0;
    global.next.clear();
    global.symbols.clear();
}

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

std::pair<Symbol *, int> SymTab::findInGen(const std::string &ident) {
    auto generatedVarScope = generatedVars.crbegin();
    for (SymTab *p = cur;
         p != nullptr && generatedVarScope != generatedVars.crend();
         p = p->prev, ++generatedVarScope) {
        auto it = p->symbols.find(ident);
        if (it != p->symbols.end() && (it->second->asParam() || generatedVarScope->find({ident, p->depth}) != generatedVarScope->end())) {
            return {it->second.get(), p->depth};
        }
    }
    return {nullptr, -1};
}

void SymTab::add(const std::string &ident, std::unique_ptr<Symbol> symbol, SymTab *where) {
    auto [it, inserted] = where->symbols.emplace(ident, std::move(symbol));
    if (inserted) {
        auto *value = it->second->asValue();
        if (value && value->isStatic()) {
            staticVars.push_back(value);
        }
    }
}

const std::vector<ValueSymbol *> &SymTab::getStaticVars() {
    return staticVars;
}

std::string SymTab::nextStaticStorageName(const std::string &ident) {
    return "__static_" + ident + "_" + std::to_string(staticStorageId++);
}

void SymTab::enterGeneratedVarScope() {
    generatedVars.emplace_back();
}

void SymTab::leaveGeneratedVarScope() {
    generatedVars.pop_back();
}

void SymTab::recordGeneratedVar(const std::string &ident, int depth) {
    generatedVars.back().emplace(ident, depth);
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
    symTabs.push_back(cur);
}

void SymTab::deepOut() {
    cur = cur->prev;
}

SymTab::SymTab(SymTab *prev) :
    prev(prev) {
    depth = prev == nullptr ? 0 : prev->depth + 1;
}

void SymTab::iterIn() {
    cur = symTabs.front();
    symTabs.pop_front();
    enterGeneratedVarScope();
}

void SymTab::iterOut() {
    cur = cur->prev;
    leaveGeneratedVarScope();
}

int SymTab::getDepth() const {
    return depth;
}
