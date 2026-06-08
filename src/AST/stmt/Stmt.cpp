//
// Created by Steel_Shadow on 2023/10/12.
//

#include "Stmt.h"

#include "AST/decl/Decl.h"
#include "AST/expr/Exp.h"
#include "backend/Instruction.h"
#include "errorHandler/Error.h"
#include "frontend/parser/Parser.h"
#include "frontend/symTab/SymTab.h"

#include <set>

using namespace Parser;

int Block::lastRow;

std::unique_ptr<Block> Block::parse() {
    auto n = std::make_unique<Block>();

    singleLex(LexType::LBRACE);

    while (Lexer::curLexType != LexType::RBRACE && Lexer::curLexType != LexType::LEX_END) {
        auto i = BlockItem::parse();
        n->blockItems.push_back(std::move(i));
    }

    lastRow = Lexer::curRow;
    if (Lexer::curLexType == LexType::RBRACE) {
        Lexer::next(); // }
    }

    output(AST::Block);
    return n;
}

const std::vector<std::unique_ptr<BlockItem>> &Block::getBlockItems() const {
    return blockItems;
}

std::unique_ptr<BlockItem> BlockItem::parse() {
    std::unique_ptr<BlockItem> n;

    // Maybe error when neither Decl nor Stmt. But it's too complicated.
    if (Lexer::curLexType == LexType::CONSTTK || Lexer::curLexType == LexType::STATICTK
        || Lexer::curLexType == LexType::INTTK || Lexer::curLexType == LexType::CHARTK) {
        n = Decl::parse();
    } else {
        n = Stmt::parse();
    }

    // output(AST::BlockItem);
    return n;
}

void Block::genIR(IR::BasicBlocks &basicBlocks) const {
    using namespace IR;
    for (auto &i: blockItems) {
        i->genIR(basicBlocks);
    }
}

bool Stmt::retVoid;
Type Stmt::retType = Type::Void;

int ControlFlow::loopDepth = 0;
int ControlFlow::switchDepth = 0;
std::stack<IR::Label> ControlFlow::breakLabels{};
std::stack<IR::Label> ControlFlow::continueLabels{};

std::unique_ptr<Stmt> Stmt::parse() {
    std::unique_ptr<Stmt> n;

    switch (Lexer::curLexType) {
        case LexType::LBRACE:
            n = BlockStmt::parse();
            break;
        case LexType::IFTK:
            n = IfStmt::parse();
            break;
        case LexType::WHILETK:
            n = WhileStmt::parse();
            break;
        case LexType::SWITCHTK:
            n = SwitchStmt::parse();
            break;
        case LexType::BREAKTK:
            n = BreakStmt::parse();
            break;
        case LexType::CONTINUETK:
            n = ContinueStmt::parse();
            break;
        case LexType::FORTK:
            n = BigForStmt::parse();
            break;
        case LexType::RETURNTK:
            n = ReturnStmt::parse();
            break;
        case LexType::PRINTFTK:
            n = PrintStmt::parse();
            break;
        case LexType::SEMICN:
            n = std::make_unique<ExpStmt>();
            Lexer::next();
            break;
        case LexType::IDENFR:
            //find '=' to distinguish LVal and Exp
            if (Lexer::findAssignBeforeSemicolon()) {
                n = LValStmt::parse();
            } else {
                n = ExpStmt::parse();
            }
            break;
        default:
            n = ExpStmt::parse();
            break;
    }

    output(AST::Stmt);
    return n;
}

std::unique_ptr<IfStmt> IfStmt::parse() {
    auto n = std::make_unique<IfStmt>();

    Lexer::next();

    SymTab::deepIn();
    singleLex(LexType::LPARENT);

    int row = Lexer::curRow;
    n->cond = Cond::parse();
    singleLex(LexType::RPARENT, row);
    n->ifStmt = Stmt::parse();

    if (Lexer::curLexType == LexType::ELSETK) {
        Lexer::next();
        n->elseStmt = Stmt::parse();
    }

    SymTab::deepOut(); // IfStmt
    return n;
}

