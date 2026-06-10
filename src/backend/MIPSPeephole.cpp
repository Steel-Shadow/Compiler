#include "backend/MIPSInternal.h"

#include <sstream>

namespace MIPS::detail {
namespace {

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

bool startsWith(const std::string &text, const std::string &prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool parseMemInst(const std::string &line, const std::string &op, std::string &reg, std::string &addr) {
    std::string prefix = "  " + op + " ";
    if (!startsWith(line, prefix)) {
        return false;
    }
    size_t comma = line.find(", ", prefix.size());
    if (comma == std::string::npos) {
        return false;
    }
    reg = line.substr(prefix.size(), comma - prefix.size());
    addr = line.substr(comma + 2);
    return true;
}

} // namespace

std::string optimizeAssembly(const std::string &assembly) {
    auto lines = splitLines(assembly);
    std::vector<std::string> optimized;
    optimized.reserve(lines.size());

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        if (startsWith(line, "  li ") && line.size() > 6 && line.rfind(", 0") == line.size() - 3) {
            std::string reg = line.substr(5, line.size() - 8);
            line = "  move " + reg + ", $zero";
        }

        if (startsWith(line, "  move ")) {
            size_t comma = line.find(", ", 7);
            if (comma != std::string::npos) {
                std::string dst = line.substr(7, comma - 7);
                std::string src = line.substr(comma + 2);
                if (dst == src) {
                    continue;
                }
            }
        }

        if (startsWith(line, "  j ") && i + 1 < lines.size()) {
            std::string target = line.substr(4);
            if (lines[i + 1] == target + ":") {
                continue;
            }
        }

        if (!optimized.empty()) {
            std::string swReg;
            std::string swAddr;
            std::string lwReg;
            std::string lwAddr;
            if (parseMemInst(optimized.back(), "sw", swReg, swAddr) && parseMemInst(line, "lw", lwReg, lwAddr) && swAddr == lwAddr) {
                if (swReg == lwReg) {
                    continue;
                }
                line = "  move " + lwReg + ", " + swReg;
            }
        }

        optimized.push_back(std::move(line));
    }

    std::ostringstream out;
    for (const auto &line: optimized) {
        out << line << "\n";
    }
    return out.str();
}

} // namespace MIPS::detail
