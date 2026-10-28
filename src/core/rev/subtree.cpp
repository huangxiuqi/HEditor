#include "core/rev/subtree.h"

namespace heditor {

int NodeSpec::span() const {
    if (isText(kind)) return static_cast<int>(text.size());
    if (!isContainer(kind)) return 1;
    int total = (kind == NodeKind::Document) ? 0 : 2;
    for (const NodeSpec& c : children) total += c.span();
    return total;
}

}  // namespace heditor
