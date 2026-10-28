#include "core/doc/table_spec.h"

#include <string>

namespace heditor {

const char* heightRuleName(HeightRule rule) {
    switch (rule) {
        case HeightRule::Auto:
            return "auto";
        case HeightRule::AtLeast:
            return "atLeast";
        case HeightRule::Exact:
            return "exact";
    }
    return "（未知）";
}

bool heightRuleFromName(const std::string& name, HeightRule* out) {
    if (out == nullptr) return false;
    const HeightRule all[] = {HeightRule::Auto, HeightRule::AtLeast, HeightRule::Exact};
    for (HeightRule r : all) {
        if (name == heightRuleName(r)) {
            *out = r;
            return true;
        }
    }
    return false;
}

const char* cellVAlignName(CellVAlign align) {
    switch (align) {
        case CellVAlign::Top:
            return "top";
        case CellVAlign::Center:
            return "center";
        case CellVAlign::Bottom:
            return "bottom";
    }
    return "（未知）";
}

bool cellVAlignFromName(const std::string& name, CellVAlign* out) {
    if (out == nullptr) return false;
    const CellVAlign all[] = {CellVAlign::Top, CellVAlign::Center, CellVAlign::Bottom};
    for (CellVAlign a : all) {
        if (name == cellVAlignName(a)) {
            *out = a;
            return true;
        }
    }
    return false;
}

}  // namespace heditor