void IfStmt::genIR(IR::BasicBlocks &bBlocks) {
    SymTab::iterIn();
    bBlocks.back()->addInst(IR::Inst(
            IR::Op::InStack, nullptr, nullptr, nullptr));

    auto trueBranch = std::make_unique<IR::BasicBlock>("IfTrueBranch");
    auto falseBranch = std::make_unique<IR::BasicBlock>("IfFalseBranch");
    auto ifEnd = std::make_unique<IR::BasicBlock>("IfEnd");

    cond->genIR(bBlocks, trueBranch->label, falseBranch->label);

    bBlocks.emplace_back(std::move(trueBranch));
    ifStmt->genIR(bBlocks);
    bBlocks.back()->addInst(IR::Inst(IR::Op::Br, nullptr, std::make_unique<IR::Label>(ifEnd->label), nullptr));

    bBlocks.emplace_back(std::move(falseBranch));
    if (elseStmt) {
        elseStmt->genIR(bBlocks);
    }

    bBlocks.emplace_back(std::move(ifEnd));

    bBlocks.back()->addInst(IR::Inst(
            IR::Op::OutStack, nullptr, nullptr, nullptr));
    SymTab::iterOut();
}

int BigForStmt::inForDepth = 0;

std::unique_ptr<BigForStmt> BigForStmt::parse() {
    auto n = std::make_unique<BigForStmt>();

    inForDepth++;
    ControlFlow::loopDepth++;

    Lexer::next();
    singleLex(LexType::LPARENT);

    SymTab::deepIn();

    if (Lexer::curLexType != LexType::SEMICN) {
        n->init = ForStmt::parse();
    }
    singleLex(LexType::SEMICN);

    if (Lexer::curLexType != LexType::SEMICN) {
        n->cond = Cond::parse();
    }
    singleLex(LexType::SEMICN);

    if (Lexer::curLexType != LexType::RPARENT) {
        n->iter = ForStmt::parse();
    }
    singleLex(LexType::RPARENT);

    n->stmt = Stmt::parse();

    inForDepth--;
    ControlFlow::loopDepth--;
    SymTab::deepOut(); // ForStmt
    return n;
}

std::stack<IR::Label> BigForStmt::stackEndLabel{};
std::stack<IR::Label> BigForStmt::stackIterLabel{};

void BigForStmt::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;

    SymTab::iterIn();
    bBlocks.back()->addInst(IR::Inst(
            IR::Op::InStack, nullptr, nullptr, nullptr));
    if (init) {
        init->genIR(bBlocks);
    }

    auto forBodyBlock = std::make_unique<BasicBlock>("ForBody");
    auto forIterCondBlock = std::make_unique<BasicBlock>("ForIter");
    auto forEndBlock = std::make_unique<BasicBlock>("ForEnd");

    stackEndLabel.push(forEndBlock->label);
    stackIterLabel.push(forIterCondBlock->label);
    ControlFlow::breakLabels.push(forEndBlock->label);
    ControlFlow::continueLabels.push(forIterCondBlock->label);

    // use unique_ptr after move
    auto pForBodyBlock = forBodyBlock.get();
    // auto pForIterCondBlock = forIterCondBlock.get();

    if (cond) {
        cond->genIR(bBlocks, forBodyBlock->label, forEndBlock->label);
    }

    bBlocks.push_back(std::move(forBodyBlock));
    if (stmt) {
        stmt->genIR(bBlocks);
    }

    bBlocks.push_back(std::move(forIterCondBlock));
    if (iter) {
        iter->genIR(bBlocks);
    }
    if (cond) {
        cond->genIR(bBlocks, pForBodyBlock->label, forEndBlock->label);
    } else {
        bBlocks.back()->addInst(Inst(Op::Br, nullptr, std::make_unique<Label>(pForBodyBlock->label), nullptr));
    }

    bBlocks.push_back(std::move(forEndBlock));

    stackEndLabel.pop();
    stackIterLabel.pop();
    ControlFlow::breakLabels.pop();
    ControlFlow::continueLabels.pop();

    SymTab::iterOut();
    bBlocks.back()->addInst(IR::Inst(
            IR::Op::OutStack, nullptr, nullptr, nullptr));
}

std::unique_ptr<ForStmt> ForStmt::parse() {
    auto n = std::make_unique<ForStmt>();

    n->lVal = LVal::parse();
    singleLex(LexType::ASSIGN);
    n->exp = Exp::parse(false);

    output(AST::ForStmt);
    return n;
}

