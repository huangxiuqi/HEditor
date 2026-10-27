// 本轮新增功能的判据：双击 / 三击选区、水印、表格列宽与行高、拖拽移动、图片环绕。
//
// 每一条都钉在"看得见或读得到"的量上：选区落在哪几个字、水印记录读回什么、
// 列边界挪到哪个磅值、行盒的宽度与纵向位置、浮动表里有没有几何。
// 逐像素判据不在这里（跨编译器与 CPU 路径不可比），那一层在浏览器端测试里。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/doc/document.h"
#include "core/doc/image_spec.h"
#include "core/doc/section_spec.h"
#include "core/engine/engine.h"
#include "core/layout/layout.h"
#include "core/utils/units.h"

#ifndef HEDITOR_NATIVE_ASSET_DIR
#define HEDITOR_NATIVE_ASSET_DIR "."
#endif

using namespace heditor;

namespace {

int g_checks = 0;
int g_failures = 0;

/**
 * 记一条断言。
 * @param cond 成立条件
 * @param what 说明
 */
void expectTrue(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) {
        std::printf("  [PASS] %s\n", what.c_str());
        return;
    }
    ++g_failures;
    std::printf("  [FAIL] %s\n", what.c_str());
}

/**
 * 拼路径。
 * @param dir 目录
 * @param name 文件名
 * @returns 拼好的路径
 */
std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    return (last == '/' || last == '\\') ? dir + name : dir + "/" + name;
}

/**
 * 选区覆盖的文本（同一段内）。
 * @param e 引擎
 * @returns 选中的文字；跨段或取不到时是空串
 */
std::string selectedText(const EditorEngine& e) {
    const Range sel = e.selection();
    TextPos a{};
    TextPos b{};
    if (!e.flowToTextPos(sel.begin(), &a) || !e.flowToTextPos(sel.end(), &b)) return std::string();
    if (a.para != b.para) return std::string();
    const std::vector<BlockSpec>& bs = e.blocks();
    if (a.para < 0 || a.para >= static_cast<int>(bs.size())) return std::string();
    const std::vector<uint32_t>& text = bs[static_cast<size_t>(a.para)].cp_text;
    if (b.offset > text.size()) return std::string();
    return codepointsToUtf8(
        std::vector<uint32_t>(text.begin() + static_cast<std::ptrdiff_t>(a.offset),
                              text.begin() + static_cast<std::ptrdiff_t>(b.offset)));
}

/**
 * 一段文字的纯文本（整段）。
 * @param e 引擎
 * @param para 段号
 * @returns 文本
 */
std::string paraText(const EditorEngine& e, int para) {
    const std::vector<BlockSpec>& bs = e.blocks();
    if (para < 0 || para >= static_cast<int>(bs.size())) return std::string();
    return codepointsToUtf8(bs[static_cast<size_t>(para)].cp_text);
}

/**
 * 光标矩形的中心（磅），给"点在某个字上"用。
 * @param e 引擎
 * @param out 写回矩形
 * @returns 中心点；取不到时返回 {0,0}
 */
