#include "AST/CompUnit.h"
#include "backend/MIPS.h"
#include "errorHandler/Error.h"

void compile(const std::string &inFile,
             const std::string &outFile,
             const std::string &errorFile,
             const std::string &IRFile,
             const std::string &mipsFile) {
    Lexer::init(inFile, outFile);
    Error::errorFileStream = std::ofstream(errorFile);
    IR::IRFileStream = std::ofstream(IRFile);
    MIPS::mipsFileStream = std::ofstream(mipsFile);

    auto compUnit = CompUnit::parse();
    if (!Error::hasError) {
        auto module = compUnit->genIR();
        module->outputIR();
        MIPS::genMIPS(*module);
    }
}

int main(int argc, char *argv[]) {
    if (argc == 6) {
        compile(argv[1], argv[2], argv[3], argv[4], argv[5]);
    } else {
        compile("testfile.txt", "", "error.txt", "ir.txt", "mips.txt");
    }
    return 0;
}
