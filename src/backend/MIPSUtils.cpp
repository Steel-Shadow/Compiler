#include "backend/MIPSInternal.h"

#include <cctype>

namespace MIPS::detail {

int alignTo(int value, int align) {
    return ((value + align - 1) / align) * align;
}

int sizeOfIRType(const std::string &type) {
    return type == "i8" ? 1 : 4;
}

std::string sanitizeLabel(std::string label) {
    for (char &ch: label) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            ch = '_';
        }
    }
    return label;
}

std::string stripPrefix(std::string text) {
    if (!text.empty() && (text.front() == '%' || text.front() == '@')) {
        text.erase(text.begin());
    }
    return text;
}

bool isInteger(const std::string &text) {
    if (text.empty()) {
        return false;
    }
    size_t pos = text[0] == '-' ? 1 : 0;
    if (pos == text.size()) {
        return false;
    }
    for (; pos < text.size(); ++pos) {
        if (!std::isdigit(static_cast<unsigned char>(text[pos]))) {
            return false;
        }
    }
    return true;
}

std::string edgeKey(const std::string &pred, const std::string &target) {
    return pred + "\n" + target;
}

} // namespace MIPS::detail
