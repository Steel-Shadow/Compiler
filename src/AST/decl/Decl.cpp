//
// Created by Steel_Shadow on 2023/10/11.
//
#include "Decl.h"

#include "Def.h"
#include "errorHandler/Error.h"
#include "frontend/parser/Parser.h"
#include "frontend/symTab/SymTab.h"

using namespace Parser;

std::unique_ptr<Decl> Decl::parse() {
    auto n = std::make_unique<Decl>();

    if (Lexer::curLexType == LexType::CONSTTK) {
        Lexer::next();
        n->cons = true;
    } else {
        n->cons = false;
        if (Lexer::curLexType == LexType::STATICTK) {
            Lexer::next();
            n->statik = true;
        }
    }

    n->btype = Btype::parse();

    int row = Lexer::curRow;
    n->defs.push_back(Def::parse(n->cons, toType(n->btype->type), n->statik));

    while (Lexer::curLexType == LexType::COMMA) {
        Lexer::next();
        row = Lexer::curRow;
        n->defs.push_back(Def::parse(n->cons, toType(n->btype->type), n->statik));
    }

    singleLex(LexType::SEMICN, row);

    if (n->cons) {
        output(AST::ConstDecl);
    } else {
        output(AST::VarDecl);
    }
    //    output(AST::Decl);
    return n;
}

const std::vector<std::unique_ptr<Def>> &Decl::getDefs() const {
    return defs;
}

void Decl::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    for (auto &def: defs) {
        def->genIR(bBlocks, toType(btype->type));
    }
}

int Def::getArraySize() const {
    auto *symbol = SymTab::find(ident)->asObject();
    int size = 1;
    for (const auto &i: symbol->getDims()) {
        size *= i;
    }
    return size;
}

std::unique_ptr<Btype> Btype::parse() {
    auto n = std::make_unique<Btype>();

    if (Lexer::curLexType == LexType::INTTK || Lexer::curLexType == LexType::CHARTK) {
        n->type = Lexer::curLexType;
        Lexer::next();
    } else {
        Error::raise();
    }
    //    output(AST::Btype);
    return n;
}

std::string Ident::parse() {
    std::string ident;

    if (Lexer::curLexType == LexType::IDENFR) {
        ident = Lexer::curToken;
        Lexer::next();
    } else {
        Error::raise();
    }

    return ident;
}
