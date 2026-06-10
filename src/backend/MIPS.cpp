#include "backend/MIPSInternal.h"

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
    out << "  j main\n";
    for (const auto &function: module.functions) {
        emitFunction(*function, out);
    }
}

void Generator::emitRuntimeStubs(std::ostream &out) {
    out << "# Builtins are emitted inline as syscalls.\n";
}

void Generator::emitFunction(const IR::Function &function, std::ostream &out) {
    detail::FunctionEmitter(function, out).emit();
}

std::string generate(const IR::Module &module) {
    std::ostringstream out;
    Generator generator;
    generator.emitModule(module, out);
    return detail::optimizeAssembly(out.str());
}

} // namespace MIPS
