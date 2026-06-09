//
// Created by Steel_Shadow on 2023/10/11.
//

#include "CompUnit.h"

#include "frontend/lexer/Lexer.h"
#include "frontend/parser/Parser.h"
#include "frontend/symTab/SymTab.h"

using namespace Parser;

std::unique_ptr<CompUnit> CompUnit::parse() {
    auto n = std::make_unique<CompUnit>();

    SymTab::addBuiltins();

    while (Lexer::curLexType == LexType::CONSTTK || Lexer::curLexType == LexType::INTTK || Lexer::curLexType == LexType::CHARTK) {
        if ((Lexer::peek(1).first == LexType::IDENFR && Lexer::peek(2).first == LexType::LPARENT)
            || (Lexer::curLexType == LexType::INTTK && Lexer::peek(1).first == LexType::MAINTK)) {
            break;
        }
        n->decls.push_back(Decl::parse());
    }

    while (Lexer::curLexType == LexType::VOIDTK || Lexer::curLexType == LexType::INTTK || Lexer::curLexType == LexType::CHARTK) {
        if (Lexer::curLexType == LexType::INTTK && Lexer::peek(1).first == LexType::MAINTK) {
            break;
        }
        n->funcDefs.push_back(FuncDef::parse());
    }

    n->mainFuncDef = MainFuncDef::parse();

    output(AST::CompUnit);

    return n;
}

std::unique_ptr<IR::Module> CompUnit::genIR() const {
    using namespace IR;

    auto module = std::make_unique<Module>("Write by Steel Shadow");

    // maybe redundant, but still set it for safety
    SymTab::resetToGlobal();
    SymTab::enterGeneratedVarScope();
    for (auto &decl: decls) {
        for (auto &def: decl->getDefs()) {
            auto *sym = SymTab::find(def->ident)->asValue();

            auto globVar = GlobVar(sym->isConst(), sym->getType(), sym->getDims(), sym->getInitVal());
            SymTab::recordGeneratedVar(def->ident, 0);
            module->addGlobVar(def->ident, globVar);
        }
    }
    for (auto *sym: SymTab::getStaticVars()) {
        module->addGlobVar(sym->getStaticStorageName(), GlobVar(sym->isConst(), sym->getType(), sym->getDims(), sym->getInitVal()));
    }

    for (auto &funcDef: funcDefs) {
        module->addFunction(funcDef->genIR());
    }

    ReturnStmt::inMainGen = true;
    module->setMainFunction(mainFuncDef->genIR());

    return module;
}