void ForStmt::genIR(IR::BasicBlocks &basicBlocks) const {
    using namespace IR;
    auto t = exp->genIR(basicBlocks);

    auto [sym, depth] = SymTab::findInGen(lVal->ident);
    auto irLVal = std::make_unique<Var>(getStorageName(sym, lVal->ident),
                                        getStorageDepth(sym, depth),
                                        sym->cons,
                                        sym->dims,
                                        sym->type);
    basicBlocks.back()->addInst(Inst(IR::Op::Store,
                                     std::move(t),
                                     std::move(irLVal),
                                     nullptr));
}

std::unique_ptr<BreakStmt> BreakStmt::parse() {
    int row = Lexer::curRow;

    if (ControlFlow::loopDepth == 0 && ControlFlow::switchDepth == 0) {
        Error::raise('m', row);
    }

    Lexer::next();
    singleLex(LexType::SEMICN, row);
    return std::make_unique<BreakStmt>();
}

void BreakStmt::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    bBlocks.back()->addInst(Inst(IR::Op::Br,
                                 nullptr,
                                 std::make_unique<Label>(ControlFlow::breakLabels.top()),
                                 nullptr));
}

std::unique_ptr<ContinueStmt> ContinueStmt::parse() {
    int row = Lexer::curRow;

    if (ControlFlow::loopDepth == 0) {
        Error::raise('m', row);
    }

    Lexer::next();
    singleLex(LexType::SEMICN, row);
    return std::make_unique<ContinueStmt>();
}

void ContinueStmt::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    bBlocks.back()->addInst(Inst(IR::Op::Br,
                                 nullptr,
                                 std::make_unique<Label>(ControlFlow::continueLabels.top()),
                                 nullptr));
}

std::unique_ptr<WhileStmt> WhileStmt::parse() {
    auto n = std::make_unique<WhileStmt>();

    ControlFlow::loopDepth++;
    Lexer::next();

    SymTab::deepIn();
    singleLex(LexType::LPARENT);

    int row = Lexer::curRow;
    n->cond = Cond::parse();
    singleLex(LexType::RPARENT, row);

    n->stmt = Stmt::parse();

    SymTab::deepOut();
    ControlFlow::loopDepth--;
    return n;
}

void WhileStmt::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    SymTab::iterIn();
    bBlocks.back()->addInst(Inst(Op::InStack, nullptr, nullptr, nullptr));

    auto condBlock = std::make_unique<BasicBlock>("WhileCond");
    auto bodyBlock = std::make_unique<BasicBlock>("WhileBody");
    auto endBlock = std::make_unique<BasicBlock>("WhileEnd");

    bBlocks.back()->addInst(Inst(Op::Br, nullptr, std::make_unique<Label>(condBlock->label), nullptr));

    auto condLabel = condBlock->label;
    auto bodyLabel = bodyBlock->label;
    auto endLabel = endBlock->label;

    ControlFlow::breakLabels.push(endLabel);
    ControlFlow::continueLabels.push(condLabel);

    bBlocks.emplace_back(std::move(condBlock));
    cond->genIR(bBlocks, bodyLabel, endLabel);

    bBlocks.emplace_back(std::move(bodyBlock));
    stmt->genIR(bBlocks);
    bBlocks.back()->addInst(Inst(Op::Br, nullptr, std::make_unique<Label>(condLabel), nullptr));

    bBlocks.emplace_back(std::move(endBlock));

    ControlFlow::breakLabels.pop();
    ControlFlow::continueLabels.pop();

    bBlocks.back()->addInst(Inst(Op::OutStack, nullptr, nullptr, nullptr));
    SymTab::iterOut();
}

std::unique_ptr<CaseStmt> CaseStmt::parse() {
    auto n = std::make_unique<CaseStmt>();

    if (Lexer::curLexType == LexType::CASETK) {
        Lexer::next();
        n->number = Number::parse();
        singleLex(LexType::COLON);
    } else if (Lexer::curLexType == LexType::DEFAULTTK) {
        n->isDefault = true;
        Lexer::next();
        singleLex(LexType::COLON);
    } else {
        Error::raise();
    }

    while (Lexer::curLexType != LexType::CASETK
           && Lexer::curLexType != LexType::DEFAULTTK
           && Lexer::curLexType != LexType::RBRACE
           && Lexer::curLexType != LexType::LEX_END) {
        n->stmts.push_back(Stmt::parse());
    }

    return n;
}

