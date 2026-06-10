#ifndef COMPILER_COMPILER_H
#define COMPILER_COMPILER_H

#include <string>

struct CompileOptions {
    std::string inputFile;
    std::string lexerOutputFile;
    std::string errorFile;
    std::string irFile;
    std::string mipsFile;
};

bool compile(const CompileOptions &options);

bool compile(const std::string &inFile,
             const std::string &outFile,
             const std::string &errorFile,
             const std::string &IRFile,
             const std::string &mipsFile);

#endif
