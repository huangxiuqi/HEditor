/**
 * 公式记录的名字互换（声明与理由见 formula_spec.h）。
 */
#include "core/rev/formula_spec.h"

namespace heditor {

const char* formulaBackendName(FormulaBackend b) {
    switch (b) {
        case FormulaBackend::Placeholder:
            return "placeholder";
    }
    return "placeholder";
}

bool formulaBackendFromName(const std::string& name, FormulaBackend* out) {
    if (out == nullptr) return false;
    if (name == "placeholder") {
        *out = FormulaBackend::Placeholder;
        return true;
    }
    return false;
}

}  // namespace heditor
