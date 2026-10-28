#include "core/doc/field_spec.h"

#include <string>

namespace heditor {

const char* fieldKindName(FieldKind kind) {
    switch (kind) {
        case FieldKind::PageNumber:
            return "pageNumber";
        case FieldKind::PageCount:
            return "pageCount";
        case FieldKind::PrintDate:
            return "printDate";
        case FieldKind::DocProperty:
            return "docProperty";
    }
    return "（未知）";
}

bool fieldKindFromName(const std::string& name, FieldKind* out) {
    if (out == nullptr) return false;
    const FieldKind all[] = {FieldKind::PageNumber, FieldKind::PageCount, FieldKind::PrintDate,
                             FieldKind::DocProperty};
    for (FieldKind k : all) {
        if (name == fieldKindName(k)) {
            *out = k;
            return true;
        }
    }
    return false;
}

bool isLayoutResult(FieldKind kind) {
    return kind == FieldKind::PageNumber || kind == FieldKind::PageCount;
}

std::string fieldValue(const FieldSpec& field, const FieldContext& ctx) {
    switch (field.kind) {
        case FieldKind::PageNumber:
            return std::to_string(ctx.page_number);
        case FieldKind::PageCount:
            return std::to_string(ctx.page_count);
        case FieldKind::PrintDate:
            return ctx.print_date;
        case FieldKind::DocProperty: {
            const auto it = ctx.doc_properties.find(field.name);
            return it == ctx.doc_properties.end() ? std::string() : it->second;
        }
    }
    return std::string();
}

}  // namespace heditor