std::unique_ptr<SwitchStmt> SwitchStmt::parse() {
    auto n = std::make_unique<SwitchStmt>();

    Lexer::next();
    singleLex(LexType::LPARENT);
    int row = Lexer::curRow;
    n->exp = Exp::parse(false);
    singleLex(LexType::RPARENT, row);
    singleLex(LexType::LBRACE);

    ControlFlow::switchDepth++;
    std::set<int> caseValues;
    bool hasDefault = false;
    Type switchType = ptrToValue(n->exp->getType());

    while (Lexer::curLexType == LexType::CASETK || Lexer::curLexType == LexType::DEFAULTTK) {
        int caseRow = Lexer::curRow;
        auto caseStmt = CaseStmt::parse();
        if (caseStmt->isDefault) {
            if (hasDefault) {
                Error::raise('n', caseRow);
            }
            hasDefault = true;
        } else {
            int value = caseStmt->number->evaluate();
            if (caseStmt->number->getType() != switchType) {
                Error::raise('e', caseRow);
            }
            if (!caseValues.insert(value).second) {
                Error::raise('n', caseRow);
            }
        }
        n->cases.emplace_back(std::move(caseStmt));
    }

    ControlFlow::switchDepth--;
    singleLex(LexType::RBRACE);
    return n;
}

void SwitchStmt::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    auto value = exp->genIR(bBlocks);
    static int switchId = 0;
    std::string switchName = "__switch_" + std::to_string(switchId++);
    int switchDepth = SymTab::cur->getDepth();
    auto switchVar = std::make_unique<Var>(switchName, switchDepth, false, std::vector<int>{}, value->type);
    auto switchVarCopy = std::make_unique<Var>(*switchVar);
    bBlocks.back()->addInst(Inst(Op::Alloca,
                                 nullptr,
                                 std::move(switchVar),
                                 std::make_unique<ConstVal>(1, Type::Int)));
    bBlocks.back()->addInst(Inst(Op::Store,
                                 std::move(value),
                                 std::move(switchVarCopy),
                                 nullptr));

    std::vector<std::unique_ptr<BasicBlock>> caseBlocks;
    caseBlocks.reserve(cases.size());
    BasicBlock *defaultBlock = nullptr;
    for (auto &caseStmt: cases) {
        auto block = std::make_unique<BasicBlock>(caseStmt->isDefault ? "SwitchDefault" : "SwitchCase");
        if (caseStmt->isDefault) {
            defaultBlock = block.get();
        }
        caseBlocks.emplace_back(std::move(block));
    }
    auto endBlock = std::make_unique<BasicBlock>("SwitchEnd");
    auto endLabel = endBlock->label;

    for (int i = 0; i < cases.size(); ++i) {
        if (cases[i]->isDefault) {
            continue;
        }
        auto switchValue = std::make_unique<Temp>(exp->getType());
        bBlocks.back()->addInst(Inst(Op::Load,
                                     std::make_unique<Temp>(*switchValue),
                                     std::make_unique<Var>(switchName, switchDepth, false, std::vector<int>{}, exp->getType()),
                                     nullptr));
        auto caseValue = std::make_unique<Temp>(cases[i]->number->getType());
        bBlocks.back()->addInst(Inst(Op::LoadImd,
                                     std::make_unique<Temp>(*caseValue),
                                     std::make_unique<ConstVal>(cases[i]->number->evaluate(), cases[i]->number->getType()),
                                     nullptr));
        auto cmp = std::make_unique<Temp>(Type::Int);
        bBlocks.back()->addInst(Inst(Op::Eql,
                                     std::make_unique<Temp>(*cmp),
                                     std::move(switchValue),
                                     std::move(caseValue)));
        bBlocks.back()->addInst(Inst(Op::Bif1,
                                     nullptr,
                                     std::move(cmp),
                                     std::make_unique<Label>(caseBlocks[i]->label)));
    }

    bBlocks.back()->addInst(Inst(Op::Br,
                                 nullptr,
                                 std::make_unique<Label>(defaultBlock ? defaultBlock->label : endLabel),
                                 nullptr));

    ControlFlow::breakLabels.push(endLabel);
    for (int i = 0; i < cases.size(); ++i) {
        bBlocks.emplace_back(std::move(caseBlocks[i]));
        for (auto &stmt: cases[i]->stmts) {
            stmt->genIR(bBlocks);
        }
    }
    ControlFlow::breakLabels.pop();

    bBlocks.emplace_back(std::move(endBlock));
}

