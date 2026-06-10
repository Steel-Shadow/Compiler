#include "backend/MIPS.h"

#include <sstream>

namespace MIPS {

void Generator::emitModule(const IR::Module &module, std::ostream &out) {
    out << "# MIPS generated from toy LLVM-like IR\n";
    out << ".data\n";
    for (const auto &global: module.globals) {
        out << global.name << ": ";
        if (global.elementType == Type::Char) {
            out << ".byte ";
        } else {
            out << ".word ";
        }
        if (global.init.empty()) {
            out << "0";
        } else {
            for (size_t i = 0; i < global.init.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << global.init[i];
            }
        }
        out << "\n";
    }
    out << "\n.text\n";
    emitRuntimeStubs(out);
    for (const auto &function: module.functions) {
        emitFunctionSkeleton(*function, out);
    }
}

void Generator::emitRuntimeStubs(std::ostream &out) {
    out << "# Runtime builtins are lowered by later backend passes.\n";
}

void Generator::emitFunctionSkeleton(const IR::Function &function, std::ostream &out) {
    out << "\n" << function.name << ":\n";
    out << "  # TODO: lower IR instructions for " << function.name << "\n";
    if (function.name == "main") {
        out << "  li $v0, 10\n";
        out << "  syscall\n";
    } else {
        out << "  jr $ra\n";
    }
}

std::string generate(const IR::Module &module) {
    std::ostringstream out;
    Generator generator;
    generator.emitModule(module, out);
    return out.str();
}

} // namespace MIPS
