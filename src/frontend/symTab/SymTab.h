//
// Created by Steel_Shadow on 2023/10/15.
//

#ifndef COMPILER_SYMTAB_H
#define COMPILER_SYMTAB_H

#include "Symbol.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
    static std::vector<SymTab *> traversalOrder;
    static size_t traversalCursor;

public:
    explicit SymTab(SymTab *prev);

    static void resetToGlobal();
    static int currentDepth();
    static SymTab *currentParent();

    static bool reDefine(const std::string &ident);

    static Symbol *find(const std::string &ident);
    static std::pair<Symbol *, int> findWithDepth(const std::string &ident);
    static std::vector<std::pair<Symbol *, int>> findAllWithDepth(const std::string &ident);

    // no effect if reDefine(ident)
    static void add(const std::string &ident, std::unique_ptr<Symbol> symbol, SymTab *where = cur);

    static void addBuiltins();

    // create a new empty SymTab, and set cur to the new one
    static void deepIn();

    static void deepOut();

    static void resetTraversal();
    static void enterRecordedScope();
    static void leaveRecordedScope();

    int getDepth() const;
};


#endif
