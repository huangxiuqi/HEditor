/**
 * 扁平片段值：从文档里取出的内容，只装文本与段落标记。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/para_format.h"

namespace heditor {
/**
 * 片段里的一段。
 */
enum class PieceKind : uint8_t {
    Text,      // 带标记的文本
    ParaMark,  // 段落标记，段落格式挂在它身上
};

struct Piece {
    /**
     * 这一片是文本还是段落标记。
     */
    PieceKind kind = PieceKind::Text;

    /**
     * 码点（仅文本片）。
     */
    std::vector<uint32_t> cps;

    /**
     * 字符格式（仅文本片）。
     */
    CharFormat marks;

    /**
     * 段落格式（仅段落标记片）。
     */
    ParaFormatPatch para;

    /**
     * 样式引用：文本片段里是字符样式，段落标记片段里是段落样式（第六节）。
     * 复制粘贴与分段的往返都要带上它，否则"复制一段带样式的段落"粘回来样式就没了。
     */
    std::string styleRef;

    /**
     * 这一段占几个流位置：文本按码点数，段落标记占一个。
     * @return 占几个流位置（文本按码点数，段落标记占一个）
     */
    int span() const;
};

struct Fragment {
    /**
     * 片段内容，按流顺序。
     */
    std::vector<Piece> pieces;

    /**
     * 片段一共占几个流位置。
     * @returns 流位置个数
     */
    int span() const;

    /**
     * 片段是空的。
     * @returns true = 没有内容
     */
    bool empty() const { return pieces.empty(); }
};

}  // namespace heditor