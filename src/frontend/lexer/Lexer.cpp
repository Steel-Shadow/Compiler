//
// Created by Steel_Shadow on 2023/9/21.
//

#include "Lexer.h"

#include "config.h"
#include "errorHandler/Error.h"
#include "tools/LinkedHashMap.h"

#include <sstream>
#include <utility>

std::ofstream Lexer::outFileStream;
std::string Lexer::fileContents;

Word words[Lexer::deep];
LexType &Lexer::curLexType = words[0].first;
Token &Lexer::curToken = words[0].second;

size_t Lexer::pos[deep]; // count from 1
int Lexer::column[deep]; // count from 1
int Lexer::row[deep]; // count from 1.
// ReSharper disable once CppRedundantQualifier
int &Lexer::curRow = Lexer::row[0];
int Lexer::lastRow = 1;

char c; // c = fileContents[posTemp - 1]
size_t posTemp;
int columnTemp;
int rowTemp{1};

// synchronize output of parser and lexer
bool firstOutput = true;
LexType lastLexType;
Token lastToken;

LinkedHashMap<std::string, LexType> reserveWords;

Word Lexer::peek(int n) {
    return words[n];
}

void advanceColumn(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        ++columnTemp;
    }
}

// also return EOF
char nextChar() {
    if (posTemp >= Lexer::fileContents.size()) {
        c = EOF;
        return EOF;
    }

    if (c == '\n') {
        rowTemp++;
        columnTemp = 1;
    } else {
        columnTemp++;
    }

    c = Lexer::fileContents[posTemp++];

    return c;
}

void reserve(const Token &t, LexType &l) {
    if (reserveWords.containsKey(t)) {
        l = reserveWords.get(t);
    } else {
        l = LexType::IDENFR;
    }
}

void output();

void updateWords(LexType l, Token t);

// stop if you read LexType::Lex_END
// or the file will be read in loop
Word Lexer::next() {
    // Ident
    // IntConst
    // FormatString

    if (posTemp > fileContents.length()) {
        updateWords(LexType::LEX_END, "");
        output();
        return words[0];
    }

    LexType lexType = LexType::LEX_EMPTY;
    Token token = std::string(1, c);

    if (c >= '0' && c <= '9') {
        while (isdigit(nextChar())) {
            token += c;
        }
        // error: bad number
        lexType = LexType::INTCON;
    } else if (c == '\'') {
        while (nextChar() != EOF) {
            token += c;
            if (c == '\\') {
                nextChar();
                token += c;
            } else if (c == '\'') {
                break;
            }
        }

        lexType = LexType::CHARCON;
        nextChar();
    } else if (c == '_' || isalpha(c)) {
        while (nextChar(), c == '_' || isalpha(c) || isdigit(c)) {
            token += c;
        }
        reserve(token, lexType);
    } else if (c == '/') {
        // q1
        nextChar();
        if (c == '/') {
            // q2
            while (nextChar() != '\n') {}
            // line comment //
            token = "";
            lexType = LexType::COMMENT;
        } else if (c == '*') {
            // q5
        q5:
            while (nextChar() != '*') {}
            do {
                if (c == '*') {
                } else if (c == '/') {
                    break; // q7
                } else {
                    goto q5;
                }
            } while (nextChar());
            // block comment /**/
            token = "";
            lexType = LexType::COMMENT;
            nextChar();
        } else {
            token = "/"; // q4
            lexType = LexType::DIV;
        }
    } else if (c == '\"') {
        // STRCON
        while (nextChar() != EOF) {
            token += c;
            if (c == '\"') {
                break;
            } // error: bad char in format string
        }

        lexType = LexType::STRCON;
        nextChar();
    } else {
        // special operator +-*/ && &
        for (const auto &[str, type]: reserveWords) {
            if (fileContents.substr(posTemp - 1, str.length()) == str) {
                posTemp += str.length();
                advanceColumn(str.length());
                c = fileContents[posTemp - 1];

                token = str;
                lexType = type;
                break;
            }
        }
        if (lexType == LexType::LEX_EMPTY) {
            if (c == '&' || c == '|') {
                Error::raise('a');
                lexType = c == '&' ? LexType::AND : LexType::OR;
            } else {
                lexType = LexType::LEX_EMPTY;
            }
            token = std::string(1, c);
            nextChar();
        }
    }

    if (lexType == LexType::COMMENT) {
        while (isspace(c)) {
            nextChar();
        }
        next();
    } else {
        updateWords(lexType, token);
        output();
        while (isspace(c)) {
            nextChar();
        }
    }

    return words[0];
}