std::unique_ptr<ReturnStmt> ReturnStmt::parse() {
    auto n = std::make_unique<ReturnStmt>();

    Lexer::next();

    if (Lexer::curLexType == LexType::SEMICN) {
        if (!Stmt::retVoid) {
            Error::raise('e');
        }
        Lexer::next();
    } else {
        if (Stmt::retVoid) {
            Error::raise('f');
        }
        int row = Lexer::curRow; // error handle
        n->exp = Exp::parse(false);
        if (!Stmt::retVoid && n->exp->getType() != Stmt::retType) {
            Error::raise('e', row);
        }
        singleLex(LexType::SEMICN, row);
    }

    return n;
}

bool ReturnStmt::inMainGen;

void ReturnStmt::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    if (exp) {
        auto temp = exp->genIR(bBlocks);
        bBlocks.back()->addInst(Inst(inMainGen ? Op::RetMain : Op::Ret,
                                     nullptr,
                                     std::move(temp),
                                     nullptr));
    } else {
        bBlocks.back()->addInst(Inst(inMainGen ? Op::RetMain : Op::Ret,
                                     nullptr,
                                     nullptr,
                                     nullptr));
    }
}

void PrintStmt::checkFormatString(const std::string &str) {
    // skip begin and end " "
    for (int i = 1; i < str.length() - 1; i++) {
        char c = str[i];

        if (c == '\\') {
            if (str[++i] != 'n') {
                Error::raise('a');
            }
        } else if (c == '%') {
            char format = str[++i];
            if (format != 'd' && format != 'c' && format != 's') {
                Error::raise('a');
            } else {
                formatTypes.push_back(format);
                numOfFormat++;
            }
        } else if (!(c == 32 || c == 33 || c >= 40 && c <= 126)) {
            Error::raise('a');
        }
    }
}

std::unique_ptr<PrintStmt> PrintStmt::parse() {
    auto n = std::make_unique<PrintStmt>();

    int row = Lexer::curRow;
    Lexer::next();

    singleLex(LexType::LPARENT);

    if (Lexer::curLexType == LexType::STRCON) {
        n->checkFormatString(Lexer::curToken);
        n->formatString = Lexer::curToken;
        Lexer::next();
    } else {
        Error::raise();
    }

    int numOfExp = 0;
    while (Lexer::curLexType == LexType::COMMA) {
        Lexer::next();
        numOfExp++;
        n->exps.push_back(Exp::parse(false));
    }

    if (numOfExp != n->numOfFormat) {
        Error::raise('l', row);
    }
    for (int i = 0; i < n->exps.size() && i < n->formatTypes.size(); ++i) {
        auto remainingRank = [](const std::unique_ptr<Exp> &exp) -> size_t {
            auto lVal = exp->getLVal();
            if (!lVal) {
                return 0;
            }
            auto sym = SymTab::find(lVal->getIdent());
            if (!sym || sym->symType == SymType::Func || sym->dims.size() <= lVal->getRank()) {
                return 0;
            }
            return sym->dims.size() - lVal->getRank();
        };

        Type expType = ptrToValue(n->exps[i]->getType());
        if (((n->formatTypes[i] == 'd' || n->formatTypes[i] == 'c') && remainingRank(n->exps[i]) > 0)
            || (n->formatTypes[i] == 'd' && expType != Type::Int)
            || (n->formatTypes[i] == 'c' && expType != Type::Char)) {
            Error::raise('e', row);
        } else if (n->formatTypes[i] == 's') {
            auto lVal = n->exps[i]->getLVal();
            auto ident = n->exps[i]->getIdent();
            auto sym = ident.empty() ? nullptr : SymTab::find(ident);
            if (lVal == nullptr || sym == nullptr || ptrToValue(sym->type) != Type::Char || sym->dims.size() <= lVal->getRank()) {
                Error::raise('e', row);
            }
        }
    }

    singleLex(LexType::RPARENT, row);
    singleLex(LexType::SEMICN, row);
    return n;
}

