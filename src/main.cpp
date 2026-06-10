#include "AST/CompUnit.h"
#include "backend/MIPS.h"
#include "errorHandler/Error.h"
#include "frontend/lexer/Lexer.h"
#include "ir/IRGenerator.h"

#include <fstream>
#include <string>

namespace {
void createEmptyOutputFile(const std::string &path) {
    if (!path.empty()) {
        std::ofstream output(path);
    }
}

void writeTextFile(const std::string &path, const std::string &text) {
    if (!path.empty()) {
        std::ofstream output(path);
        output << text;
    }
}
} // namespace

void compile(const std::string &inFile,
             const std::string &outFile,
             const std::string &errorFile,
             const std::string &IRFile,
             const std::string &mipsFile) {
    Lexer::init(inFile, outFile);
    Error::errorFileStream = std::ofstream(errorFile);
    createEmptyOutputFile(IRFile);
    createEmptyOutputFile(mipsFile);

    auto compUnit = CompUnit::parse();
    if (!Error::hasError) {
        IR::Module module = IR::generateModule(*compUnit);
        writeTextFile(IRFile, module.toString());
        writeTextFile(mipsFile, MIPS::generate(module));
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
