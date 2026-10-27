// 由 scripts/gen_payloads.py 生成，勿手改。
//
// 来源：docs/api/payloads.json（C++ 写出代码的投影）。
// 用途：pk::checked 拿它比对运行时写出的顶层键 —— 见 src/core/engine/payload_schema.h。
#pragma once

#include <iterator>
#include <string_view>

namespace pk {

/**
 * 一个键：名字 + 粗种类（种类只给 TS 用，C++ 侧只比名字）+ 是否可能缺席。
 */
struct KeySpec {
    std::string_view name;
    std::string_view kind;
    bool optional;
};

/**
 * 一份载荷：名字 + 按写出顺序排列的键 + 是否允许缺键（多出口）。
 */
struct PayloadSpec {
    std::string_view name;
    const KeySpec* keys;
    int count;
    bool partial;
};

namespace caret_style {
inline constexpr KeySpec kKeys[] = {
    {"color", "arr", false},
    {"widthPt", "num", false},
};
inline constexpr PayloadSpec kSpec{"caretStyle", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace caret_style

namespace selection_style {
inline constexpr KeySpec kKeys[] = {
    {"color", "arr", false},
};
inline constexpr PayloadSpec kSpec{"selectionStyle", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace selection_style

namespace stats {
inline constexpr KeySpec kKeys[] = {
    {"pageCount", "num", false},
    {"glyphCount", "num", false},
    {"placedLines", "num", false},
    {"clusters", "num", false},
    {"faces", "num", false},
    {"fontInstances", "num", false},
    {"fontRunCount", "num", false},
    {"missingGlyphCount", "num", false},
    {"fontCacheHits", "num", false},
    {"fontCacheMisses", "num", false},
    {"fontStorageReads", "num", false},
    {"fontStorageBytes", "num", false},
    {"fontIoMs", "num", false},
    {"fontResidentBytes", "num", false},
    {"fontStorageFailures", "num", false},
    {"fontStorageEofs", "num", false},
    {"shapeMs", "num", false},
    {"layoutMs", "num", false},
    {"pageFpMs", "num", false},
    {"paintMs", "num", false},
    {"paintFillMs", "num", false},
    {"paintGlyphMs", "num", false},
    {"paintGlyphBoundsMs", "num", false},
    {"paintedGlyphs", "num", false},
    {"paintPresentMs", "num", false},
    {"paintPreMs", "num", false},
    {"paintSwapMs", "num", false},
    {"paintTransientMs", "num", false},
    {"paintOverlayMs", "num", false},
    {"paintLedgerMs", "num", false},
    {"paintReportMs", "num", false},
    {"totalMs", "num", false},
    {"frameWidth", "num", false},
    {"frameHeight", "num", false},
    {"frameBytes", "num", false},
    {"glyphCacheBytes", "num", false},
    {"glyphCacheLimit", "num", false},
    {"zoom", "num", false},
};
inline constexpr PayloadSpec kSpec{"stats", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace stats

namespace bench_relayout_paint {
inline constexpr KeySpec kKeys[] = {
    {"runs", "num", false},
    {"shapeMs", "num", false},
    {"layoutMs", "num", false},
    {"paintMs", "num", false},
    {"totalMs", "num", false},
};
inline constexpr PayloadSpec kSpec{"benchRelayoutPaint", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace bench_relayout_paint

namespace flow_text_pos {
inline constexpr KeySpec kKeys[] = {
    {"para", "num", false},
    {"offset", "num", false},
};
inline constexpr PayloadSpec kSpec{"flowTextPos", kKeys, static_cast<int>(std::size(kKeys)), true};
}  // namespace flow_text_pos

namespace selection_report {
inline constexpr KeySpec kKeys[] = {
    {"anchor", "obj", false},
    {"focus", "obj", false},
    {"empty", "bool", false},
    {"rects", "arr", false},
};
inline constexpr PayloadSpec kSpec{"selectionReport", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace selection_report

namespace hit_test {
inline constexpr KeySpec kKeys[] = {
    {"para", "num", false},
    {"offset", "num", false},
    {"field", "num", false},
    {"side", "num", false},
    {"line", "num", false},
    {"opt", "num", false},
    {"cellPara", "num", true},
    {"cell", "arr", true},
    {"img", "num", true},
    {"imgHandle", "num", true},
    {"drag", "num", true},
    {"dragImage", "num", true},
    {"gridTable", "num", true},
    {"gridCol", "num", true},
    {"gridRow", "num", true},
};
inline constexpr PayloadSpec kSpec{"hitTest", kKeys, static_cast<int>(std::size(kKeys)), true};
}  // namespace hit_test

namespace image_list {
inline constexpr KeySpec kKeys[] = {
    {"count", "num", false},
    {"bytes", "num", false},
    {"images", "arr", false},
};
inline constexpr PayloadSpec kSpec{"imageList", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace image_list

namespace image_hit {
inline constexpr KeySpec kKeys[] = {
    {"index", "num", false},
    {"handle", "num", false},
};
inline constexpr PayloadSpec kSpec{"imageHit", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace image_hit

namespace watermark {
inline constexpr KeySpec kKeys[] = {
    {"enabled", "bool", false},
    {"text", "str", false},
    {"sizePt", "num", false},
    {"color", "arr", false},
    {"angle", "num", false},
};
inline constexpr PayloadSpec kSpec{"watermark", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace watermark

namespace controls {
inline constexpr KeySpec kKeys[] = {
    {"mode", "num", false},
    {"count", "num", false},
    {"caret", "num", false},
    {"items", "arr", false},
};
inline constexpr PayloadSpec kSpec{"controls", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace controls

namespace lines {
inline constexpr KeySpec kKeys[] = {
    {"lines", "arr", false},
    {"glyphs", "arr", false},
};
inline constexpr PayloadSpec kSpec{"lines", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace lines

namespace bracket_slots {
inline constexpr KeySpec kKeys[] = {
    {"slots", "arr", false},
};
inline constexpr PayloadSpec kSpec{"bracketSlots", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace bracket_slots

namespace edit_stats {
inline constexpr KeySpec kKeys[] = {
    {"layoutRan", "bool", false},
    {"relaidLines", "num", false},
    {"totalLines", "num", false},
    {"relaidParagraphs", "num", false},
    {"reusedParagraphs", "num", false},
    {"suffixShifted", "bool", false},
    {"totalParagraphs", "num", false},
    {"layoutWasFull", "bool", false},
    {"dirtyPageCount", "num", false},
    {"repaintedPages", "num", false},
    {"totalPages", "num", false},
    {"windowPages", "num", false},
    {"windowFirstPage", "num", false},
    {"bandY0", "num", false},
    {"bandY1", "num", false},
    {"repaintedPixels", "num", false},
    {"repaintedX", "num", false},
    {"repaintedY", "num", false},
    {"repaintedW", "num", false},
    {"repaintedH", "num", false},
    {"frameW", "num", false},
    {"frameH", "num", false},
    {"framePixels", "num", false},
    {"layoutMs", "num", false},
    {"rebuildCompileMs", "num", false},
    {"rebuildReused", "num", false},
    {"rebuildFlowMs", "num", false},
    {"rebuildFlowPosMs", "num", false},
    {"rebuildCopyMs", "num", false},
    {"rebuildControlsMs", "num", false},
    {"rebuildPath", "num", false},
    {"rebuildReject", "num", false},
    {"pageFpMs", "num", false},
    {"paintMs", "num", false},
    {"totalMs", "num", false},
    {"caretPara", "num", false},
    {"caretOffset", "num", false},
    {"glyphCount", "num", false},
    {"missingGlyphCount", "num", false},
    {"fontCacheHits", "num", false},
    {"fontCacheMisses", "num", false},
    {"fontStorageReads", "num", false},
    {"fontStorageBytes", "num", false},
    {"fontIoMs", "num", false},
    {"fontResidentBytes", "num", false},
    {"fontStorageFailures", "num", false},
    {"fontStorageEofs", "num", false},
    {"caretIndex", "num", false},
    {"docCodepoints", "num", false},
    {"wordCount", "num", false},
    {"caretSide", "num", false},
    {"caretVertical", "bool", false},
    {"caretBlockReversed", "bool", false},
    {"caretPage", "num", false},
    {"caretVisible", "bool", false},
    {"caretX", "num", false},
    {"caretY", "num", false},
    {"caretW", "num", false},
    {"caretH", "num", false},
    {"editMode", "num", false},
    {"controlCount", "num", false},
    {"caretControl", "num", false},
    {"blockedEdits", "num", false},
};
inline constexpr PayloadSpec kSpec{"editStats", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace edit_stats

namespace diff {
inline constexpr KeySpec kKeys[] = {
    {"diff", "num", false},
    {"frameW", "num", false},
    {"frameH", "num", false},
    {"box", "num", true},
};
inline constexpr PayloadSpec kSpec{"diff", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace diff

namespace audit_page {
inline constexpr KeySpec kKeys[] = {
    {"diff", "num", false},
    {"w", "num", false},
    {"h", "num", false},
    {"page", "num", false},
    {"cached", "num", false},
    {"box", "num", true},
};
inline constexpr PayloadSpec kSpec{"auditPage", kKeys, static_cast<int>(std::size(kKeys)), true};
}  // namespace audit_page

namespace audit_tables {
inline constexpr KeySpec kKeys[] = {
    {"diff", "num", false},
    {"cells", "num", false},
    {"emptyCell", "num", false},
    {"badRange", "num", false},
    {"overlap", "num", false},
    {"noMark", "num", false},
    {"pathMismatch", "num", false},
    {"badSpan", "num", false},
    {"stray", "num", false},
    {"firstCell", "num", false},
};
inline constexpr PayloadSpec kSpec{"auditTables", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace audit_tables

namespace audit_layout {
inline constexpr KeySpec kKeys[] = {
    {"note", "num", false},
    {"diff", "num", false},
    {"glyphDiff", "num", false},
    {"lineDiff", "num", false},
    {"paraDiff", "num", false},
    {"startsDiff", "num", false},
    {"tableDiff", "num", false},
    {"statsSame", "bool", false},
    {"first", "obj", false},
    {"glyphs", "num", false},
    {"lines", "num", false},
    {"paras", "num", false},
    {"relaidParagraphs", "num", false},
    {"relaidLines", "num", false},
    {"layoutWasFull", "bool", false},
    {"curMs", "num", false},
    {"fullMs", "num", false},
};
inline constexpr PayloadSpec kSpec{"auditLayout", kKeys, static_cast<int>(std::size(kKeys)), true};
}  // namespace audit_layout

namespace audit_glyph_pages {
inline constexpr KeySpec kKeys[] = {
    {"diff", "num", false},
    {"pages", "num", false},
    {"glyphs", "num", false},
    {"badPages", "num", false},
    {"firstBadPage", "num", false},
    {"missing", "num", false},
    {"extra", "num", false},
    {"stray", "num", false},
};
inline constexpr PayloadSpec kSpec{"auditGlyphPages", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace audit_glyph_pages

namespace audit_compile {
inline constexpr KeySpec kKeys[] = {
    {"note", "num", false},
    {"diff", "num", false},
    {"paras", "num", false},
    {"curMs", "num", false},
    {"fullMs", "num", false},
};
inline constexpr PayloadSpec kSpec{"auditCompile", kKeys, static_cast<int>(std::size(kKeys)), true};
}  // namespace audit_compile

namespace table_grid {
inline constexpr KeySpec kKeys[] = {
    {"table", "num", true},
    {"page", "num", true},
    {"x", "num", true},
    {"w", "num", true},
    {"cols", "arr", true},
    {"rows", "arr", true},
    {"ok", "bool", false},
};
inline constexpr PayloadSpec kSpec{"tableGrid", kKeys, static_cast<int>(std::size(kKeys)), true};
}  // namespace table_grid

namespace table_state {
inline constexpr KeySpec kKeys[] = {
    {"inTable", "num", false},
    {"rows", "num", false},
    {"cols", "num", false},
    {"widthPt", "num", false},
    {"align", "num", false},
    {"headerRows", "num", false},
    {"row", "num", false},
    {"rowHeader", "num", false},
    {"rowSplit", "num", false},
    {"rowBreak", "num", false},
    {"heightPt", "num", false},
    {"heightRule", "num", false},
    {"cellRow", "num", false},
    {"cellCol", "num", false},
    {"rowSpan", "num", false},
    {"colSpan", "num", false},
    {"vAlign", "num", false},
    {"selCells", "num", false},
    {"selRows", "num", false},
    {"selCols", "num", false},
    {"canInsertRow", "num", false},
    {"canRemoveRow", "num", false},
    {"canInsertColumn", "num", false},
    {"canRemoveColumn", "num", false},
    {"canMerge", "num", false},
    {"canSplit", "num", false},
    {"canRemoveTable", "num", false},
};
inline constexpr PayloadSpec kSpec{"tableState", kKeys, static_cast<int>(std::size(kKeys)), true};
}  // namespace table_state

namespace context_target {
inline constexpr KeySpec kKeys[] = {
    {"kind", "num", false},
    {"para", "num", false},
    {"offset", "num", false},
    {"cellTable", "num", false},
    {"cellRow", "num", false},
    {"cellCol", "num", false},
    {"control", "num", false},
    {"image", "num", false},
};
inline constexpr PayloadSpec kSpec{"contextTarget", kKeys, static_cast<int>(std::size(kKeys)), false};
}  // namespace context_target

namespace drop_spot {
inline constexpr KeySpec kKeys[] = {
    {"page", "num", false},
    {"x", "num", false},
    {"y", "num", false},
    {"h", "num", false},
    {"w", "num", false},
    {"para", "num", false},
    {"offset", "num", false},
};
inline constexpr PayloadSpec kSpec{"dropSpot", kKeys, static_cast<int>(std::size(kKeys)), true};
}  // namespace drop_spot

}  // namespace pk
