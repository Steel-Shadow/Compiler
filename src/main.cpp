#include "AST/CompUnit.h"
#include "errorHandler/Error.h"
#include "frontend/lexer/Lexer.h"

#include <fstream>
#include <string>

namespace {
void createEmptyOutputFile(const std::string &path) {
    if (!path.empty()) {
        std::ofstream output(path);
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

    (void) CompUnit::parse();
}

int main(int argc, char *argv[]) {
    if (argc == 6) {
        compile(argv[1], argv[2], argv[3], argv[4], argv[5]);
    } else {
        compile("testfile.txt", "", "error.txt", "ir.txt", "mips.txt");
    }
    return 0;
}
