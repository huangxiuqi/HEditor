#include "core/doc/char_format.h"

namespace heditor {

CharFormat mergeCharFormat(const CharFormat& under, const CharFormat& over) {
    CharFormat out = under;
    if (over.bold.has_value()) out.bold = over.bold;
    if (over.italic.has_value()) out.italic = over.italic;
    if (over.underline.has_value()) out.underline = over.underline;
    if (over.font_index.has_value()) out.font_index = over.font_index;
    if (over.size.has_value()) out.size = over.size;
    if (over.color.has_value()) out.color = over.color;
    return out;
}

}  // namespace heditor
