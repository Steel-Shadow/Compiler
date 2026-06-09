//
// Created by Steel_Shadow on 2023/10/15.
//

#ifndef COMPILER_SYMTAB_H
#define COMPILER_SYMTAB_H

#include "Symbol.h"

#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

// tree
// global -next-> SymTab... -next-> *cur
class SymTab {
    SymTab *prev; // prev SymTable

    std::vector<std::unique_ptr<SymTab>> next; // next SymTable

    // Ident to Symbol
    std::unordered_map<std::string, std::unique_ptr<Symbol>> symbols;

    int depth;

    static SymTab *cur;
    static SymTab global;
    static std::list<SymTab *> symTabs;
    static std::vector<std::set<std::pair<std::string, int>>> generatedVars;
    static std::vector<ValueSymbol *> staticVars;

public:
    explicit SymTab(SymTab *prev);

    static void resetToGlobal();
    static int currentDepth();
    static SymTab *currentParent();

    static bool reDefine(const std::string &ident);

    static Symbol *find(const std::string &ident);
    static std::pair<Symbol *, int> findInGen(const std::string &ident);

    // no effect if reDefine(ident)
    static void add(const std::string &ident, std::unique_ptr<Symbol> symbol, SymTab *where = cur);

    static void enterGeneratedVarScope();
    static void leaveGeneratedVarScope();
    static void recordGeneratedVar(const std::string &ident, int depth);

    static const std::vector<ValueSymbol *> &getStaticVars();

    static void addBuiltins();

    // create a new empty SymTab, and set cur to the new one
    static void deepIn();

    static void deepOut();

    // breadth-first search
    static void iterIn();
    static void iterOut();

    int getDepth() const;
};


#endif
