//
// Created by Steel_Shadow on 2023/10/15.
//

#include "SymTab.h"
#include "errorHandler/Error.h"

SymTab SymTab::global{nullptr};

SymTab *SymTab::cur = &global;

std::vector<std::set<std::pair<std::string, int>>> SymTab::knownVars;
std::vector<Symbol *> SymTab::staticVars;

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
            return &it->second;
        }
    }
    return nullptr;
}

std::pair<Symbol *, int> SymTab::findInGen(const std::string &ident) {
    auto knownVars_i = knownVars.crbegin();
    for (SymTab *p = cur;
         p != nullptr && knownVars_i != knownVars.crend();
         p = p->prev, ++knownVars_i) {
        auto it = p->symbols.find(ident);
        if (it != p->symbols.end() && (it->second.symType == SymType::Param || knownVars_i->find({ident, p->depth}) != knownVars_i->end())) {
            return {&it->second, p->depth};
        }
    }
    return {nullptr, -1};
}

int SymTab::findDepth(const std::string &ident) {
    for (auto symTab = cur; symTab != nullptr; symTab = symTab->prev) {
        auto it = symTab->symbols.find(ident);
        if (it != symTab->symbols.end()) {
            return symTab->depth;
        }
    }
    return -1;
}

void SymTab::add(const std::string &ident, Symbol &&symbol, SymTab *where) {
    auto [it, inserted] = where->symbols.emplace(ident, std::move(symbol));
    if (inserted && it->second.statik) {
        staticVars.push_back(&it->second);
    }
}

const std::vector<Symbol *> &SymTab::getStaticVars() {
    return staticVars;
}

void SymTab::addBuiltins() {
    static Symbol intParam(Type::Int, std::vector<int>{});
    static Symbol charParam(Type::Char, std::vector<int>{});
    static Symbol charArrayParam(Type::CharPtr, std::vector<int>{0});

    auto addBuiltin = [](const std::string &ident, Type retType, std::vector<Param> params) {
        if (global.symbols.find(ident) == global.symbols.end()) {
            SymTab::add(ident, Symbol(retType, params), &global);
        }
    };

    addBuiltin("get_int", Type::Int, {});
    addBuiltin("get_char", Type::Char, {});
    addBuiltin("get_string", Type::Void, {{"buffer", &charArrayParam}, {"max_len", &intParam}});
    addBuiltin("put_int", Type::Void, {{"a", &intParam}});
    addBuiltin("put_char", Type::Void, {{"a", &charParam}});
    addBuiltin("put_string", Type::Void, {{"str", &charArrayParam}});
    addBuiltin("put_str", Type::Void, {{"str", &charArrayParam}});
}


std::list<SymTab *> SymTab::symTabs;

void SymTab::deepIn() {
    auto &newSymTab = cur->next.emplace_back(std::make_unique<SymTab>(cur));
    cur = newSymTab.get();
    symTabs.push_back(cur);
}

void SymTab::deepOut() {
    cur = cur->prev;
}

SymTab *SymTab::getPrev() const {
    return prev;
}

SymTab::SymTab(SymTab *prev) :
    prev(prev) {
    depth = prev == nullptr ? 0 : prev->depth + 1;
}

void SymTab::iterIn() {
    /*static std::queue<SymTab *> iterIndex = global.dfs();
    cur = iterIndex.front();
    iterIndex.pop();*/
    cur = symTabs.front();
    symTabs.pop_front();
    SymTab::knownVars.emplace_back();
}

void SymTab::iterOut() {
    cur = cur->prev;
    SymTab::knownVars.pop_back();
}

int SymTab::getDepth() const {
    return depth;
}

/*std::queue<SymTab *> SymTab::dfs() {
    std::queue<SymTab *> res;

    std::stack<SymTab *> stack;
    stack.push(this);

    while (!stack.empty()) {
        SymTab *node = stack.top();
        stack.pop();

        res.push(node);

        for (auto it = node->next.rbegin(); it != node->next.rend(); ++it) {
            stack.push(it->get());
        }
    }

    return res;
}*/
