#ifndef COMPILER_MIPS_H
#define COMPILER_MIPS_H

#include "ir/IR.h"

#include <ostream>
#include <string>

namespace MIPS {

class Generator {
public:
    void emitModule(const IR::Module &module, std::ostream &out);

private:
    void emitRuntimeStubs(std::ostream &out);
    void emitFunction(const IR::Function &function, std::ostream &out);
};

std::string generate(const IR::Module &module);

} // namespace MIPS

#endif
