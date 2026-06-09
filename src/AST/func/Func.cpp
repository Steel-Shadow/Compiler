//
// Created by Steel_Shadow on 2023/10/11.
//

#include "Func.h"

#include "errorHandler/Error.h"
#include "frontend/parser/Parser.h"
#include "frontend/symTab/SymTab.h"

using namespace Parser;

bool canStartFuncFParam(LexType type) {
    return type == LexType::INTTK || type == LexType::CHARTK;
}

// const array can't be param
std::unique_ptr<FuncDef> FuncDef::parse() {
    auto n = std::make_unique<FuncDef>();

    n->funcType = FuncType::parse();

    int row = Lexer::curRow;
    n->ident = Ident::parse();
    if (SymTab::reDefine(n->ident)) {
        Error::raise('b', row);
    }

    SymTab::deepIn();

    std::vector<Param> params{};

    singleLex(LexType::LPARENT);
    if (canStartFuncFParam(Lexer::curLexType)) {
        n->funcFParams = FuncFParams::parse();
        params = n->funcFParams->getParameters();
    }
    singleLex(LexType::RPARENT, row);

    SymTab::add(n->ident, Symbol(n->funcType->getType(), params), SymTab::cur->getPrev());

    Stmt::retType = n->funcType->getType();
    Stmt::retVoid = Stmt::retType == Type::Void;
    n->block = Block::parse();

    if (!Stmt::retVoid) {
        if (n->block->getBlockItems().empty()
            || !dynamic_cast<ReturnStmt *>(n->block->getBlockItems().back().get())) {
            // In fact, we should check "return;"
            // But it's not included in our work.
            Error::raise('g', Block::lastRow);
        }
    }

    SymTab::deepOut(); // FuncDef
    output(AST::FuncDef);
    return n;
}

std::unique_ptr<MainFuncDef> MainFuncDef::parse() {
    auto n = std::make_unique<MainFuncDef>();

    singleLex(LexType::INTTK);
    singleLex(LexType::MAINTK);

    SymTab::deepIn();

    int row = Lexer::curRow;
    singleLex(LexType::LPARENT);
    singleLex(LexType::RPARENT, row);

    Stmt::retType = Type::Int;
    Stmt::retVoid = false;
    n->block = Block::parse();

    if (!Stmt::retVoid) {
        if (n->block->getBlockItems().empty() || !dynamic_cast<ReturnStmt *>(n->block->getBlockItems().back().get())) {
            // In fact, we should check "return;"
            // But it's not included in our work.
            Error::raise('g', Block::lastRow);
        }
    }

    SymTab::deepOut(); // MainFuncDef
    output(AST::MainFuncDef);
    return n;
}

std::unique_ptr<IR::Function> MainFuncDef::genIR() const {
    using namespace IR;
    auto main = std::make_unique<Function>(
            "main", Type::Int, std::vector<Param>());

    BasicBlocks bBlocks;
    bBlocks.emplace_back(std::make_unique<BasicBlock>("main", true));
    SymTab::iterIn();

    Function::idAllocator = 0;
    block->genIR(bBlocks);

    main->moveBasicBlocks(std::move(bBlocks));
    SymTab::iterOut();
    return main;
}

std::unique_ptr<FuncType> FuncType::parse() {
    auto n = std::make_unique<FuncType>();

    if (Lexer::curLexType == LexType::VOIDTK || Lexer::curLexType == LexType::INTTK || Lexer::curLexType == LexType::CHARTK) {
        n->type = Lexer::curLexType;
        Lexer::next();
    } else {
        Error::raise();
    }

    output(AST::FuncType);
    return n;
}

Type FuncType::getType() const {
    return toType(type);
}

std::unique_ptr<FuncFParams> FuncFParams::parse() {
    auto n = std::make_unique<FuncFParams>();

    n->funcFParams.push_back(FuncFParam::parse());

    while (Lexer::curLexType == LexType::COMMA) {
        Lexer::next();
        n->funcFParams.push_back(FuncFParam::parse());
    }

    output(AST::FuncFParams);
    return n;
}

std::vector<Param> FuncFParams::getParameters() const {
    std::vector<Param> params;
    params.reserve(funcFParams.size());
    for (auto &i: funcFParams) {
        auto sym = SymTab::find(i->ident);
        params.emplace_back(i->ident, sym);
    }
    return params;
}

std::unique_ptr<FuncFParam> FuncFParam::parse() {
    auto n = std::make_unique<FuncFParam>();

    n->type = Btype::parse();

    int row = Lexer::curRow; // error handle
    n->ident = Ident::parse();
    if (SymTab::reDefine(n->ident)) {
        Error::raise('b', row);
    }

    // ['[' ']' { '[' ConstExp ']' }]
    if (Lexer::curLexType == LexType::LBRACK) {
        row = Lexer::curRow;
        Lexer::next();
        n->dims.push_back(nullptr); // p[] is not p!
        singleLex(LexType::RBRACK, row);

        while (Lexer::curLexType == LexType::LBRACK) {
            Lexer::next();

            row = Lexer::curRow;
            n->dims.push_back(Exp::parse(true));
            singleLex(LexType::RBRACK, row);
        }
    }

    if (n->dims.empty()) {
        SymTab::add(n->getId(), Symbol(toType(n->type->type), std::vector<int>{}));
    } else {
        SymTab::add(n->getId(), Symbol(valueToPtr(toType(n->type->type)), n->getDims()));
    }
    output(AST::FuncFParam);
    return n;
}

const std::string &FuncFParam::getId() const {
    return ident;
}

std::vector<int> FuncFParam::getDims() const {
    std::vector<int> dimsValue;
    dimsValue.reserve(dims.size());
    for (auto &i: dims) {
        dimsValue.push_back(i == nullptr ? 0 : i->evaluate());
    }
    return dimsValue;
}

std::unique_ptr<FuncRParams> FuncRParams::parse() {
    auto n = std::make_unique<FuncRParams>();

    n->params.push_back(Exp::parse(false));
    while (Lexer::curLexType == LexType::COMMA) {
        Lexer::next();

        n->params.push_back(Exp::parse(false));
    }

    output(AST::FuncRParams);
    return n;
}

std::unique_ptr<IR::Function> FuncDef::genIR() {
    using namespace IR;
    auto functionSymbol = SymTab::find(ident);
    auto function = std::make_unique<Function>(ident, functionSymbol->type, functionSymbol->params);

    BasicBlocks bBlocks;
    Function::idAllocator = 0;

    SymTab::iterIn();

    bBlocks.emplace_back(std::make_unique<BasicBlock>(ident, true));
    block->genIR(bBlocks);

    if (bBlocks.back()->instructions.empty() || bBlocks.back()->instructions.back().op != Op::Ret) {
        bBlocks.back()->addInst(Inst(Op::Ret,
                                     nullptr,
                                     nullptr,
                                     nullptr));
    }

    function->moveBasicBlocks(std::move(bBlocks));
    SymTab::iterOut();
    return function;
}
