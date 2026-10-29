/**
 * 编辑会话：光标解析、命令层与光标可见性。
 *
 * 编辑侧的所有权在这一层：它只持状态，具体命令与解析仍在 CaretResolver / EditCommand
 * 里，引擎通过 edit_session_ 访问。
 */
#pragma once

#include "core/engine/caret.h"
#include "core/engine/edit_command.h"

namespace heditor {

/**
 * 编辑会话：光标、命令层与光标可见性的所有者。
 */
class EditSession {
public:

    /**
     * 光标与选区解析。
     */
    CaretResolver caret;

    /**
     * 编辑命令层。
     */
    EditCommand edit;

    /**
     * 光标是否可见。
     */
    bool caret_visible = true;
};

}  // namespace heditor
