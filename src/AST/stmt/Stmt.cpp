//
// Created by Steel_Shadow on 2023/10/12.
//

#include "Stmt.h"

#include "AST/decl/Decl.h"
#include "AST/expr/Exp.h"
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

bool Stmt::retVoid;
Type Stmt::retType = Type::Void;

int ControlFlow::loopDepth = 0;
int ControlFlow::switchDepth = 0;

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

std::unique_ptr<ForStmt> ForStmt::parse() {
    auto n = std::make_unique<ForStmt>();

    n->lVal = LVal::parse();
    singleLex(LexType::ASSIGN);
    n->exp = Exp::parse(false);

    output(AST::ForStmt);
    return n;
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

std::unique_ptr<ContinueStmt> ContinueStmt::parse() {
    int row = Lexer::curRow;

    if (ControlFlow::loopDepth == 0) {
        Error::raise('m', row);
    }

    Lexer::next();
    singleLex(LexType::SEMICN, row);
    return std::make_unique<ContinueStmt>();
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

void PrintStmt::checkFormatString(const std::string &str) {
    // skip begin and end " "
    for (size_t i = 1; i + 1 < str.length(); ++i) {
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
        } else if (!(c == 32 || c == 33 || (c >= 40 && c <= 126))) {
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
    for (size_t i = 0; i < n->exps.size() && i < n->formatTypes.size(); ++i) {
        Type expType = ptrToValue(n->exps[i]->getType());
        if (((n->formatTypes[i] == 'd' || n->formatTypes[i] == 'c') && n->exps[i]->getRemainingRank() > 0)
            || (n->formatTypes[i] == 'd' && expType != Type::Int)
            || (n->formatTypes[i] == 'c' && expType != Type::Char)) {
            Error::raise('e', row);
        } else if (n->formatTypes[i] == 's') {
            auto lVal = n->exps[i]->getLVal();
            auto ident = n->exps[i]->getIdent();
            auto sym = ident.empty() ? nullptr : SymTab::find(ident);
            auto *object = sym ? sym->asObject() : nullptr;
            if (lVal == nullptr || object == nullptr || ptrToValue(object->getType()) != Type::Char || object->getDims().size() <= lVal->getRank()) {
                Error::raise('e', row);
            }
        }
    }

    singleLex(LexType::RPARENT, row);
    singleLex(LexType::SEMICN, row);
    return n;
}

std::unique_ptr<LValStmt> LValStmt::parse() {
    std::unique_ptr<LValStmt> n;

    int row = Lexer::curRow;
    auto lVal = LVal::parse();

    auto sym = SymTab::find(lVal->getIdent());
    auto *object = sym ? sym->asObject() : nullptr;
    auto *value = sym ? sym->asValue() : nullptr;
    if (sym && !object) {
        Error::raise('e', row);
    }
    if (value && value->isConst()) {
        Error::raise('h', row);
    }

    singleLex(LexType::ASSIGN);

    if (Lexer::curLexType == LexType::GETINTTK) {
        n = GetIntStmt::parse();
        n->lVal = std::move(lVal);
        if (object && ptrToValue(object->getType()) != Type::Int) {
            Error::raise('e', row);
        }
    } else {
        n = AssignStmt::parse();
        n->lVal = std::move(lVal);
        if (auto assign = dynamic_cast<AssignStmt *>(n.get())) {
            if (object && (object->getDims().size() != n->lVal->dims.size() || assign->exp->getRemainingRank() > 0 || ptrToValue(object->getType()) != assign->exp->getType())) {
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

std::unique_ptr<AssignStmt> AssignStmt::parse() {
    auto n = std::make_unique<AssignStmt>();

    int row = Lexer::curRow;
    n->exp = Exp::parse(false);
    n->exp->getType();
    singleLex(LexType::SEMICN, row);

    return n;
}

std::unique_ptr<ExpStmt> ExpStmt::parse() {
    auto n = std::make_unique<ExpStmt>();

    int row = Lexer::curRow;
    n->exp = Exp::parse(false);
    n->exp->getType();
    singleLex(LexType::SEMICN, row);

    return n;
}

std::unique_ptr<BlockStmt> BlockStmt::parse() {
    auto n = std::make_unique<BlockStmt>();

    SymTab::deepIn();
    n->block = Block::parse();

    SymTab::deepOut(); // BlockStmt

    return n;
}
