#include "Compiler.h"

int main(int argc, char *argv[]) {
    if (argc == 6) {
        compile({argv[1], argv[2], argv[3], argv[4], argv[5]});
    } else {
        compile({"testfile.txt", "", "error.txt", "ir.txt", "mips.txt"});
    }
    return 0;
}