Point caretCenter(const EditorEngine& e, Rect* out) {
    Rect r{};
    if (!e.caretRectPt(&r)) return Point{0.0f, 0.0f};
    if (out != nullptr) *out = r;
    return Point{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}

/**
 * 造一份"每段一句"的文档。
 * @param paras 每段的文字
 * @returns 文档树
 */
NodeTree makeDoc(const std::vector<std::string>& paras) {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    for (const std::string& text : paras) {
        if (!text.empty()) {
            tree.setText(tree.appendChild(sec, NodeKind::TextRun), utf8ToCodepoints(text));
        }
        tree.appendChild(sec, NodeKind::ParagraphMark);
    }
    return tree;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string cjk = joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSansSC-VF.ttf");
    const std::string latin = joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSans-Latin-VF.ttf");

    EditorEngine e;
    EngineConfig cfg;
    cfg.zoom = 1.5f;
    e.setConfig(cfg);
    if (!e.init({cjk, latin})) {
        std::fprintf(stderr, "字体加载失败：%s\n", cjk.c_str());
        return 2;
    }

    // 1 双击选词
    std::printf("\n[1] 双击选词\n");
    {
        e.setDocument(makeDoc({"患者主诉：反复心悸三天，加重一天。"}), StyleSheet{});
        e.relayout();
        // "患者主诉：反复心悸三天" —— 心在第 7 位、悸在第 8 位。
        e.setCaret(0, 8);
        Rect r{};
        const Point c = caretCenter(e, &r);
        expectTrue(e.selectWordAtPt(c.x, c.y), "双击落在「悸」上选中了一个词");
        // 没有分词器：连续汉字整串算一段（这是对"词"最接近的近似，判据钉住这条口径）。
        expectTrue(selectedText(e) == "反复心悸三天",
                   "选中的是连续汉字段（实际「" + selectedText(e) + "」）");
        // 标点与汉字分属两类：双击落在逗号上只选标点。
        e.setCaret(0, 11);
        const Point c2 = caretCenter(e, nullptr);
        expectTrue(e.selectWordAtPt(c2.x, c2.y), "双击落在逗号上");
        expectTrue(selectedText(e) == "，", "标点自成一类（实际「" + selectedText(e) + "」）");
        // 空段落没有可选内容：返回假，宿主据此退回落光标。
        e.setDocument(makeDoc({"", ""}), StyleSheet{});
        e.relayout();
        e.setCaret(0, 0);
        const Point c3 = caretCenter(e, nullptr);
        expectTrue(!e.selectWordAtPt(c3.x, c3.y), "空段落双击没有词可选（返回假）");
    }

    // 2 三击选段
    std::printf("\n[2] 三击选段\n");
    {
        e.setDocument(makeDoc({"第一段文字内容", "第二段文字"}), StyleSheet{});
        e.relayout();
        e.setCaret(0, 3);
        const Point c = caretCenter(e, nullptr);
        expectTrue(e.selectParaAtPt(c.x, c.y), "三击选中整段");
        expectTrue(selectedText(e) == paraText(e, 0), "选中的就是这一段全部文字");
    }

    // 3 水印
    std::printf("\n[3] 水印\n");
    {
        e.setDocument(makeDoc({"正文一段"}), StyleSheet{});
        e.relayout();
        WatermarkSpec w;
        w.enabled = true;
        w.text = "草稿";
        w.size_pt = 40.0f;
        w.color = Color{128, 128, 128, 80};
        w.angle_deg = -45.0f;
        expectTrue(e.setWatermarkAndRender(w), "设水印成功");
        const WatermarkSpec back = e.watermark();
        expectTrue(back.enabled && back.text == "草稿", "读回水印（开启且文字对）");
        expectTrue(back.size_pt == 40.0f, "读回字号");
        expectTrue(back.color == (Color{128, 128, 128, 80}), "读回颜色与不透明度");
        expectTrue(e.undoAndRender(), "撤销这一步");
        expectTrue(!e.watermark().enabled, "撤销之后水印不画了（一步撤销）");
        // 幂等：同一个值再设一次不产生修订。
        WatermarkSpec off;
        expectTrue(e.setWatermark(off), "设成默认值");
        expectTrue(!e.canUndo() || e.undoAndRender(), "再撤销一次");
        expectTrue(!e.watermark().enabled, "水印仍是关闭的");
    }

    // 4 表格列宽与行高
    std::printf("\n[4] 表格列宽与行高\n");
    {
        e.setDocument(makeDoc({"表格前一段"}), StyleSheet{});
        e.relayout();
        e.setCaret(0, 0);
        expectTrue(e.insertTableAndRender(2, 2, false), "插入 2x2 表格");
        expectTrue(!e.layout().placed_tables.empty(), "排版里有这张表的几何");
        const PlacedTable pt0 = e.layout().placed_tables.front();
        const uint64_t handle = pt0.handle;
        const float col_w0 = pt0.col_x[1] - pt0.col_x[0];
        const float row_h0 = pt0.placed.empty() ? 0.0f : pt0.placed.front().h;
        expectTrue(col_w0 > 1.0f, "初始列宽是正值（自动平分）");

        // 格线命中：内部那条列边界判得出来，格心判不出来。
        //   宿主拖列宽全靠它 —— 判错一格就会在"看起来要拖线"的地方落光标。
        {
            uint64_t t = 0;
            int gc = 0;
            int gr = -1;
            const float line_x = pt0.x + pt0.col_x[1];
            const float mid_y =
                pt0.placed.empty() ? 0.0f : pt0.placed.front().y + pt0.placed.front().h * 0.5f;
            expectTrue(e.tableGridLineAt(line_x + 2.0f, mid_y, &t, &gc, &gr),
                       "压在内部列边界上：判得出格线");
            expectTrue(t == handle && gc == 1 && gr < 0,
                       "给的是这张表、第一条内部列边界、没有行边界");
            expectTrue(!e.tableGridLineAt(pt0.x + col_w0 * 0.5f, mid_y, &t, &gc, &gr),
                       "格心不在格线上：判不出来（这一下该落光标）");
            // 左边界不是格线（左边没有列可以让给它），右边界是：拖它加宽最后一列，
            //   表宽跟着走（Word 同一条）。表尾那一行的下边是表的底框，同样照拖。
            expectTrue(!e.tableGridLineAt(pt0.x, mid_y, &t, &gc, &gr), "表格左边界不算格线");
            expectTrue(e.tableGridLineAt(pt0.x + pt0.w, mid_y, &t, &gc, &gr),
                       "表格右边界算格线（拖它加宽最后一列）");
            expectTrue(t == handle && gc == pt0.cols && gr < 0,
                       "右边界报的是最后一条列边界、没有行边界");
            {
                const PlacedRow& last = pt0.placed.back();
                expectTrue(e.tableGridLineAt(pt0.x + pt0.w * 0.25f, last.y + last.h, &t, &gc, &gr),
                           "表格底边框算格线");
                expectTrue(t == handle && gr == last.row && gc == 0,
                           "底边框报的是最后一行、没有列边界");
            }
        }

        // 拖拽语义：相邻两列之和不变（表宽不变）。这里给 1:3 的比例，验它落成 1:3。
        const float table_w = pt0.w;
        expectTrue(e.setTableColWidthsAndRender(handle, {table_w * 0.25f, table_w * 0.75f}),
                   "把两列钉成 1:3");
        {
            const PlacedTable& a = e.layout().placed_tables.front();
            const float w0 = a.col_x[1] - a.col_x[0];
            const float w1 = a.col_x[2] - a.col_x[1];
            expectTrue(std::fabs(w0 - table_w * 0.25f) < 0.8f, "第一列 = 表宽的 1/4（实际 " +
                                                                   std::to_string(w0) + "，表宽 " +
                                                                   std::to_string(table_w) + "）");
            expectTrue(std::fabs(w0 + w1 - table_w) < 0.8f,
                       "两列之和仍等于表宽（实际 " + std::to_string(w0 + w1) + "）");
            expectTrue(std::fabs(a.col_x[2] - table_w) < 0.01f, "最后一条列边界就是表宽");
        }
        expectTrue(e.setTableRowHeightAndRender(handle, 0, 90.0f), "把第一行高钉成 90 磅");
        {
            const PlacedTable& a = e.layout().placed_tables.front();
            expectTrue(!a.placed.empty() && a.placed.front().h >= 89.0f,
                       "第一行高 >= 90 磅（实际 " +
                           std::to_string(a.placed.empty() ? 0.0f : a.placed.front().h) + "）");
        }
        expectTrue(e.undoAndRender(), "撤销行高");
        expectTrue(e.undoAndRender(), "撤销列宽");
        {
            const PlacedTable& a = e.layout().placed_tables.front();
            const float w0 = a.col_x[1] - a.col_x[0];
            expectTrue(std::fabs(w0 - col_w0) < 0.6f, "撤销之后列宽回到自动平分");
            const float h = a.placed.empty() ? 0.0f : a.placed.front().h;
            expectTrue(std::fabs(h - row_h0) < 0.6f, "撤销之后行高回到按内容");
        }
        // 非文档模式拒绝：页面设置是设计动作。
        e.setEditMode(EditMode::ReadOnly);
        expectTrue(!e.setTableColWidths(handle, {120.0f, 120.0f}), "只读模式拒绝改列宽");
        e.setEditMode(EditMode::Document);
    }

    // 5 拖拽移动
    std::printf("\n[5] 拖拽移动选区与图片\n");
    {
        e.setDocument(makeDoc({"abcdefghij"}), StyleSheet{});
        e.relayout();
        // 目标点先取：段首那个字的位置。
        e.setCaret(0, 0);
        const Point dest = caretCenter(e, nullptr);
        // 选中 "cde"（流位置上第 2..5 位）。
        const FlowPos lo = e.flowPosAt(0, 2);
        const FlowPos hi = e.flowPosAt(0, 5);
        expectTrue(e.setSelection(Range{lo, hi}), "选中 cde");
        const auto kindAt = [&e](float x, float y) -> int {
            TextPos p{};
            int side = 0;
            if (!e.hitTestPt(x, y, &p, nullptr, &side)) return -1;
            return e.dragKindAt(p, side, e.imageAtPt(x, y));
        };
        expectTrue(kindAt(dest.x, dest.y) == 0, "点在选区之外：不能拖（不会误把落点当起手）");
        // 点在选区里面 → 1。
        e.setCaret(0, 3);
        const Point inside = caretCenter(e, nullptr);
        expectTrue(e.setSelection(Range{lo, hi}), "重新选中 cde");
        expectTrue(kindAt(inside.x, inside.y) == 1, "点在选区里：可以拖选区");
        // 起点那一格不算内部：从选区开头起手往外拖，表达的是"从这里重新选"。
        e.setCaret(0, 2);
        const Point at_lo = caretCenter(e, nullptr);
        expectTrue(e.setSelection(Range{lo, hi}), "再选中 cde");
        expectTrue(kindAt(at_lo.x, at_lo.y) == 0,
                   "压在选区起点：判成拖选，不是搬家（反向拖选要靠它）");

        expectTrue(e.moveSelectionToPt(dest.x, dest.y), "把 cde 搬到段首");
        expectTrue(paraText(e, 0) == "cdeabfghij",
                   "文字变成 cdeabfghij（实际 " + paraText(e, 0) + "）");
        expectTrue(e.undoAndRender(), "撤销移动");
        expectTrue(paraText(e, 0) == "abcdefghij", "撤销之后回到原样（一次移动一步撤销）");

        // 目标落在选区内部：拒绝。
        e.setSelection(Range{e.flowPosAt(0, 2), e.flowPosAt(0, 6)});
        e.setCaret(0, 4);
        const Point same = caretCenter(e, nullptr);
        expectTrue(!e.moveSelectionToPt(same.x, same.y),
                   "目标落回选区内部：拒绝（原地搬没有意义）");

        // 图片搬运：插一张图，再把它拖到段首。
        std::vector<uint8_t> px(4u * 4u * 4u, 180);
        std::string err;
        expectTrue(e.registerImage("dot", 4, 4, px.data(), px.size(), &err), "注册 4x4 像素图");
        ImageSpec spec;
        spec.resource_id = "dot";
        spec.px_w = 4;
        spec.px_h = 4;
        spec.dpi = 96;
        spec.width = Length::abs(ptToEmu(24.0f));
        spec.height = Length::abs(ptToEmu(24.0f));
        e.setCaret(0, 10);
        const Point imgPt = caretCenter(e, nullptr);
        expectTrue(e.insertImageAndRender(spec), "在段末插图");
        expectTrue(e.imageCount() == 1, "文档里有一张图");
        const auto atomOwner = [&e]() -> int {
            const std::vector<BlockSpec>& bs = e.blocks();
            for (const BlockSpec& b : bs) {
                for (const InlineObject& o : b.objects) {
                    if (o.kind == InlineObjectKind::Atom) return o.owner;
                }
            }
            return -1;
        };
        expectTrue(e.moveImageToPtAndRender(0, dest.x, dest.y), "把图片搬到段首");
        (void)atomOwner;
        {
            const std::vector<EditorEngine::ImageBox> boxes = e.images();
            int img_clusters = 0;
            int img_owner = -1;
            float img_a0 = 0.0f;
            float img_a1 = 0.0f;
            for (const LineBox& lb : e.layout().lines) {
                for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
                    const LineCluster& c = lb.clusters[ci];
                    if (c.atom != InlineAtom::Image) continue;
                    ++img_clusters;
                    img_owner = c.owner_control;
                    img_a0 = lb.inline_origin_pt + c.inline_pt;
                    img_a1 = (ci + 1 < lb.clusters.size())
                                 ? lb.inline_origin_pt + lb.clusters[ci + 1].inline_pt
                                 : lb.inline_origin_pt + lb.inline_end_pt;
                }
            }
            std::printf("     [diag] 簇 owner=%d 节点=%d a0=%.2f a1=%.2f\n", img_owner,
                        boxes.empty() ? -1 : static_cast<int>(boxes.front().id), img_a0, img_a1);
            std::printf("     [diag] 图片数 %zu rects %zu w=%.2f h=%.2f 簇=%d 文本「%s」\n",
                        boxes.size(), boxes.empty() ? 0u : boxes.front().rects.size(),
                        boxes.empty() ? 0.0f : boxes.front().w_pt,
                        boxes.empty() ? 0.0f : boxes.front().h_pt, img_clusters,
                        paraText(e, 0).c_str());
            expectTrue(!boxes.empty() && !boxes.front().rects.empty(), "搬完之后图片仍有几何");
            if (!boxes.empty() && !boxes.front().rects.empty()) {
                expectTrue(std::fabs(boxes.front().rects.front().r_pt.x - 56.7f) < 40.0f,
                           "图片落到段首那一带（左边接近版心左沿）");
            }
        }
        expectTrue(e.undoAndRender(), "撤销图片移动");
    }

    // 6 图片环绕
    std::printf("\n[6] 图片环绕\n");
    {
        std::string long_line;
        for (int i = 0; i < 40; ++i) long_line += "文字内容";
        e.setDocument(makeDoc({long_line, "第二段", "第三段"}), StyleSheet{});
        e.relayout();

        std::vector<uint8_t> px(4u * 4u * 4u, 200);
        std::string err;
        if (e.imageCount() == 0) {
            expectTrue(e.registerImage("box", 4, 4, px.data(), px.size(), &err), "注册 4x4 像素图");
        }
        ImageSpec spec;
        spec.resource_id = "box";
        spec.px_w = 4;
        spec.px_h = 4;
        spec.dpi = 96;
        spec.width = Length::abs(ptToEmu(120.0f));
        spec.height = Length::abs(ptToEmu(80.0f));
        e.setCaret(0, 0);
        expectTrue(e.insertImageAndRender(spec), "在段首插一张 120x80 的图");
        const size_t inline_floats = e.layout().placed_floats.size();
        expectTrue(inline_floats == 0, "行内图不进浮动表");
        const float inline_end =
            e.layout().lines.empty() ? 0.0f : e.layout().lines.front().inline_end_pt;
        // 行内时图落在哪儿（磅）。改环绕方式之后要拿它比"图有没有留在原处"。
        Rect inline_rect{};
        {
            const std::vector<EditorEngine::ImageBox> boxes = e.images();
            if (!boxes.empty() && !boxes.front().rects.empty()) {
                inline_rect = boxes.front().rects.front().r_pt;
            }
        }
        expectTrue(inline_rect.w > 1.0f, "行内图有几何（后面几条要用它）");

        expectTrue(e.setImageLayoutAndRender(0, 1), "改成四周型");
        expectTrue(e.layout().placed_floats.size() == 1, "四周型：浮动表里有一条几何");
        {
            const PlacedFloat& pf = e.layout().placed_floats.front();
            expectTrue(std::fabs(pf.w - 120.0f) < 0.5f && std::fabs(pf.h - 80.0f) < 0.5f,
                       "浮动框尺寸 = 120x80 磅");
            // 只看与图纵向重叠的那几行：它们让开了一侧，行内长度应明显短于整宽。
            float widest_beside = 0.0f;
            int beside = 0;
            for (const LineBox& lb : e.layout().lines) {
                if (lb.page != pf.page) continue;
                if (lb.block_hi_pt <= pf.y || lb.block_lo_pt >= pf.y + pf.h) continue;
                ++beside;
                widest_beside = std::max(widest_beside, lb.inline_end_pt);
            }
            expectTrue(beside > 0, "图旁边确实排了行");
            // 位置不变：改环绕方式只改文字的流法，图不该跳到段首去 ——
            //   跳了的话读者会以为图被删了又插了一张。
            expectTrue(
                std::fabs(pf.x - inline_rect.x) < 1.0f && std::fabs(pf.y - inline_rect.y) < 1.0f,
                "四周型：图留在原位置（不跳段首）");
            // 行盒起点不能晚于这一行的第一个字形：晚于它，光标与选区就落在图左边，
            //   而字画在图右边 —— 选中一个字，矩形却在图的另一边。
            bool origin_ok = true;
            for (const LineBox& lb : e.layout().lines) {
                if (lb.vertical) continue;
                if (lb.block_hi_pt <= pf.y || lb.block_lo_pt >= pf.y + pf.h) continue;
                float min_x = 1.0e9f;
                for (const PlacedGlyph& g : e.layout().glyphs) {
                    if (g.line != lb.line || g.vertical) continue;
                    min_x = std::min(min_x, g.x_pt);
                }
                if (min_x < 1.0e8f && min_x < lb.inline_origin_pt - 2.0f) origin_ok = false;
            }
            expectTrue(origin_ok, "让宽的那些行：行盒起点不晚于字形（选区与文字同一侧）");
            expectTrue(widest_beside < inline_end - 60.0f,
                       "四周型让出宽度：图旁的行比整宽窄（" + std::to_string(widest_beside) +
                           " < " + std::to_string(inline_end) + "）");
        }
        // 图挪到版心中间：四周型要把一行分成左右两段（Word 的做法）。
        //   贴着左边时只有右侧一段，看不出两段的差别；中间这个位置才验得到。
        expectTrue(e.moveImageToPtAndRender(0, 0.0f, 0.0f, true, 150.0f, 0.0f),
                   "把图往右挪 150 磅（放到版心中间）");
        {
            const PlacedFloat& pf = e.layout().placed_floats.front();
            int two_sided = 0;
            bool sides_ok = true;
            for (const LineBox& lb : e.layout().lines) {
                if (lb.page != pf.page) continue;
                if (lb.block_hi_pt <= pf.y || lb.block_lo_pt >= pf.y + pf.h) continue;
                if (lb.spans.size() < 2) continue;
                ++two_sided;
                const float gap_lo = pf.x - lb.inline_origin_pt;
                const float gap_hi = pf.x + pf.w - lb.inline_origin_pt;
                bool any_left = false;
                bool any_right = false;
                bool in_gap = false;
                for (const LineCluster& c : lb.clusters) {
                    if (c.inline_pt < gap_lo - 1.0f) any_left = true;
                    if (c.inline_pt >= gap_hi) any_right = true;
                    if (c.inline_pt > gap_lo && c.inline_pt < gap_hi) in_gap = true;
                }
                if (!any_left || !any_right || in_gap) sides_ok = false;
            }
            expectTrue(two_sided > 0, "图在中间：至少一行分成左右两段");
            expectTrue(sides_ok, "两段行：左段的字在图左、右段的字在图右、没有簇落在图上");
            // 选区按段切：一条横跨图的矩形会把图也涂上。
            e.selectAll();
            bool overlap = false;
            for (const EditorEngine::PageRect& pr : e.selectionRects()) {
                if (pr.page != pf.page) continue;
                // 只看与图同一条纵向带上的矩形：图上方与下方的行本来是整宽的，
                //   它们横跨图所在的那几列也不算"盖住图"。
                if (pr.r_pt.y >= pf.y + pf.h - 1.0f || pr.r_pt.y + pr.r_pt.h <= pf.y + 1.0f) {
                    continue;
                }
                const float lo = std::max(pr.r_pt.x, pf.x);
                const float hi = std::min(pr.r_pt.x + pr.r_pt.w, pf.x + pf.w);
                if (hi - lo > 2.0f) overlap = true;
            }
            expectTrue(!overlap, "整段选区不横跨图：每一条矩形都让开图");
            e.clearSelection();
        }
        expectTrue(e.moveImageToPtAndRender(0, 0.0f, 0.0f, true, -150.0f, 0.0f), "把图挪回原处");
        expectTrue(e.setImageLayoutAndRender(0, 2), "改成上下型");
        {
            // 上下型：文字不进图片的纵向区间 —— 第二段的行顶在图片底边之下。
            const PlacedFloat& pf = e.layout().placed_floats.front();
            bool any_below = false;
            for (const LineBox& lb : e.layout().lines) {
                if (lb.paragraph == 0) continue;
                if (lb.page > pf.page ||
                    (lb.page == pf.page && lb.block_lo_pt >= pf.y + pf.h - 1.0f)) {
                    any_below = true;
                }
            }
            expectTrue(any_below, "上下型：后续段落排在图片底边之下");
        }
        expectTrue(e.setImageLayoutAndRender(0, 3), "改成衬于文字下方");
        {
            expectTrue(e.layout().placed_floats.size() == 1, "衬于下方也记一条几何（要画出来）");
            float widest = 0.0f;
            for (const LineBox& lb : e.layout().lines) widest = std::max(widest, lb.inline_end_pt);
            expectTrue(std::fabs(widest - inline_end) < 1.0f, "衬于下方不占位：行宽与行内时一样宽");
        }
        expectTrue(e.setImageLayoutAndRender(0, 4), "改成浮于文字上方");
        expectTrue(e.layout().placed_floats.size() == 1, "浮于上方同样只叠不占位");
        expectTrue(e.setImageLayoutAndRender(0, 0), "改回嵌入型");
        expectTrue(e.layout().placed_floats.empty(), "嵌回行内：浮动表清空");
        {
            const std::vector<EditorEngine::ImageBox> boxes = e.images();
            const Rect back = (!boxes.empty() && !boxes.front().rects.empty())
                                  ? boxes.front().rects.front().r_pt
                                  : Rect{};
            expectTrue(std::fabs(back.x - inline_rect.x) < 1.0f &&
                           std::fabs(back.y - inline_rect.y) < 1.0f,
                       "改回行内：图还在原来的位置（没有残留一份浮动的几何）");
        }
        expectTrue(e.undoAndRender(), "撤销回上一档");
        expectTrue(!e.layout().placed_floats.empty(), "撤销之后又是浮动的");
    }

    std::printf("\n──────────────────────────────\n");
    std::printf("共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
