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
