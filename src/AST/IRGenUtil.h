//
// Created by Codex on 2026/06/09.
//

#ifndef COMPILER_IRGENUTIL_H
#define COMPILER_IRGENUTIL_H

#include "frontend/symTab/Symbol.h"
#include "middle/IR.h"

#include <memory>
#include <string>

std::unique_ptr<IR::Var> makeIRVar(const Symbol *symbol, const std::string &ident, int depth);
std::unique_ptr<IR::Var> makeIRVar(const Symbol *symbol, const std::string &ident, int depth, Type type);
std::unique_ptr<IR::Var> makeIRVar(const Symbol *storageSymbol, const std::string &ident, int depth, const Symbol *attributeSymbol);

#endif
