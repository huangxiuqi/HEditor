#include "core/doc/fragment_value.h"

namespace heditor {

int Piece::span() const { return kind == PieceKind::ParaMark ? 1 : static_cast<int>(cps.size()); }

int Fragment::span() const {
    int total = 0;
    for (const Piece& p : pieces) total += p.span();
    return total;
}

}  // namespace heditor
