//
// Created by Steel_Shadow on 2023/10/11.
//

#ifndef COMPILER_COMPUNIT_H
#define COMPILER_COMPUNIT_H

#include <memory>
#include <vector>


#include "decl/Decl.h"
#include "func/Func.h"

// CompUnit → {Decl} {FuncDef} MainFuncDef
struct CompUnit {
    std::vector<std::unique_ptr<Decl>> decls;
    std::vector<std::unique_ptr<FuncDef>> funcDefs;
    std::unique_ptr<MainFuncDef> mainFuncDef;

    static std::unique_ptr<CompUnit> parse();
};

#endif
