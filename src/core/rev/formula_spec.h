/**
 * 公式：表达式与排版后端标识，外加显示尺寸。
 *
 * 本期只画显式占位框，不做伪渲染（迁移路线阶段 4 的原话）。这条取舍是有意的：
 * 把表达式按普通文字画出来，看起来"有内容"，实际字距、上下标、分式全是错的 ——
 * 那是伪成功，比一个明确的框更危险。所以 FormulaBackend 本期只有 Placeholder 一档，
 * 引擎照尺寸画一个框并把表达式写进去，读者一眼看得出"这里是一枚公式，还没排版"。
 *
 * 显示尺寸留 Auto 表示"按表达式的长度估一个框"（估的是框，不是公式的排版结果）。
 *
 * 与 para / marks / control 一样挂在节点上、只对 Formula 有意义：编译要按流顺序取它。
 */
#pragma once

#include <cstdint>
#include <string>

#include "core/rev/length.h"

namespace heditor {
/**
 * 公式的排版后端。本期的唯一取值是 Placeholder：显式占位框，不做伪渲染。
 * 加一档就是加一个真正会排版公式的后端（例如 MathML 或自写的盒模型），
 * 那时引擎侧多一个绘制分支，模型与格式都不用动。
 */
enum class FormulaBackend : uint8_t {
    Placeholder = 0,
};

/**
 * 名字与枚举的互换。落盘只写名字，理由同 image_spec.h。
 * @param b 后端
 * @return 名字
 */
const char* formulaBackendName(FormulaBackend b);

/**
 * 名字转公式后端。
 * @param name 名字
 * @param out 写回后端
 * @return true = 名字认识
 */
bool formulaBackendFromName(const std::string& name, FormulaBackend* out);

struct FormulaSpec {
    /**
     * 表达式。本期不解释它，只原样存住并写进占位框 —— 解释它需要的是排版后端，
     * 不是模型。
     */
    std::string expr;

    /**
     * 用哪个排版后端（本期只有占位）。
     */
    FormulaBackend backend = FormulaBackend::Placeholder;

    /**
     * 显示尺寸。Auto = 按表达式估一个框（见文件头）。
     */
    Length width;

    /**
     * 显示高度。
     */
    Length height;

    bool operator==(const FormulaSpec&) const = default;
};

}  // namespace heditor