void buildReserveWords() {
    // IDENFR
    // INTCON
    // STRCON
    reserveWords.put("main", LexType::MAINTK);
    reserveWords.put("const", LexType::CONSTTK);
    reserveWords.put("int", LexType::INTTK);
    reserveWords.put("char", LexType::CHARTK);
    reserveWords.put("static", LexType::STATICTK);
    reserveWords.put("break", LexType::BREAKTK);
    reserveWords.put("continue", LexType::CONTINUETK);
    reserveWords.put("if", LexType::IFTK);
    reserveWords.put("else", LexType::ELSETK);
    reserveWords.put("while", LexType::WHILETK);
    reserveWords.put("switch", LexType::SWITCHTK);
    reserveWords.put("case", LexType::CASETK);
    reserveWords.put("default", LexType::DEFAULTTK);
    reserveWords.put("&&", LexType::AND);
    reserveWords.put("||", LexType::OR);
    reserveWords.put("for", LexType::FORTK);
    reserveWords.put("getint", LexType::GETINTTK);
    reserveWords.put("printf", LexType::PRINTFTK);
    reserveWords.put("return", LexType::RETURNTK);
    reserveWords.put("+", LexType::PLUS);
    reserveWords.put("-", LexType::MINU);
    reserveWords.put("void", LexType::VOIDTK);
    reserveWords.put("*", LexType::MULT);
    reserveWords.put("/", LexType::DIV);
    reserveWords.put("%", LexType::MOD);
    reserveWords.put("<=", LexType::LEQ);
    reserveWords.put("<", LexType::LSS);
    reserveWords.put(">=", LexType::GEQ);
    reserveWords.put(">", LexType::GRE);
    reserveWords.put("==", LexType::EQL);
    reserveWords.put("!=", LexType::NEQ);
    reserveWords.put("!", LexType::NOT);
    reserveWords.put("=", LexType::ASSIGN);
    reserveWords.put(";", LexType::SEMICN);
    reserveWords.put(",", LexType::COMMA);
    reserveWords.put(":", LexType::COLON);
    reserveWords.put("(", LexType::LPARENT);
    reserveWords.put(")", LexType::RPARENT);
    reserveWords.put("[", LexType::LBRACK);
    reserveWords.put("]", LexType::RBRACK);
    reserveWords.put("{", LexType::LBRACE);
    reserveWords.put("}", LexType::RBRACE);
}

void output() {
    if (firstOutput) {
        firstOutput = false;
        lastLexType = Lexer::curLexType;
        lastToken = Lexer::curToken;
    } else {
        if (!(lastLexType == LexType::LEX_EMPTY || lastLexType == LexType::LEX_END)) {
#ifdef STDOUT_LEXER
            std::cout << toString(lastLexType) << " " << lastToken
                      << '\n';
#endif
#ifdef FILEOUT_LEXER
            Lexer::outFileStream << toString(lastLexType) << " " << lastToken
                                 << '\n';
#endif
        }

        lastLexType = Lexer::curLexType;
        lastToken = Lexer::curToken;
    }
}

void updateWords(LexType l, Token t) {
    using namespace Lexer;
    if (words[0].first != LexType::LEX_EMPTY && words[0].first != LexType::LEX_END) {
        lastRow = row[0];
    }
    for (size_t i = 0; i < deep - 1; ++i) {
        words[i] = words[i + 1];
        pos[i] = pos[i + 1];
        column[i] = column[i + 1];
        row[i] = row[i + 1];
    }

    pos[deep - 1] = posTemp;
    column[deep - 1] = columnTemp;
    row[deep - 1] = rowTemp;

    words[deep - 1].first = l;
    words[deep - 1].second = std::move(t);
}

// distinguish between Exp and LVal in Stmt
// It's wrong if Cond is a kind of Exp, but our work doesn't require it.
bool Lexer::findAssignBeforeSemicolon() {
    bool inChar = false;
    bool inString = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    bool escaped = false;
    for (size_t t = pos[0] == 0 ? 0 : pos[0] - 1; t < fileContents.size(); ++t) {
        const char current = fileContents[t];
        const char next = t + 1 < fileContents.size() ? fileContents[t + 1] : '\0';

        if (current == '\n') {
            return false;
        }

        if (inLineComment) {
            continue;
        }
        if (inBlockComment) {
            if (current == '*' && next == '/') {
                inBlockComment = false;
                ++t;
            }
            continue;
        }
        if (inChar || inString) {
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if ((inChar && current == '\'') || (inString && current == '"')) {
                inChar = false;
                inString = false;
            }
            continue;
        }

        if (current == '/' && next == '/') {
            inLineComment = true;
            ++t;
        } else if (current == '/' && next == '*') {
            inBlockComment = true;
            ++t;
        } else if (current == '\'') {
            inChar = true;
        } else if (current == '"') {
            inString = true;
        } else if (current == ';') {
            return false;
        } else if (current == '=' && next != '='
                   && (t == 0 || (fileContents[t - 1] != '=' && fileContents[t - 1] != '!' && fileContents[t - 1] != '<' && fileContents[t - 1] != '>'))) {
            return true;
        }
    }
    return false;
}

void resetLexerState() {
    for (size_t i = 0; i < Lexer::deep; ++i) {
        words[i] = {LexType::LEX_EMPTY, ""};
        Lexer::pos[i] = 0;
        Lexer::column[i] = 0;
        Lexer::row[i] = 1;
    }

    c = '\0';
    posTemp = 0;
    columnTemp = 0;
    rowTemp = 1;
    Lexer::lastRow = 1;
    firstOutput = true;
    lastLexType = LexType::LEX_EMPTY;
    lastToken.clear();
}

void Lexer::init(const std::string &inFile, [[maybe_unused]] const std::string &outFile) {
    resetLexerState();
    buildReserveWords();

    auto inFileStream = std::ifstream(inFile);
    if (!inFileStream) {
        throw std::runtime_error("Reading " + inFile + " fails!");
    }

#if defined(FILEOUT_LEXER) || defined(FILEOUT_PARSER)
    outFileStream = std::ofstream(outFile);
    if (!outFileStream && !outFile.empty()) {
        throw std::runtime_error("Writing " + outFile + " fails!");
    }
#endif


    std::stringstream buffer;
    buffer << inFileStream.rdbuf();
    fileContents = buffer.str();

    while (isspace(nextChar())) {}
    for (size_t i = 0; i < deep; ++i) {
        next();
    }
}