void PrintStmt::addStr(const IR::BasicBlocks &bBlocks, std::string &buffer) {
    if (buffer.empty()) {
        return;
    }

    IR::Str::MIPS_strings.push_back('\"' + buffer + '\"');
    buffer.clear();
    bBlocks.back()->addInst(IR::Inst(IR::Op::PrintStr,
                                     nullptr,
                                     std::make_unique<IR::Str>(),
                                     nullptr));
}

void PrintStmt::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;

    struct PrintArg {
        char format{};
        std::unique_ptr<Temp> value;
        std::unique_ptr<Var> var;
        std::unique_ptr<Element> offset;
    };

    std::vector<PrintArg> args;
    args.reserve(formatTypes.size());

    for (int i = 0; i < exps.size() && i < formatTypes.size(); ++i) {
        PrintArg arg;
        arg.format = formatTypes[i];
        if (arg.format == 'd' || arg.format == 'c') {
            arg.value = exps[i]->genIR(bBlocks);
        } else if (arg.format == 's') {
            auto lVal = exps[i]->getLVal();
            auto [symbol, depth] = SymTab::findInGen(lVal->ident);
            arg.var = std::make_unique<IR::Var>(
                    getStorageName(symbol, lVal->ident),
                    getStorageDepth(symbol, depth),
                    symbol->cons,
                    symbol->dims,
                    symbol->type,
                    symbol->symType);
            int constOffset = 0;
            std::unique_ptr<Temp> dynamicOffset;
            bool getNonConstIndex = lVal->getOffset(constOffset, dynamicOffset, bBlocks, symbol->dims, symbol->type);
            if (getNonConstIndex) {
                arg.offset = std::move(dynamicOffset);
            } else {
                arg.offset = std::make_unique<ConstVal>(constOffset, Type::Int);
            }
        }
        args.push_back(std::move(arg));
    }

    // string | %d | %c | %s
    std::string buffer;
    // skip \" in formatString
    for (int i = 1, j = 0; i < formatString.length() - 1; i++) {
        if (formatString[i] == '%') {
            char format = formatString[++i];
            addStr(bBlocks, buffer);

            if (format == 'd' || format == 'c') {
                bBlocks.back()->addInst(Inst(format == 'd' ? IR::Op::PrintInt : IR::Op::PrintChar,
                                             nullptr,
                                             std::move(args[j++].value),
                                             nullptr));
            } else if (format == 's') {
                auto &arg = args[j++];
                bBlocks.back()->addInst(Inst(IR::Op::PrintStr,
                                             nullptr,
                                             std::move(arg.var),
                                             std::move(arg.offset)));
            }
        } else {
            buffer += formatString[i];
        }
    }
    addStr(bBlocks, buffer);
}

std::unique_ptr<LValStmt> LValStmt::parse() {
    std::unique_ptr<LValStmt> n;

    int row = Lexer::curRow;
    auto lVal = LVal::parse();

    auto sym = SymTab::find(lVal->getIdent());
    if (sym && sym->cons) {
        Error::raise('h', row);
    }

    singleLex(LexType::ASSIGN);

    if (Lexer::curLexType == LexType::GETINTTK) {
        n = GetIntStmt::parse();
        n->lVal = std::move(lVal);
        if (sym && ptrToValue(sym->type) != Type::Int) {
            Error::raise('e', row);
        }
    } else {
        n = AssignStmt::parse();
        n->lVal = std::move(lVal);
        if (auto assign = dynamic_cast<AssignStmt *>(n.get())) {
            auto remainingRank = [](const std::unique_ptr<Exp> &exp) -> size_t {
                auto expLVal = exp->getLVal();
                if (!expLVal) {
                    return 0;
                }
                auto expSym = SymTab::find(expLVal->getIdent());
                if (!expSym || expSym->symType == SymType::Func || expSym->dims.size() <= expLVal->getRank()) {
                    return 0;
                }
                return expSym->dims.size() - expLVal->getRank();
            };

            if (sym && (sym->dims.size() != n->lVal->dims.size()
                        || remainingRank(assign->exp) > 0
                        || ptrToValue(sym->type) != assign->exp->getType())) {
                Error::raise('e', row);
            }
        }
    }

    return n;
}

std::unique_ptr<GetIntStmt> GetIntStmt::parse() {
    auto n = std::make_unique<GetIntStmt>();

    int row = Lexer::curRow;

    singleLex(LexType::GETINTTK);
    singleLex(LexType::LPARENT);
    singleLex(LexType::RPARENT, row);
    singleLex(LexType::SEMICN, row); // ugly error handling!

    return n;
}

