//
// Created by Steel_Shadow on 2023/10/12.
//
#ifndef COMPILER_STMT_H
#define COMPILER_STMT_H

#include "common/Type.h"

#include <memory>
#include <string>
#include <vector>


struct Cond;

struct LVal;

struct Exp;

struct Number;

/*-------------------------Block-----------------------------*/
// BlockItem → Decl | Stmt
struct BlockItem {
    virtual ~BlockItem() = default;

    static std::unique_ptr<BlockItem> parse();
};

// Stmt → LVal '=' Exp ';'
// | [Exp] ';'
// | Block
// | 'if' '(' Cond ')' Stmt [ 'else' Stmt ]
// | 'break' ';' | 'continue' ';'
// | 'for' '(' [ForStmt] ';' [Cond] ';' [ForStmt] ')' Stmt
// | 'return' [Exp] ';'
// | LVal '=' 'getint''('')'';'
// | 'printf''('FormatString{','Exp}')'';'
struct Stmt : public BlockItem {
    static std::unique_ptr<Stmt> parse();

    static bool retVoid; // check return in FuncDef
    static Type retType;
};

struct ControlFlow {
    static int loopDepth;
    static int switchDepth;
};

/*-----------------------------------------------------------*/

// LVal '=' 'getint''('')'';' | LVal '=' Exp ';'
struct LValStmt : public Stmt {
    std::unique_ptr<LVal> lVal;

    static std::unique_ptr<LValStmt> parse();
};

// LVal '=' Exp ';'
struct AssignStmt : public LValStmt {
    std::unique_ptr<Exp> exp;

    static std::unique_ptr<AssignStmt> parse();
};

// [Exp] ';'
struct ExpStmt : public Stmt {
    std::unique_ptr<Exp> exp;

    static std::unique_ptr<ExpStmt> parse();
};

// Block → '{' { BlockItem } '}'
struct Block {
    std::vector<std::unique_ptr<BlockItem>> blockItems;

    const std::vector<std::unique_ptr<BlockItem>> &getBlockItems() const;

    static std::unique_ptr<Block> parse();

    static int lastRow; // show return error message
};

// Block
struct BlockStmt : public Stmt {
    std::unique_ptr<Block> block;

    static std::unique_ptr<BlockStmt> parse();
};

// 'if' '(' Cond ')' Stmt [ 'else' Stmt ]
struct IfStmt : public Stmt {
    std::unique_ptr<Cond> cond;
    std::unique_ptr<Stmt> ifStmt;
    std::unique_ptr<Stmt> elseStmt;

    static std::unique_ptr<IfStmt> parse();
};

// 'break' ';'
struct BreakStmt : public Stmt {
    static std::unique_ptr<BreakStmt> parse();
};

// 'continue' ';'
struct ContinueStmt : public Stmt {
    static std::unique_ptr<ContinueStmt> parse();
};

// 'while' '(' Cond ')' Stmt
struct WhileStmt : public Stmt {
    std::unique_ptr<Cond> cond;
    std::unique_ptr<Stmt> stmt;

    static std::unique_ptr<WhileStmt> parse();
};

// CaseStmt → 'case' Number ':' { Stmt } | 'default' ':' { Stmt }
struct CaseStmt {
    bool isDefault{false};
    std::unique_ptr<Number> number;
    std::vector<std::unique_ptr<Stmt>> stmts;

    static std::unique_ptr<CaseStmt> parse();
};

// 'switch' '(' Exp ')' '{' { CaseStmt } '}'
struct SwitchStmt : public Stmt {
    std::unique_ptr<Exp> exp;
    std::vector<std::unique_ptr<CaseStmt>> cases;

    static std::unique_ptr<SwitchStmt> parse();
};

// ForStmt → LVal '=' Exp
struct ForStmt {
    std::unique_ptr<LVal> lVal;
    std::unique_ptr<Exp> exp;

    static std::unique_ptr<ForStmt> parse();
};

// 'for' '(' [ForStmt] ';' [Cond] ';' [ForStmt] ')' Stmt
struct BigForStmt : public Stmt {
    std::unique_ptr<ForStmt> init;
    std::unique_ptr<Cond> cond;
    std::unique_ptr<ForStmt> iter;
    std::unique_ptr<Stmt> stmt;

    // for error handling
    static int inForDepth;

    static std::unique_ptr<BigForStmt> parse();
};

// 'return' [Exp] ';'
struct ReturnStmt : public Stmt {
    std::unique_ptr<Exp> exp;

    static std::unique_ptr<ReturnStmt> parse();
};

// | LVal '=' 'getint''('')'';'
struct GetIntStmt : public LValStmt {
    static std::unique_ptr<GetIntStmt> parse();
};

// 'printf''('FormatString{','Exp}')'';'
struct PrintStmt : public Stmt {
    std::string formatString;
    std::vector<std::unique_ptr<Exp>> exps;

    int numOfFormat; // error handling
    std::vector<char> formatTypes;

    static std::unique_ptr<PrintStmt> parse();

private:
    void checkFormatString(const std::string &str);
};

#endif
