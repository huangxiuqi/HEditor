#include "core/doc/edit_mode.h"

namespace heditor {

const char* editModeName(EditMode m) {
    switch (m) {
        case EditMode::Document:
            return "文档模式";
        case EditMode::Form:
            return "表单模式";
        case EditMode::ReadOnly:
            return "只读模式";
    }
    return "文档模式";
}

EditMode editModeFromCode(int code) {
    switch (code) {
        case 1:
            return EditMode::Form;
        case 2:
            return EditMode::ReadOnly;
        default:
            return EditMode::Document;
    }
}

}  // namespace heditor