void GetIntStmt::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    bBlocks.back()->addInst(Inst(IR::Op::GetInt,
                                 nullptr,
                                 nullptr,
                                 nullptr));

    auto rValue = std::make_unique<Temp>(-static_cast<int>(MIPS::Register::v0), Type::Int);

    auto [symbol, depth] = SymTab::findInGen(lVal->ident);
    auto var = std::make_unique<IR::Var>(
            getStorageName(symbol, lVal->ident),
            getStorageDepth(symbol, depth),
            symbol->cons,
            symbol->dims,
            symbol->type,
            symbol->symType);

    if (lVal->dims.empty()) {
        bBlocks.back()->addInst(Inst(IR::Op::Store,
                                     std::move(rValue),
                                     std::move(var),
                                     nullptr));
    } else {
        int constOffset;
        std::unique_ptr<Temp> dynamicOffset;
        bool getNonConstIndex = lVal->getOffset(constOffset, dynamicOffset, bBlocks, symbol->dims, symbol->type);
        if (getNonConstIndex) {
            bBlocks.back()->addInst(Inst(IR::Op::StoreDynamic,
                                         std::move(rValue),
                                         std::move(var),
                                         std::move(dynamicOffset)));
        } else {
            bBlocks.back()->addInst(Inst(IR::Op::Store,
                                         std::move(rValue),
                                         std::move(var),
                                         std::make_unique<ConstVal>(constOffset, Type::Int)));
        }
    }
}

std::unique_ptr<AssignStmt> AssignStmt::parse() {
    auto n = std::make_unique<AssignStmt>();

    int row = Lexer::curRow;
    n->exp = Exp::parse(false);
    n->exp->getType();
    singleLex(LexType::SEMICN, row);

    return n;
}

void AssignStmt::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    auto rValue = exp->genIR(bBlocks);

    auto [symbol, depth] = SymTab::findInGen(lVal->ident);
    auto var = std::make_unique<IR::Var>(
            getStorageName(symbol, lVal->ident),
            getStorageDepth(symbol, depth),
            symbol->cons,
            symbol->dims,
            symbol->type,
            symbol->symType);

    if (lVal->dims.empty()) {
        bBlocks.back()->addInst(Inst(IR::Op::Store,
                                     std::move(rValue),
                                     std::move(var),
                                     nullptr));
    } else {
        int constOffset;
        std::unique_ptr<Temp> dynamicOffset;
        bool getNonConstIndex = lVal->getOffset(constOffset, dynamicOffset, bBlocks, symbol->dims, symbol->type);
        if (getNonConstIndex) {
            bBlocks.back()->addInst(Inst(IR::Op::StoreDynamic,
                                         std::move(rValue),
                                         std::move(var),
                                         std::move(dynamicOffset)));
        } else {
            bBlocks.back()->addInst(Inst(IR::Op::Store,
                                         std::move(rValue),
                                         std::move(var),
                                         std::make_unique<ConstVal>(constOffset, Type::Int)));
        }
    }
}

std::unique_ptr<ExpStmt> ExpStmt::parse() {
    auto n = std::make_unique<ExpStmt>();

    int row = Lexer::curRow;
    n->exp = Exp::parse(false);
    n->exp->getType();
    singleLex(LexType::SEMICN, row);

    return n;
}

void ExpStmt::genIR(IR::BasicBlocks &bBlocks) {
    using namespace IR;
    if (exp) {
        exp->genIR(bBlocks);
    }
}

std::unique_ptr<BlockStmt> BlockStmt::parse() {
    auto n = std::make_unique<BlockStmt>();

    SymTab::deepIn();
    n->block = Block::parse();

    SymTab::deepOut(); // BlockStmt

    return n;
}

void BlockStmt::genIR(IR::BasicBlocks &bBlocks) {
    SymTab::iterIn();
    bBlocks.back()->addInst(IR::Inst(
            IR::Op::InStack, nullptr, nullptr, nullptr));

    block->genIR(bBlocks);

    bBlocks.back()->addInst(IR::Inst(
            IR::Op::OutStack, nullptr, nullptr, nullptr));
    SymTab::iterOut();
}
