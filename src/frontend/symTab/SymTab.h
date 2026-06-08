//
// Created by Steel_Shadow on 2023/10/15.
//

#ifndef COMPILER_SYMTAB_H
#define COMPILER_SYMTAB_H

#include "Symbol.h"

#include <list>
#include <memory>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>

// tree
// global -next-> SymTab... -next-> *cur
class SymTab {
    SymTab *prev; // prev SymTable

    std::vector<std::unique_ptr<SymTab>> next; // next SymTable

    // Ident to Symbol
    std::unordered_map<std::string, Symbol> symbols;

    int depth;

    static std::list<SymTab *> symTabs;

public:
    static SymTab *cur;
    static SymTab global;

    static std::vector<std::set<std::pair<std::string, int>>> knownVars;
    static std::vector<Symbol *> staticVars;

    explicit SymTab(SymTab *prev);

    static bool reDefine(const std::string &ident);

    static Symbol *find(const std::string &ident);
    static std::pair<Symbol *, int> findInGen(const std::string &ident);

    // find depth of Symbol from current SymTab
    // return -1 if not found
    static int findDepth(const std::string &ident);

    // no effect if reDefine(ident)
    static void add(const std::string &ident, Symbol &&symbol, SymTab *where = cur);

    static const std::vector<Symbol *> &getStaticVars();

    static void addBuiltins();

    // create a new empty SymTab, and set cur to the new one
    static void deepIn();

    static void deepOut();

    SymTab *getPrev() const;

    // breadth-first search
    static void iterIn();
    static void iterOut();

    int getDepth() const;

    // std::queue<SymTab *> dfs();
};


#endif
