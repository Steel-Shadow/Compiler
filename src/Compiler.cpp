#include "Compiler.h"

#include "AST/ASTState.h"
#include "AST/CompUnit.h"
#include "backend/MIPS.h"
#include "errorHandler/Error.h"
#include "frontend/lexer/Lexer.h"
#include "frontend/symTab/SymTab.h"
#include "middle/IR.h"

#include <fstream>

namespace {
void prepareCompileState(const std::string &errorFile,
                         const std::string &IRFile,
                         const std::string &mipsFile) {
    Error::reset();
    ASTState::reset();
    SymTab::reset();
    IR::reset();
    Error::errorFileStream = std::ofstream(errorFile);
    IR::IRFileStream = std::ofstream(IRFile);
    MIPS::mipsFileStream = std::ofstream(mipsFile);
}
} // namespace

bool compile(const CompileOptions &options) {
    prepareCompileState(options.errorFile, options.irFile, options.mipsFile);
    Lexer::init(options.inputFile, options.lexerOutputFile);

    auto compUnit = CompUnit::parse();
    if (!Error::hasError) {
        auto module = compUnit->genIR();
        module->outputIR();
        MIPS::genMIPS(*module);
    }
    return !Error::hasError;
}

bool compile(const std::string &inFile,
             const std::string &outFile,
             const std::string &errorFile,
             const std::string &IRFile,
             const std::string &mipsFile) {
    return compile({inFile, outFile, errorFile, IRFile, mipsFile});
}
