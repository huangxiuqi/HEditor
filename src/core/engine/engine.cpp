/**
 * 引擎门面与编辑会话。
 *
 * 这里放三样东西：生命周期与文档装载（init / setConfig / setDocument）、
 * 把排版结果暴露给宿主的查询（帧、光标、命中、控件）、以及编辑入口（插入删除、分段合并、
 * 缩进）。
 * 排版调度在 engine_layout.cpp、呈现与光栅增量在 engine_paint.cpp、审计判据在
 * engine_audit.cpp、诊断输出在 engine_diag.cpp —— 拆开是因为它们的变更理由不同。
 *
 * 编辑入口这一段将来会搬到命令层（见 docs/正式化架构与迁移路线.md 阶段 2），
 * 所以现在不为它做进一步的职责拆分。
 */

#include "core/engine/engine.h"

#include "core/doc/record_json.h"
#include "core/engine/control_spans.h"

#include "core/rev/fragment.h"
#include "core/rev/project.h"

#include "core/text/writing_mode.h"
#include "core/utils/hash.h"
#include "core/utils/time.h"
#include "core/utils/units.h"

#include <hb.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

#include "core/text/shaper.h"

namespace heditor {

namespace {

/**
 * 业务类型字符串 → 旧表示的三种控件种类。
 *
 * 两者层次不同：controlType 是开放的（宿主可以注册一种新的），ControlKind 是封闭的三档 ——
 * 只够表达"值怎么校验、怎么显示"这一层，界面上的差别（复选、下拉、日期浮层）要等控件类型
 * 注册表落地。认不出来的类型回落到文本：宁可当一个普通文本框，也不要让一个没登记的类型
 * 变成一个"打不进去"的字段。空串同理（没写类型的控件就是文本框）。
 */
ControlKind controlKindFromBusinessType(const std::string& type) {
    if (type == "number") return ControlKind::NumberField;
    if (type == "date") return ControlKind::DateField;

    return ControlKind::TextField;
}

/**
 * 光标宽度的下限（磅）。窄于它之后，缩放到 1.0 以下时细到看不见 ——
 * 而"看不见的光标"与"没有光标"在界面上无法区分。比它更细的取值一律夹到这里。
 */
constexpr float kMinCaretWidthPt = 0.4f;

/**
 * Skia 全局字形 mask 缓存的字节预算。
 *
 * SkStrikeCache 的默认上限是 2 MB（SK_DEFAULT_FONT_CACHE_LIMIT），而它清理时按
 *   "整份 strike"删：internalPurge 断言不做小清理（bytesNeeded 至少取已用的四分之一），
 *   从 LRU 尾巴上摘对象，一次就是一份 strike。本项目一般只有一两个字体规格，
 *   也就是一个 strike —— 越过 2 MB 就等于缓存被整份抹掉，接下来每个字形都重新冷光栅，
 *   而"预热"这件事也跟着作废（预热正是把 mask 送进这份缓存）。
 * 取 48 MB：一页约 700 个字形、mask 数百字节，再乘 setSubpixel(true) 的横向相位
 *   （同一字形按 1/4 px 拆成多份 mask），一页的常驻量在 MB 量级；整篇用到的不同字形
 *   撑死几万份 mask，48 MB 足够整篇一次装下、之后只命中。
 */
constexpr size_t kGlyphCacheBudgetBytes = 48u * 1024u * 1024u;

}  // namespace

/**
 * 生命周期：字体加载与引擎初始化。
 */

bool EditorEngine::init(const std::vector<std::string>& fontPaths) {
    // 视图先摆好：init 之后任何一次光标读写都要它，而它只存容器地址（之后不再变）。
    refreshViews();
    for (const auto& p : fontPaths) {
        if (fonts_.addFace(p) < 0) return false;
    }
    if (fonts_.faceCount() <= 0) return false;

    // 字形缓存在 Skia 侧是进程内一份 strike 缓存（与是哪一块 Bitmap 无关），
    //   所以这里只需要把预算调够，没有"两块图各存一份"的问题。
    ensureRenderSetup();
    return true;
}

/**
 * 光栅后端那几项一次性设置。目前只有一项：Skia 全局字形 mask 缓存的预算。
 *
 * 为什么必须显式设：SkStrikeCache 的默认上限只有 2 MB（SK_DEFAULT_FONT_CACHE_LIMIT），
 *   而它清理时按"整份 strike"删 —— internalPurge 断言"不做小清理"，从 LRU 尾巴上
 *   摘对象，一次就是一份 strike。本项目一般只有一两个字体规格，也就是一个 strike：
 *   越限就等于把缓存一次抹光，接下来每个字形都重新冷光栅。
 * 幂等：engine.h 里记了为什么要放在这里而不是 init()（wasm 宿主不走 init）。
 */
void EditorEngine::ensureRenderSetup() {
    if (render_setup_done_) return;
    render_setup_done_ = true;
    // 字体表先立起来：其余位图都 shareFonts(font_owner_)，而 reset(0,0) 正是
    //   建出那份共享表的最小代价（一像素都不分配）。
    font_owner_.reset(0, 0);
    font_owner_.setGlyphCacheBudget(kGlyphCacheBudgetBytes);
}

/**
 * 把颜色混进一个 FNV 值。光栅签名与页面底色共用同一套编码 —— 同一个颜色在两个地方
 * 算出两种哈希，排查时就会以为改的不是同一项。
 */
static uint64_t fnvMixColor(uint64_t h, const Color& c) {
    return fnvMix(h, (static_cast<uint64_t>(c.r) << 24) | (static_cast<uint64_t>(c.g) << 16) |
                         (static_cast<uint64_t>(c.b) << 8) | static_cast<uint64_t>(c.a));
}

/**
 * "页位图上画出来是什么"这组配置的签名。
 *
 * 页指纹只哈希字形等内容；页面底色、边框、格线、原子框、缩放、页度量这些一个都不在里面。
 *   把它们混成一个签名记进 PageBitmap，签名一变所有页自动失效 —— 新增光栅配置项时只要
 *   加进这里，不必再维护一张"记得清缓存"的清单（TRAPS 128 的正解）。
 * 与排版无关的项刻意不收：页间隙只影响落点、背景色只影响帧、光标/选区色只影响瞬态层。
 */
static uint64_t rasterSignature(const EngineConfig& cfg, bool marks_shown,
                                uint64_t image_generation) {
    uint64_t h = kFnvBasis;
    h = fnvMixFloat(h, cfg.zoom);
    h = fnvMixFloat(h, cfg.page.width_pt);
    h = fnvMixFloat(h, cfg.page.height_pt);
    h = fnvMixFloat(h, cfg.page.margins.top);
    h = fnvMixFloat(h, cfg.page.margins.right);
    h = fnvMixFloat(h, cfg.page.margins.bottom);
    h = fnvMixFloat(h, cfg.page.margins.left);
    h = fnvMixFloat(h, cfg.page.slab_pt);
    h = fnvMix(h, cfg.page.continuous ? 1ULL : 0ULL);
    h = fnvMixColor(h, cfg.page_fill);
    h = fnvMixColor(h, cfg.page_border);
    h = fnvMix(h, cfg.draw_page_border ? 1ULL : 0ULL);
    h = fnvMix(h, cfg.show_margin_marks ? 1ULL : 0ULL);
    h = fnvMixColor(h, cfg.margin_mark_color);
    h = fnvMixColor(h, cfg.table_line);
    h = fnvMixColor(h, cfg.atom_fill);
    h = fnvMixColor(h, cfg.atom_border);
    h = fnvMixColor(h, cfg.atom_text);
    h = fnvMixFloat(h, cfg.atom_pad_pt);
    // 表单标记：它们画在页位图里，改外观就是"这一页的像素变了"，而排版与页指纹都看不见。
    h = fnvMix(h, cfg.show_brackets ? 1ULL : 0ULL);
    h = fnvMix(h, cfg.show_control_dots ? 1ULL : 0ULL);
    h = fnvMixColor(h, cfg.control_mark);
    h = fnvMixColor(h, cfg.control_mark_ro);
    h = fnvMixFloat(h, cfg.control_dot_pt);
    // 编辑标记：同样是"画在页位图里、排版看不见"的一层，改档位就是这一页的像素变了。
    //   混的是"此刻画不画"而不是档位本身 —— 只读态不画，而它不进页内容指纹（内容没变），
    //   两处都不管的话，切到只读之后缓存会把"画了标记的那一版像素"一直贴下去。
    h = fnvMix(h, marks_shown ? 1ULL : 0ULL);
    h = fnvMix(h, static_cast<uint64_t>(cfg.marks));
    h = fnvMixColor(h, cfg.mark_color);
    // 图片像素不在文档里，页指纹看不见它们：换了图（或删了资源）只有这一个计数能让
    //   已经光栅好的页作废。漏掉它的症状是"换图之后画面上还是上一张"。
    h = fnvMix(h, image_generation);
    return h;
}

uint64_t EditorEngine::rasterSignatureNow() const {
    return rasterSignature(cfg_, marksShown(), images_.generation());
}

void EditorEngine::setConfig(const EngineConfig& cfg) {
    // 开 / 关括号会改变占位（括号进文档流）→ 必须重排。
    //   不重排的话括号会按新的开关画、正文却按旧的口径排 —— 画的用的还是旧的簇坐标，
    //   表象是括号盖住字/槽里空一条，而且看不出来是哪一边错了。
    //   （其它配置项不动排版结构：zoom / 页度量只影响几何，光栅签名一变页位图就作废
    //     —— 宿主改那两项之后本来就会立刻重排重绘。）
    const bool plan_changed = (cfg.show_brackets != cfg_.show_brackets);
    // 连页标志与条带高度是引擎从"页度量 + 页间隙"装配出来的，排版层只认 PageMetrics。
    //   必须在比较之前装配 —— 否则切模式时 PageMetrics 没变，判不出"要重排"，
    //   画面上就成了"分页的排版 + 连页的落点"（内容按旧断点排，却按新条带贴）。
    PageMetrics want = cfg.page;
    want.continuous = cfg.continuous;
    // 只有连页才需要条带高度。分页下留 0，排版按 slabHeightPt 的回退值算（页高）；
    //   这样改页间隙不会牵动分页的"要不要重排"判据 —— 那个改动只影响落点，不影响断行。
    want.slab_pt =
        cfg.continuous ? want.height_pt + (cfg.page_gap_pt > 0.0f ? cfg.page_gap_pt : 0.0f) : 0.0f;
    // 页度量改了整篇的断行位置都会变，没有可定位的影响面 → 整篇标脏。
    const bool page_changed = (want != cfg_.page);
    cfg_ = cfg;
    cfg_.page = want;
    if (cfg_.zoom < 0.25f) cfg_.zoom = 0.25f;
    if (cfg_.zoom > 8.0f) cfg_.zoom = 8.0f;
    if (cfg_.caret_width_pt < kMinCaretWidthPt) cfg_.caret_width_pt = kMinCaretWidthPt;
    // 光栅签名一变，所有页的像素都不再作数（判据在 pageBitmapFresh）。
    //   这里不再逐项比对再清缓存：那张清单漏一项就是"改了底色屏幕不变"（TRAPS 128）。
    const uint64_t sig = rasterSignatureNow();
    if (sig != raster_sig_) {
        raster_sig_ = sig;
        page_dirty_rects_.clear();  // 旧矩形描述的是旧像素，作废
    }
    if (plan_changed) markLayoutDirtyForBrackets();
    if (page_changed) markAllLayoutDirty();
    if (plan_changed || page_changed) relayout();
    // 撤销上限不影响呈现，只影响栈能留多长 —— 调小的话当场裁掉多余的（不必等下一次提交）。
    if (session_) session_->setLimit(undoLimit());
    refreshViews();
}

void EditorEngine::setCaretAndSelectionStyle(const Color& caret, float caret_width_pt,
                                             const Color& selection) {
    cfg_.caret_color = caret;
    cfg_.caret_width_pt = caret_width_pt < kMinCaretWidthPt ? kMinCaretWidthPt : caret_width_pt;
    cfg_.selection_color = selection;
    refreshViews();
}

/**
 * 一份修订装上之后的公共收尾（装载与打开共用）。
 *
 * 顺序不能变：会话先换（下面每一步都读 document()），再投影、最后整篇标脏。
 *   页缓存与脏矩形一起清：它们描述的是上一份文档的像素，页号相同不代表内容相同，
 *   而"哪个页号对应哪段内容"正是换文档时唯一不能沿用的东西。
 */
void EditorEngine::installDocument(rev::Doc doc) {
    session_.emplace(std::move(doc), cfg_.undo_limit);
    proj_hist_ = rev::BlockHistory{};
    loaded_ = true;
    sel_history_.clear();
    sel_pos_ = 0;
    setCaretByText(TextPos{0, 0});
    page_cache_.clear();
    page_dirty_rects_.clear();
    rebuildFromTree();
    // 整份文档换了：段数与内容都可能全变，没有可复用的前缀。
    markAllLayoutDirty();
}

/**
 * 文档装载：模型树那条路（第十四节的接缝）。
 */
void EditorEngine::setDocument(const NodeTree& tree, const StyleSheet& sheet) {
    refreshViews();
    rev::Doc doc;
    std::string why;
    if (!rev::fromTree(tree, &doc, &why)) {
        // 表示不了的形状拒绝装载：装一半的结果是一份看起来完整的修订，而它与模型不是同一份
        // 文档，调用方没有别的办法知道。宁可停在"没有装载"这个明确状态上。
        loaded_ = false;
        session_.reset();
        proj_hist_ = rev::BlockHistory{};
        blocks_.clear();
        blocks_full_.clear();
        block_src_.clear();
        block_extents_.clear();
        tables_.clear();
        flow_.reset();
        page_cache_.clear();
        page_dirty_rects_.clear();
        return;
    }
    sheet_ = sheet;
    installDocument(std::move(doc));
}

rev::SerializeOptions EditorEngine::fontWriteOptions() const {
    rev::SerializeOptions opt;
    // 身份按稳定性排：PostScript 名（OpenType 里唯一性有规范保证）→ 族名（给人看）→
    //   文件名（可以改，所以排在最后）。文件改名不该让一份文档指向另一张脸。
    opt.font_ref = [this](int index) -> rev::FontRef {
        const FontFace* face = fonts_.face(index);
        if (face == nullptr) return rev::FontRef{};
        return rev::FontRef{face->postScriptName(), face->familyName(), fontFileId(face->path())};
    };
    return opt;
}

rev::DeserializeOptions EditorEngine::fontReadOptions() const {
    rev::DeserializeOptions opt;
    // 与写端同一个顺序找本机那张脸。都找不到就返回 -1（读者保留文件里的原下标，交给
    //   回退链），而不是"随便挑一张" —— 挑错字体比缺字体更难发现。
    opt.resolve_font = [this](const rev::FontRef& ref) -> int {
        int i = fonts_.indexOfPostScript(ref.postscript);
        if (i < 0) i = fonts_.indexOfFamily(ref.family);
        if (i < 0) i = fonts_.indexOfFile(ref.file);
        return i;
    };
    return opt;
}

std::string EditorEngine::fontsJson() const {
    nlohmann::ordered_json arr = nlohmann::ordered_json::array();
    for (int i = 0; i < fonts_.faceCount(); ++i) {
        const FontFace* f = fonts_.face(i);
        if (f == nullptr) continue;
        nlohmann::ordered_json o = nlohmann::ordered_json::object();
        o["index"] = i;
        o["family"] = f->familyName();
        o["postScript"] = f->postScriptName();
        o["file"] = fontFileId(f->path());
        // 按需装载的脸整份不在内存里 —— 这条决定宿主要不要为它算常驻字节。
        o["lazy"] = f->source() != nullptr;
        arr.push_back(std::move(o));
    }
    return arr.dump();
}

rev::SerializeResult EditorEngine::saveDocument() const {
    if (!session_) return rev::SerializeResult{false, std::string(), "没有装载文档"};
    return rev::serialize(session_->doc(), fontWriteOptions());
}

rev::DeserializeResult EditorEngine::loadDocument(const std::string& text) {
    rev::Doc doc;
    rev::DeserializeResult r = rev::deserialize(text, &doc, fontReadOptions());
    // 读坏了就地返回：session_ / 光标 / 页缓存一个字节都没动，调用方拿到的失败
    //   与"这一趟没发生"是同一件事（rev::deserialize 的契约）。
    if (!r.ok) return r;
    // sheet_ 不动：命名样式表属于宿主的配置，不属于文件（v1 的文件里没有样式表）。
    installDocument(std::move(doc));
    return r;
}

ClipCopy EditorEngine::copySelection() const {
    ClipCopy out;
    if (!session_) {
        out.error = "没有装载文档";
        return out;
    }
    if (!caret_.hasSelection()) {
        out.error = "没有选中内容";
        return out;
    }
    const Range sel = caret_.selection();
    const rev::ClipCapture cap =
        rev::captureRange(session_->doc(), flowVisibility(), sel.begin(), sel.end());
    if (!cap.ok) {
        out.error = cap.error;
        return out;
    }
    const rev::ClipWriteResult w = rev::writeFragment(cap.nodes, fontWriteOptions());
    if (!w.ok) {
        out.error = w.error;
        return out;
    }
    out.fragment = w.text;
    out.text = cap.text;
    out.html = w.html;
    out.lo = cap.lo;
    out.hi = cap.hi;
    out.ok = true;
    return out;
}

std::string EditorEngine::pasteFragment(const std::string& text) {
    if (!session_) return "没有装载文档";
    const rev::ClipReadResult r = rev::readFragment(text, fontReadOptions());
    if (!r.ok) return r.error;
    if (!edit_.pasteNodes(r.nodes)) return "粘贴失败：落点不接受这段内容";
    return std::string();
}

std::string EditorEngine::pastePlainText(const std::string& utf8) {
    if (!session_) return "没有装载文档";
    std::vector<NodeSpec> nodes;
    std::string line;
    const auto flush = [&nodes, &line]() {
        if (!line.empty()) {
            NodeSpec run;
            run.kind = NodeKind::TextRun;
            run.text = utf8ToCodepoints(line);
            nodes.push_back(std::move(run));
        }
        line.clear();
    };
    for (size_t i = 0; i < utf8.size(); ++i) {
        const char c = utf8[i];
        if (c != '\n' && c != '\r') {
            line.push_back(c);
            continue;
        }
        // CRLF 是两个字符一个换行：吃掉紧随的 LF，否则会多出一个空段落。
        if (c == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n') ++i;
        flush();
        NodeSpec mark;
        mark.kind = NodeKind::ParagraphMark;
        nodes.push_back(std::move(mark));
    }
    flush();
    if (nodes.empty()) return "没有可粘贴的文本";
    if (!edit_.pasteNodes(nodes)) return "粘贴失败：落点不接受这段内容";
    return std::string();
}

bool EditorEngine::deleteSelection() { return edit_.deleteSelection(); }

std::string EditorEngine::pasteFragmentAndRender(const std::string& text) {
    const std::string err = pasteFragment(text);
    if (!err.empty()) return err;
    relayout();
    repaintAfterEdit();
    return std::string();
}

std::string EditorEngine::pastePlainTextAndRender(const std::string& utf8) {
    const std::string err = pastePlainText(utf8);
    if (!err.empty()) return err;
    relayout();
    repaintAfterEdit();
    return std::string();
}

bool EditorEngine::applyCharFormatAndRender(const CharFormat& patch) {
    if (!session_) return false;
    // 幂等：格式没变时命令层不提交，这里也不要重排 —— 撤销栈与画面都该一动不动。
    if (!edit_.applyCharFormat(patch)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::applyParaFormatAndRender(const ParaFormatPatch& patch) {
    if (!session_) return false;
    if (!edit_.applyParaFormat(patch)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

std::string EditorEngine::charFormatAtJson(bool effective) const {
    if (!session_) return "{}";
    if (effective) return effectiveFormatAtJson();
    // 键名与写入那条命令同一套，但尺寸给"磅数"而不是 Length 的文本形式 ——
    //   读与写对称，界面才不用解析两种表示。
    const CharFormat m = edit_.charFormatAtCaret();
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    if (m.bold) j["b"] = *m.bold;
    if (m.italic) j["i"] = *m.italic;
    if (m.underline) j["u"] = *m.underline;
    if (m.font_index) j["fi"] = *m.font_index;
    if (m.size && m.size->kind == LengthKind::Abs)
        j["sz"] = static_cast<double>(emuToPt(m.size->abs_emu));
    if (m.color) j["col"] = {m.color->r, m.color->g, m.color->b};
    return j.dump();
}

std::string EditorEngine::effectiveFormatAtJson() const {
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    const Range sel = selection();
    const std::vector<BlockSpec>& bs = blocks();
    if (bs.empty()) return j.dump();

    // 覆盖哪些 run：空选区就是落点那一处（取落点左边那个字，与"接着敲字沿用哪一份"
    //   同一个约定）；有选区就是整段区间覆盖到的每一个 run。
    std::vector<const FormatRun*> covered;
    std::vector<FormatRun> owned;  // effectiveRuns 是值返回，先接住再取地址
    std::vector<std::vector<FormatRun>> all_runs;
    int p_lo = 0;
    int p_hi = 0;
    uint32_t cp_lo = 0;
    uint32_t cp_hi = 0;
    if (sel.empty()) {
        const TextPos at = caret();
        if (at.para < 0 || at.para >= static_cast<int>(bs.size())) return j.dump();
        p_lo = p_hi = at.para;
        const uint32_t len = static_cast<uint32_t>(bs[static_cast<size_t>(at.para)].cp_text.size());
        const uint32_t cp = static_cast<uint32_t>(at.offset);
        cp_lo = cp > 0 ? cp - 1 : 0;
        cp_hi = std::min(cp_lo + 1, len);
        if (len == 0) cp_hi = 0;
    } else {
        TextPos a{-1, 0};
        TextPos b{-1, 0};
        if (!flowToTextPos(sel.begin(), &a) || !flowToTextPos(sel.end(), &b)) return j.dump();
        if (a.para < 0 || b.para < 0) return j.dump();
        p_lo = a.para;
        p_hi = b.para;
        cp_lo = static_cast<uint32_t>(a.offset);
        cp_hi = static_cast<uint32_t>(b.offset);
    }
    all_runs.reserve(static_cast<size_t>(p_hi - p_lo + 1));
    for (int p = p_lo; p <= p_hi && p < static_cast<int>(bs.size()); ++p) {
        const BlockSpec& blk = bs[static_cast<size_t>(p)];
        all_runs.push_back(blk.effectiveRuns());
        const std::vector<FormatRun>& runs = all_runs.back();
        if (runs.empty()) continue;
        const uint32_t len = static_cast<uint32_t>(blk.cp_text.size());
        const uint32_t lo = (p == p_lo) ? cp_lo : 0;
        const uint32_t hi = (p == p_hi) ? cp_hi : len;
        if (p == p_lo && p == p_hi && sel.empty()) {
            // 空选区：只要落点那一个字所在的 run。
            for (const FormatRun& r : runs) {
                if (r.cp_begin <= lo && (lo < r.cp_end || len == 0)) {
                    covered.push_back(&r);
                    break;
                }
            }
            if (covered.empty()) covered.push_back(&runs.back());
            continue;
        }
        if (hi <= lo) continue;  // 空段里没有可读的字
        for (const FormatRun& r : runs) {
            if (r.cp_end <= lo || r.cp_begin >= hi) continue;
            covered.push_back(&r);
        }
    }
    if (covered.empty()) {
        // 选区只落在段落标记上（这一段的字一个都没覆盖到）：报这一段的第一个 run ——
        //   与"在这一段里接着敲字会用哪一份"一致。
        for (const std::vector<FormatRun>& runs : all_runs) {
            if (!runs.empty()) {
                covered.push_back(&runs.front());
                break;
            }
        }
        if (covered.empty()) return j.dump();
    }

    // 逐项求交：选区里只要有一种值不同，这一项就不报（缺席 = 没有统一的值）。
    //   这正是字处理软件的行为 —— 跨出一种格式，那个按钮就灭掉；同一套规则左右对称，
    //   与"从哪一头开始划"无关。
    const FormatRun* f0 = covered.front();
    bool same_b = true;
    bool same_i = true;
    bool same_u = true;
    bool same_fi = true;
    bool same_sz = true;
    bool same_col = true;
    for (const FormatRun* r : covered) {
        if ((r->weight >= 600.0f) != (f0->weight >= 600.0f)) same_b = false;
        if (r->italic != f0->italic) same_i = false;
        if (r->underline != f0->underline) same_u = false;
        if (r->font_index != f0->font_index) same_fi = false;
        if (r->size_pt != f0->size_pt) same_sz = false;
        if (!(r->color == f0->color)) same_col = false;
    }
    // 加粗没有独立字段，它是可变字体的 wght 轴：写入那一侧 true 落成 700。
    //   600 及以上算粗（半粗也读作粗），这样"样式层设了 semi-bold"不会被报成不粗。
    if (same_b) j["b"] = f0->weight >= 600.0f;
    if (same_i) j["i"] = f0->italic;
    if (same_u) j["u"] = f0->underline;
    if (same_fi) j["fi"] = f0->font_index;
    if (same_sz) j["sz"] = static_cast<double>(f0->size_pt);
    if (same_col) j["col"] = {f0->color.r, f0->color.g, f0->color.b};
    return j.dump();
}

std::string EditorEngine::paraFormatAtJson() const {
    if (!session_) return "{}";
    const ParaFormatPatch p = edit_.paraFormatAtCaret();
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    if (p.font_index) j["fi"] = *p.font_index;
    if (p.size && p.size->kind == LengthKind::Abs)
        j["sz"] = static_cast<double>(emuToPt(p.size->abs_emu));
    if (p.line_factor) j["lf"] = *p.line_factor;
    if (p.space_after && p.space_after->kind == LengthKind::Abs)
        j["sa"] = static_cast<double>(emuToPt(p.space_after->abs_emu));
    if (p.page_break_before) j["pbb"] = *p.page_break_before;
    return j.dump();
}

bool EditorEngine::deleteSelectionAndRender() {
    if (!deleteSelection()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

const rev::Doc& EditorEngine::document() const {
    static const rev::Doc kEmpty;
    return session_ ? session_->doc() : kEmpty;
}

FlowVisibility EditorEngine::flowVisibility() const {
    // 隐藏控件的可见性口径与投影同一处取值：文档模式是设计态（隐藏的照常参与流，
    //   否则作者没法把它选中改回来），表单与只读是呈现态（整棵摘除、不占流位置）。
    return (mode_ == EditMode::Document) ? FlowVisibility::Design : FlowVisibility::Display;
}

void EditorEngine::rebuildFromTree(const rev::BlockDamage* dmg) {
    // 视图先摆好：下面这一段就要用位置解析（落基、控件导出），而它读的是视图。
    //   容器都是成员、不换对象，所以地址现在取和最后取一样。
    // 块流换了一份：上一轮那份"筛过括号"的副本已经对不上了，先作废。
    //   下一次排版会按新的名单重筛（见 syncInlineObjects），在那之前 blocks() 读全量那份。
    filter_active_ = false;
    refreshViews();
    edit_stats_.rebuild_path = 0;
    edit_stats_.rebuild_reject = 0;
    const FlowVisibility vis = flowVisibility();
    const auto t_compile0 = Clock::now();
    // 增量投影：blocks_full_ 既是上一轮的块流、也是这一轮的输出 —— 没变的块原地不动
    //   （不重算、不拷贝），只有损伤区间那一段被新物化的块替换。
    rev::Projection res = rev::project(document(), sheet_, rev::ViewParams{vis}, &blocks_full_,
                                       proj_hist_.keys.empty() ? nullptr : &proj_hist_, dmg);
    edit_stats_.rebuild_reused = res.reused;
    proj_hist_.style_version = res.style_version;
    proj_hist_.keys = std::move(res.keys);
    block_src_.clear();
    block_src_.reserve(proj_hist_.keys.size());
    for (const rev::BlockKey& k : proj_hist_.keys) block_src_.push_back(k.source);
    block_extents_ = std::move(res.extents);
    tables_ = std::move(res.tables);
    edit_stats_.rebuild_compile_ms = msSince(t_compile0);
    finishRebuildFromTree(vis, res.controls);
}

void EditorEngine::finishRebuildFromTree(FlowVisibility vis,
                                         const std::vector<rev::ControlSpan>& controls) {
    const auto t_index0 = Clock::now();
    // 流索引与块起点一起重建：两者是同一份文档的两种说法，分两次建就会出现
    //   "块按新树、流位置按旧树"的错配，而那种错配只表现为光标落偏一个字，很难查。
    //   块起点本身不再由这一侧推算 —— 投影走树时已经连同内容一起数出来了（见下面）。
    flow_.emplace(document(), vis);
    edit_stats_.rebuild_flow_ms = msSince(t_index0);
    const auto t_pos0 = Clock::now();

    // 光标与块起点是同一套坐标的两半，所以重建块起点时必须一起重算光标：光标存的是全局
    //   流位置，而这个下标只有配上 block_flow_ 才读得出"段与段内位置"。基一换，光标的数字
    //   没变、含义却变了 —— 前面某一段变长之后，它会静默漂到相邻位置。
    //   重算按"段内位置"落基而不是保留绝对下标：光标此刻的语义是"停在这一段内容的这个流
    //   位置上"，段落内容变了它该跟着文本走。编辑之后真正的跟随由事务的位置映射负责，
    //   这条只保证"基换过之后读数仍然自洽"。
    //   两个读数必须在覆盖 block_flow_ 之前取，assign 之后旧基就没了。
    //   选区落地之后 anchor 也要跟着落基：它现在是一个真位置，只落 focus 会把它收成空选区
    //   （症状是"Shift 扩展之后来一次重编译，选区就没了"）。
    const int rebase_para = caret_.caretPara();
    const int rebase_rel = caret_.caretRel();
    const int rebase_anchor_para = caret_.paraForFlow(caret_.selection().anchor);
    const int rebase_anchor_rel = caret_.relForFlow(caret_.selection().anchor);
    // 绝对位置在这里由局部量累加出来：投影给的 span（本块几位）与 gap（前面那道边界几位）。
    //   没有"第二遍算法"要调和，也就没有两处 max —— 表格的格边界是 gap=2 这个事实，
    //   而不是"两个读数取较大值"得出的结论。
    buildBlockFlow();
    // 新基写完了才落基（见上面那段说明）。两端一起落。
    caret_.rebaseSelection(rebase_anchor_para, rebase_anchor_rel, rebase_para, rebase_rel);

    edit_stats_.rebuild_flowpos_ms = msSince(t_pos0);
    const auto t_copy0 = Clock::now();
    edit_stats_.rebuild_copy_ms = msSince(t_copy0);
    const auto t_ctl0 = Clock::now();
    exportControls(controls);
    edit_stats_.rebuild_controls_ms = msSince(t_ctl0);

    // 文档规模在产生处发布：它由这一轮的块流与控件集合决定，与"有没有呈现过"无关。
    //   挂在呈现上时，撤销之后不呈现就读到旧规模 —— 而这个读数正是宿主判断滚动条与
    //   页数的输入。段数按全量块流给：聚焦档下可见块更少，但它不是"文档有多少段"。
    edit_stats_.total_paragraphs = static_cast<int>(blocks_full_.size());
    edit_stats_.doc_codepoints = documentCodepoints();
    edit_stats_.control_count = controls_.size();
}

void EditorEngine::buildBlockFlow() {
    const size_t n = block_extents_.size();
    block_flow_.assign(n, 0);
    block_flow_span_.assign(n, 0);
    // gap 是"这一块之前那道界占几位"，所以先跨过去再记起点：普通段界一位（段落标记），
    //   表格格边界两位（出上一格 + 进这一格），正文第一段前面则是节边界。
    FlowPos cursor = 0;
    for (size_t i = 0; i < n; ++i) {
        cursor += block_extents_[i].gap;
        block_flow_[i] = cursor;
        block_flow_span_[i] = block_extents_[i].span;
        cursor += block_extents_[i].span;
    }
}

int EditorEngine::caretSide() const { return caret_.caretSide(); }

int EditorEngine::caretFieldHint() const { return caret_.caretFieldHint(); }

FlowPos EditorEngine::flowPosAtSide(int para, size_t cp_offset, int side) const {
    return caret_.flowPosAtSide(para, cp_offset, side);
}

FlowPos EditorEngine::flowPosAt(int para, size_t cp_offset, int owner_hint) const {
    return caret_.flowPosAt(para, cp_offset, owner_hint);
}

bool EditorEngine::flowToTextPos(FlowPos flow, TextPos* out) const {
    return caret_.flowToTextPos(flow, out);
}

FlowPos EditorEngine::flowSize() const { return flow_ ? flow_->size() : 0; }

FlowPos EditorEngine::blockFlowStart(int para) const {
    if (para < 0 || para >= static_cast<int>(block_flow_.size())) return 0;
    return block_flow_[static_cast<size_t>(para)];
}

int EditorEngine::blockFlowSpan(int para) const {
    if (para < 0 || para >= static_cast<int>(block_flow_span_.size())) return 0;
    return block_flow_span_[static_cast<size_t>(para)];
}

/**
 * 从投影给出的跨度建控件集合。
 *
 * 跨度是投影走树时记下的（起止块号与段内码点），业务标识与锁定态就在节点载荷里 ——
 * 所以这一层不再扫块流的对象表，也不再对每个控件 findNode 一次。少的不只是一遍线性查找：
 * 过去区间算两次（产物一次、这里一次），两份一旦分叉，界面上只表现为"某个字段锁不住"或
 * "括号画在别处"，而两份产物各自看都"对"。
 */
void EditorEngine::exportControls(const std::vector<rev::ControlSpan>& spans) {
    controls_.clear();
    owner_index_.clear();
    control_node_ids_.clear();

    // 插入顺序就是对外下标顺序（controls()[i]、setControlProp(i, ...)）。跨度按文档顺序给
    //   （见 rev::ControlSpan 的说明），所以这里照原序建，不重排 —— 换一种排法就等于把
    //   "第几个字段"换成"按区间排第几个"，而两者在嵌套与零宽字段上不是同一个答案。
    for (const rev::ControlSpan& s : spans) {
        if (s.node == nullptr || s.block < 0 || s.end_block < 0) continue;
        Control c;
        c.range.start = TextPos{s.block, static_cast<size_t>(s.cp)};
        c.range.end = TextPos{s.end_block, static_cast<size_t>(s.end_cp)};
        // 标签刻意不填：它是内容。标识、种类与锁定态是节点上的数据。
        if (const auto* spec = std::get_if<ControlSpec>(&s.node->payload)) {
            c.id = spec->key;
            c.kind = controlKindFromBusinessType(spec->controlType);
            c.read_only = spec->readonly;
        }
        const int index = controls_.add(c);
        owner_index_[static_cast<int>(s.node->id)] = index;
        control_node_ids_.push_back(s.node->id);
    }
    controls_.normalize();
}

/**
 * 把两份视图摆给它们各自的类：位置解析读 PositionView，命令层读 EditView。
 *
 * 视图里存的是这些容器的地址，而容器都是引擎的成员、不换对象，所以摆一次之后地址一直有效；
 * 四个收口点（init / setDocument / 重建之后 / 重排之后）各摆一次是保守做法。文档本身是
 * session_ 的成员，换修订只换内容、不换对象 —— 地址同样稳定。
 */
void EditorEngine::refreshViews() {
    PositionView v;
    v.blocks = &blocks();
    v.blocks_full = &blocks_full_;
    v.block_flow = &block_flow_;
    v.block_flow_span = &block_flow_span_;
    v.flow = &flow_;
    v.controls = &controls_;
    v.control_node_ids = &control_node_ids_;
    v.owner_index = &owner_index_;
    v.layout = &layout_;
    caret_.setView(v);
    caret_.setCaretWidthPt(cfg_.caret_width_pt);

    EditView e;
    e.host = this;
    e.doc = &document();
    e.caret = &caret_;
    e.flow = &flow_;
    e.blocks = &blocks();
    e.blocks_full = &blocks_full_;
    e.block_flow = &block_flow_;
    e.block_flow_span = &block_flow_span_;
    e.block_src = &block_src_;
    e.tables = &tables_;
    e.controls = &controls_;
    e.control_node_ids = &control_node_ids_;
    e.mode = &mode_;
    e.tab_spaces = &cfg_.tab_spaces;
    e.blocked_edits = &blocked_edits_;
    edit_.setView(e);
}

/**
 * 撤销上限：配置里的 0 表示"不限制"，而会话层的 0 表示"不留历史"。两者不能直接对应，
 * 所以在这里换成一个足够大的数 —— 0 的语义属于对外约定，不该为了内部实现改掉。
 */
int EditorEngine::undoLimit() const { return cfg_.undo_limit > 0 ? cfg_.undo_limit : 1000000; }

bool EditorEngine::commitSteps(std::vector<Step> steps, int dirty_para, Range selection_before,
                               Range selection_after, rev::MergeRun merge) {
    if (!loaded_ || !session_) return false;
    // 旧修订与旧产物一一对应：投影复用要按它们的差算损伤区间。Doc 是不可变的，
    //   拷贝只是一个引用计数（未改的子树与新的那份共享）。
    const rev::Doc before = session_->doc();
    const FlowVisibility vis = flowVisibility();
    const rev::Applied r = session_->applySteps(steps, vis, merge);
    if (!r.ok) return false;
    if (!r.changed) return true;  // 什么都没改：不占撤销栈，也不白跑一次重建
    const rev::Damage dmg = rev::damage(before, session_->doc(), vis);
    recordSelection(selection_before, selection_after);
    rebuildFromTree(&dmg.range);
    // 起点取两者更早的那个：dirty_para 是命令层按编辑计划算的落点，damage.lo 是内容上
    //   第一处真实差异 —— 计划可能比差异更早（保守），反过来不该发生，取小就是安全的。
    //   右端只有损伤区间知道，交给后缀复用当"被换掉几段"。
    markLayoutDirtyRange(std::min(dirty_para, dmg.range.lo), dmg.range.hi);
    return true;
}

void EditorEngine::recordSelection(Range before, Range after) {
    // 分支之后原先那条"未来"已经不成立：截断到当前位置再压新的。
    if (sel_pos_ < sel_history_.size()) sel_history_.resize(sel_pos_);
    sel_history_.push_back(SelectionRecord{before, after});
    ++sel_pos_;
    const int limit = undoLimit();
    if (static_cast<int>(sel_history_.size()) > limit) {
        const size_t drop = sel_history_.size() - static_cast<size_t>(limit);
        sel_history_.erase(sel_history_.begin(), sel_history_.begin() + drop);
        sel_pos_ = sel_pos_ >= drop ? sel_pos_ - drop : 0;
    }
}

bool EditorEngine::undo() {
    if (!loaded_ || !session_) return false;
    const int plan_before = bracketPlanId();
    // 撤销/重做不改"选中的是哪张图"：改尺寸那一步撤销回去，图仍该是选中的。恢复选区
    //   那一步会清掉图片选中，所以这里先记下、最后一处放回去。
    const NodeId image_before = image_sel_;
    clearImageResizePreview();
    const rev::Doc before = session_->doc();
    if (!session_->undo()) return false;
    const rev::Damage dmg = rev::damage(before, session_->doc(), flowVisibility());
    rebuildFromTree(&dmg.range);
    // 撤销改到哪一段由损伤区间回答：两份修订一减就是它，不必整篇重排。
    markLayoutDirtyForDamage(dmg.range);
    // 恢复的是整个区间，不是只看焦点端：撤销前那一对快照本来就是 Range。
    if (sel_pos_ > 0 && sel_pos_ <= sel_history_.size()) {
        --sel_pos_;
        setSelection(sel_history_[sel_pos_].before);
    }
    restoreImageSelection(image_before);
    // 撤销可能把"有没有控件"整体改掉（撤销删掉最后一个控件、或把只读控件撤回来），
    //   那样占位集合整个翻转，而损伤区间只报块内容 —— 计划变了就整体重排。
    if (bracketPlanId() != plan_before) markLayoutDirtyForBrackets();
    return true;
}

bool EditorEngine::redo() {
    if (!loaded_ || !session_) return false;
    const int plan_before = bracketPlanId();
    const NodeId image_before = image_sel_;
    clearImageResizePreview();
    const rev::Doc before = session_->doc();
    if (!session_->redo()) return false;
    const rev::Damage dmg = rev::damage(before, session_->doc(), flowVisibility());
    rebuildFromTree(&dmg.range);
    markLayoutDirtyForDamage(dmg.range);
    if (sel_pos_ < sel_history_.size()) {
        setSelection(sel_history_[sel_pos_].after);
        ++sel_pos_;
    }
    restoreImageSelection(image_before);
    if (bracketPlanId() != plan_before) markLayoutDirtyForBrackets();
    return true;
}

bool EditorEngine::undoAndRender() {
    if (!undo()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

int EditorEngine::undoDepth() const { return session_ ? session_->undoDepth() : 0; }

bool EditorEngine::redoAndRender() {
    if (!redo()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

size_t EditorEngine::frameBytes() const {
    // 尺寸是算出来的，不是"拼出来之后数一数"。这一条是给 statsJson 用的：它每一份状态
    //   都带这个数，而拼一次窗口导出图（2000 段 / 6 页窗口在 dpr 2 下实测 83 MB）比
    //   整个呈现还贵。把"问大小"与"要像素"分开，问大小就只剩两次乘法。
    return static_cast<size_t>(frameWidth()) * static_cast<size_t>(frameHeight()) * 4u;
}

const uint8_t* EditorEngine::frame() const {
    ensureFrameExport();
    return frame_export_.data();
}

int EditorEngine::frameHeight() const {
    const int step = pagePxHeight() + pageGapPx();
    return std::max(1, win_page_count_) * step;
}

size_t EditorEngine::documentCodepoints() const {
    size_t n = 0;
    // 按全量块流数：这是文档的码点数，不是"当前可见的码点数"。聚焦档下 blocks()
    //   是一份筛过的副本，用它会让同一个文档在有、无聚焦时给出两个规模。
    for (const BlockSpec& b : blocks_full_) n += b.cp_text.size();
    return n;
}

/**
 * 光标：位置设置、字素簇吸附与光标矩形查询。
 */

bool EditorEngine::caretRectPt(Rect* out) const { return caret_.caretRectPt(out); }

/**
 * 光标矩形（页内像素）。窗口偏移由宿主按 (页号 x 步距) 自己加 —— 引擎这一侧只认页内坐标，
 * 于是同一个光标在窗口挪动前后给出的是同一个矩形。旧版在这里加 off_y，使这个值随
 * "窗口首页落在哪个槽"变化，宿主只能再减回去（两处口径必须配对，改一处就整体错位）。
 */
Rect EditorEngine::caretRectPx(int* page) const {
    int p = 0;
    Rect r{};
    if (page != nullptr) *page = -1;
    if (!caret_.caretRectFor(caret(), &p, &r)) return Rect{};
    const float zoom = cfg_.zoom;
    Rect px;
    px.x = std::floor(r.x * zoom);
    px.y = std::floor(r.y * zoom);
    px.w = std::max(1.0f, std::ceil(r.w * zoom));
    px.h = std::max(1.0f, std::ceil(r.h * zoom));
    if (page != nullptr) *page = p;
    return px;
}

bool EditorEngine::caretVertical() const { return caret_.caretVertical(); }

bool EditorEngine::caretBlockReversed() const { return caret_.caretBlockReversed(); }

std::vector<EditorEngine::PageRect> EditorEngine::selectionRects() const {
    return caret_.selectionRects();
}

/**
 * 当前视口窗口内的选区矩形（视口没钉过时退化成整篇）。
 */
std::vector<EditorEngine::PageRect> EditorEngine::selectionRectsVisible() const {
    if (vp_count_ <= 0) return selectionRects();
    return selectionRectsIn(vp_first_page_, vp_count_);
}

std::vector<EditorEngine::PageRect> EditorEngine::selectionRectsIn(int first_page,
                                                                   int count) const {
    return caret_.selectionRectsIn(first_page, count);
}

/**
 * 选区矩形（页内像素坐标）。与 caretRectPx 同一套换算（乘 zoom），但不加任何窗口偏移 ——
 * 页号随矩形一起给出，落点由读的一方按 (页号 x 步距) 加。
 * 宽高为 0 的（空行的那个插入点）在这里丢掉 —— 画不出来。
 */
std::vector<PageRect> EditorEngine::selectionRectsPx() const { return selectionRectsPxIn(0, 0); }

std::vector<PageRect> EditorEngine::selectionRectsPxIn(int first_page, int count) const {
    std::vector<PageRect> out;
    const std::vector<PageRect> prs = selectionRectsIn(first_page, count);
    if (prs.empty()) return out;
    const float zoom = cfg_.zoom;
    out.reserve(prs.size());
    for (const PageRect& pr : prs) {
        PageRect px;
        px.page = pr.page;
        px.r_pt.x = std::floor(pr.r_pt.x * zoom);
        px.r_pt.y = std::floor(pr.r_pt.y * zoom);
        px.r_pt.w = std::ceil(pr.r_pt.w * zoom);
        px.r_pt.h = std::ceil(pr.r_pt.h * zoom);
        if (px.r_pt.w <= 0.0f || px.r_pt.h <= 0.0f) continue;
        out.push_back(px);
    }
    return out;
}

TextPos EditorEngine::caret() const { return caret_.caret(); }

void EditorEngine::setCaretByText(TextPos p) { caret_.setCaretByText(p); }

/**
 * 把"文本坐标 + 侧别"投影成 rel 并摆好，含重排。
 *
 * 为什么摆两次：聚焦档下"显示哪些括号"由光标位置决定（见 bracketedControls），
 * 而 rel 是按对象表算出来的 —— 先摆一次让重排看到光标的新位置，重排换表之后再摆一次，
 * rel 才是按最终那张表算的。只摆一次的话，rel 描述的是重排前那一批格子。
 * 两次之间输入（文本坐标 + 侧别）不变，所以收敛：第二次不会再触发重排。
 *
 * side < 0 = 没有侧别信息，按"最内层含端点"投影一次（见 setCaretByText）。
 */
void EditorEngine::projectCaret(int para, size_t offset, int side, bool extend, int line_aff) {
    // 光标一动就说明用户在选文字，图片选区（对象选区）随之作废：两套选区同时存在，
    //   "按删除键删的是哪一段"就没有答案。
    image_sel_ = kInvalidNodeId;
    const int plan_before = bracketPlanId();
    caret_.placeCaret(para, offset, side, extend, line_aff);
    relayoutIfBracketPlanChanged(plan_before);
    caret_.placeCaret(para, offset, side, extend, line_aff);
    // 落点换了，粒度跟着重算 —— 扩展跨过格边界就整格，回到同一格就退回文字。
    //   定位（非扩展）不吸附：一次点击落在字上是文字光标，清空由 placeCaret 自己做。
    if (extend) syncCellSelection();
}

/**
 * 拖选 / Shift 扩展之后重算选区粒度（见头文件）。
 *
 * 用 rawSelection：整格吸附过的端点会一直停在格边界上，拿它去判"还在不在同一格"
 * 就永远判得出来是同一格 —— 用户把鼠标拖回来时选区不会退回文字那一支。
 */
void EditorEngine::syncCellSelection() {
    const Range sel = caret_.rawSelection();
    if (sel.empty()) {
        caret_.clearCellRange();
        return;
    }
    const LayoutTable* ta = nullptr;
    const LayoutTable* tf = nullptr;
    const LayoutCell* ca = layoutCellAt(caret_.paraForFlow(sel.anchor), &ta);
    const LayoutCell* cf = layoutCellAt(caret_.paraForFlow(sel.focus), &tf);
    if (ca == nullptr || cf == nullptr || ta != tf) {
        caret_.clearCellRange();
        return;
    }
    // 同一个格：两端区间一模一样（格的区间本身就是这两个端点）。
    if (ca == cf) {
        caret_.clearCellRange();
        return;
    }
    // 矩形包络：行取两端格覆盖的行，列取两端格覆盖的列。合并格（跨度大于一）按它盖住的
    //   整块算 —— 跨进合并格的行或列都算选到它。
    const int r0 = std::min(ca->row, cf->row);
    const int r1 = std::max(ca->row + ca->row_span - 1, cf->row + cf->row_span - 1);
    const int c0 = std::min(ca->cell, cf->cell);
    const int c1 = std::max(ca->cell + ca->col_span - 1, cf->cell + cf->col_span - 1);
    // 按块区间收：一格的身份是它占的段的区间，不是它的流区间 —— 空格的内容长度为 0，
    //   流区间退化成点，与下一格的第一段用同一个流位置，按流位置挑格会挑错。
    std::vector<CaretResolver::CellBlocks> cells;
    FlowPos caret_flow = -1;
    for (const LayoutCell& c : ta->cells) {
        const int cr1 = c.row + c.row_span - 1;
        const int cc1 = c.cell + c.col_span - 1;
        if (cr1 < r0 || c.row > r1 || cc1 < c0 || c.cell > c1) continue;
        if (c.block_count <= 0) continue;
        const int last_b = c.first_block + c.block_count - 1;
        if (c.first_block < 0 || last_b >= static_cast<int>(block_flow_.size()) ||
            last_b >= static_cast<int>(block_flow_span_.size()))
            continue;
        cells.push_back(CaretResolver::CellBlocks{c.first_block, c.block_count});
        if (&c == cf)
            caret_flow = block_flow_[static_cast<size_t>(last_b)] +
                         block_flow_span_[static_cast<size_t>(last_b)];
    }
    // 少于两格说明两端的格在矩形里对不上（表格结构刚变过），眼下按文字选区算更安全。
    if (cells.size() < 2) {
        caret_.clearCellRange();
        return;
    }
    // 插入点钉在焦点那一格的末尾（见 setCellBlocks 的 caret_flow）。
    caret_.setCellBlocks(std::move(cells), caret_flow);
}

void EditorEngine::setCaret(int para, size_t offset) {
    // 这个入口是程序化定位（宿主恢复光标、测试摆位），不是用户的一次点击 —— 落点限制
    //   不在这里生效。限制在"用户真的能点到哪"的那三个入口上：点击（setCaretAt）、
    //   方向键（moveCaretBy）、Home/End 与 Tab。编辑被拒仍由 policyAt 兜住，
    //   所以"摆到集合外再打字"得到的仍然是拒绝，不会静默越界。
    projectCaret(para, offset, -1);
}

/**
 * 带侧别的定位（点画布）。
 *
 * 为什么偏移本身不够：同一个偏移上可能压着好几枚括号，"控件之前 / 进入控件 / 内容末尾 /
 * 控件之后"是互不相同的插入点。只存偏移，落点只能按"最内层、同深度取下标小者"来定，
 * 于是点后一个字段的开头时字会落进前一个字段；前一个字段只读时更直接 —— 编辑被拒，
 * 看起来就像点不进去。相邻的空字段（][][ ）更是整段位置都点不进去。
 *
 * 侧别就是被丢掉的那个分量，浏览器点画布时量得到：占位簇各占一枚，数格子即可。
 * 宿主把 hitTestPt 给的侧别原样带回来，这里与偏移合成 rel（唯一真源）。
 */
void EditorEngine::setCaretAtSide(int para, size_t offset, int side) {
    placeCaretFromClick(para, offset, side, /*line_aff=*/-1);
}

/**
 * 一次落点的唯一实现：投影 → 夹到可插入位置 → 记下"拖选可以从这里起手"。
 *
 * 这是"用户点到哪"，于是它受限（见 confineCaret）；拖选（setFocusAtSide）不受限 ——
 *   选区与光标是两件事，选区要能盖住只读区。按下的那一点若本身就在屏障里，先记下来，
 *   等拖选第一次搬焦点端时放到锚端上；夹这一步会清掉待定值，所以顺序不能反。
 */
void EditorEngine::placeCaretFromClick(int para, size_t offset, int side, int line_aff) {
    const FlowPos before = caretFlowPos();
    projectCaret(para, offset, side, /*extend=*/false, line_aff);
    const FlowPos pressed = caretFlowPos();
    const bool in_barrier = barrierAtFlow(pressed);
    confineCaret(before, CaretConfine::Place);
    if (in_barrier) drag_anchor_ = pressed;
}

/**
 * 拖选的移动端：只搬 focus，anchor 留在按下点。
 *
 * projectCaret 的 extend 形参本来就有（键盘的 Shift 扩展族走它），拖选要的是同一个动作，
 * 只是位置来自命中测试而不是方向键。所以这里没有第二套几何：落点、吸附、侧别全部复用。
 */
void EditorEngine::setFocusAtSide(int para, size_t offset, int side) {
    applyDragAnchor();
    projectCaret(para, offset, side, /*extend=*/true);
}

/**
 * 带行别的定位与拖选（点击落点）。
 *
 * 与上一对只差行别：折行处那个位置属于两行，点下一行行首时必须说清是哪一行，
 * 否则光标画到上一行行尾（见 caretLineAffinity）。
 */
void EditorEngine::setCaretAtSideLine(int para, size_t offset, int side, int line_aff) {
    placeCaretFromClick(para, offset, side, line_aff);
}

void EditorEngine::setFocusAtSideLine(int para, size_t offset, int side, int line_aff) {
    applyDragAnchor();
    projectCaret(para, offset, side, /*extend=*/true, line_aff);
}

/**
 * 拖选第一次搬焦点端时，把锚端放回按下那一刻的位置。
 *
 * 锚端与焦点端是两件事：焦点端夹不进控件，锚端可以停在控件里 —— 从选择类的标签上
 *   按下往右拖，选区就该从那个字开始，而不是从控件边界开始。
 * 待定值只在按下之后的第一次搬运用一次；它已经不在屏障里（内容被改短了）就丢掉。
 */
void EditorEngine::applyDragAnchor() {
    if (drag_anchor_ < 0) return;
    const FlowPos anchor = drag_anchor_;
    drag_anchor_ = -1;
    if (!barrierAtFlow(anchor)) return;
    caret_.setAnchorAtFlow(anchor);
}

/**
 * 宿主与探针的说话方式：它们只知道控件下标，分不出共享边界的两侧，于是用
 * LineBox::kInlineOffsetOuter 说"贴外侧"。这里把它翻成侧别。
 */
void EditorEngine::setCaretAt(int para, size_t offset, int field) {
    if (blocks().empty()) {
        setCaretAtSide(para, offset, -1);
        return;
    }
    int p = para;
    if (p < 0) p = 0;
    if (p >= static_cast<int>(blocks().size())) p = static_cast<int>(blocks().size()) - 1;
    const size_t at = caret_.snapToCluster(p, offset);
    setCaretAtSide(p, at, caret_.sideFromHint(p, at, field));
}

bool EditorEngine::setCaretAtFlow(FlowPos flow) { return caret_.setCaretAtFlow(flow); }

bool EditorEngine::setSelection(Range r) {
    image_sel_ = kInvalidNodeId;
    return caret_.setSelection(r);
}

void EditorEngine::clearSelection() {
    image_sel_ = kInvalidNodeId;
    caret_.clearSelection();
}

bool EditorEngine::selectAll() {
    image_sel_ = kInvalidNodeId;
    return caret_.selectAll();
}

/**
 * 按段号选一格：段的身份比流区间可靠 —— 空格的内容长度为 0，流区间是一个点，
 * 与下一格的第一段共用同一个流位置（见 CaretResolver::CellBlocks）。
 */
bool EditorEngine::selectCellAtPara(int para) {
    const LayoutCell* c = layoutCellAt(para, nullptr);
    if (c == nullptr || c->block_count <= 0) return false;
    const int first = c->first_block;
    const int last_b = first + c->block_count - 1;
    if (first < 0 || last_b >= static_cast<int>(block_flow_.size()) ||
        last_b >= static_cast<int>(block_flow_span_.size()))
        return false;
    // 插入点钉在这一格的末尾（整格选区的光标不跟鼠标走）。
    const FlowPos prev_lo = block_flow_[static_cast<size_t>(first)];
    const FlowPos caret_flow =
        block_flow_[static_cast<size_t>(last_b)] + block_flow_span_[static_cast<size_t>(last_b)];
    // 离散动作（单击边距带、三击）要把两端也落到这一格上：拖选那条路不能这么做
    //   （锚点要留着，往回拖才退得回文字选区），所以吸附由这里做而不是 setCellBlocks。
    if (!caret_.setSelection(Range{prev_lo, caret_flow})) return false;
    return caret_.setCellBlocks({CaretResolver::CellBlocks{first, c->block_count}}, caret_flow);
}

rev::EditCheck EditorEngine::policyAt(TextPos p) const { return edit_.policyAt(p); }

rev::EditCheck EditorEngine::policyErase(TextPos a, TextPos b) const {
    return edit_.policyErase(a, b);
}

/**
 * 光标此刻的流位置（派生）。整格选区时它是钉住的那一格末尾，与 caret() 同源。
 */
FlowPos EditorEngine::caretFlowPos() const {
    const TextPos p = caret();
    return flowPosAtSide(p.para, p.offset, caretSide());
}

bool EditorEngine::controlContentSpan(int index, FlowPos* lo, FlowPos* hi) const {
    if (!flow_.has_value()) return false;
    if (index < 0 || index >= static_cast<int>(control_node_ids_.size())) return false;
    return controlContentSpanOf(*flow_, control_node_ids_[static_cast<size_t>(index)], lo, hi);
}

int EditorEngine::innermostControlAt(FlowPos f) const {
    if (!flow_.has_value()) return -1;
    return innermostControlAtFlow(*flow_, control_node_ids_, f);
}

bool EditorEngine::barrierAtFlow(FlowPos f) const {
    FlowPos first = 0;
    FlowPos end = 0;
    return barrierSpanAt(f, &first, &end);
}

bool EditorEngine::barrierSpanAt(FlowPos f, FlowPos* first, FlowPos* end) const {
    for (int i = 0; i < static_cast<int>(control_node_ids_.size()); ++i) {
        FlowPos a = 0;
        FlowPos b = 0;
        if (!controlContentSpan(i, &a, &b)) continue;
        if (f < a || f > b) continue;
        const rev::Node* n = rev::findNode(document(), control_node_ids_[static_cast<size_t>(i)]);
        if (n == nullptr) continue;
        const auto* spec = std::get_if<ControlSpec>(&n->payload);
        if (spec == nullptr) continue;
        const ChoiceDetail* choice = choiceOf(*spec);
        if (choice == nullptr || !choice->caret_barrier) continue;
        if (first != nullptr) *first = a - 1;
        if (end != nullptr) *end = b + 1;
        return true;
    }
    return false;
}

bool EditorEngine::insertableAtFlow(FlowPos f) const {
    // 屏障先于模式：选择类的内容三档里都不是插入点（它的"标签"不是可写正文）。
    if (barrierAtFlow(f)) return false;
    if (mode_ == EditMode::Document) return true;
    if (mode_ != EditMode::Form) return false;  // 只读（以及将来新增的模式）：一个都不认
    const int inner = innermostControlAt(f);
    if (inner < 0) return false;
    // 可编辑性是交集：把光标包住的每一层都不能是只读的。
    for (int k = inner; k >= 0; k = controls_.at(k).parent) {
        if (controls_.at(k).read_only) return false;
    }
    return true;
}

/**
 * 光标此刻是不是一个可插入位置 —— 它决定画不画那根竖线。
 *
 * 选区与光标是两件事：选区可以盖住只读区（核对上文、复制），光标不能停在打不进字的地方。
 *   表单模式只认可编辑集合内的位置（含"只读控件里嵌可编辑控件"那一档交集），只读模式
 *   一个都不认。位置本身不被改写，见 confineCaret。
 *
 * 判据不参数化任何块流：空文档的 caretRectPx 本来就给 0 宽，画不出来 —— 而块流有两份
 *   （blocks_full_ 与只筛过括号的 blocks_），从其中一份判"文档有没有内容"会在括号不显示时
 *   把光标整条判掉（见 TRAPS 228）。
 */
bool EditorEngine::caretInsertable() const {
    const FlowPos f = caretFlowPos();
    if (f < 0) return false;
    return insertableAtFlow(f);
}

/**
 * 这一拍到底画不画光标（相位、可插入位置、图片选区三件事的交集）。
 *
 * 图片选区是对象选区，与文本光标互斥：两样同时画出来时，光标那条竖线正落在图的右边线上，
 *   正好压住右中把手 —— 那一段既看不清也点不准。
 */
bool EditorEngine::caretShown() const {
    return caret_visible_ && image_sel_ == kInvalidNodeId && caretInsertable();
}

/**
 * 把光标收进可插入位置（表单模式的落点规则）。
 *
 * 目标落在控件之外时，夹回"移动之前所在控件"的内容区间 —— 视觉上就是"停在字段边界上"，
 *   而不是把光标摆到一个打不进字的地方去。夹不进去（移位之前就不在任何字段里）就整下不动。
 *
 * 程序化定位（setCaret / setCaretAt）不经过这里：宿主与探针要能把光标摆到任意位置去核对
 *   策略，受限的是用户真的能点到哪、按到哪。
 *
 * @param before 这一步移动之前的流位置
 * @param why 这一步是落点还是移动（只影响被屏障挡住时贴哪一侧）
 * @return true = 光标位置真的变了
 */
bool EditorEngine::confineCaret(FlowPos before, CaretConfine why) {
    // 任何一次光标写入都让拖选的待定锚点作废；落点那一路在夹完之前重新记（见 setCaretAtSideLine）。
    drag_anchor_ = -1;
    const FlowPos now = caretFlowPos();
    if (mode_ != EditMode::Form) {
        // 文档与只读模式只挡屏障：目标落进选择类的内容里时跳到块外。
        //   移动贴"来的那一侧"的对面 —— 贴回原侧会让方向键在控件边上原地不动，永远过不去。
        //   落点贴离点击处最近的那一侧 —— 按来向取对面会让同一处点击在控件左右来回跳。
        FlowPos first = 0;
        FlowPos end = 0;
        if (!barrierSpanAt(now, &first, &end)) return now != before;
        FlowPos target = end;
        if (why == CaretConfine::Place) {
            target = ((now - first) * 2 <= (end - first)) ? first : end;
        } else if (before >= end) {
            target = first;
        } else if (before <= first) {
            target = end;
        } else {
            target = (now < before) ? first : end;
        }
        if (!caret_.setCaretAtFlow(target)) return false;
        return target != before;
    }
    if (caretInsertable()) return now != before;

    FlowPos lo = 0;
    FlowPos hi = 0;
    bool found = false;
    for (int i = 0; i < controls_.size(); ++i) {
        FlowPos a = 0;
        FlowPos b = 0;
        if (!controlContentSpan(i, &a, &b)) continue;
        if (before < a || before > b) continue;
        // 嵌套时取最内层（起点最大者，同起点取跨度短的）：夹进外层会把光标带到两层之间的
        //   空档里，而那个空档下一格还是走不进去。
        if (!found || a > lo || (a == lo && (b - a) < (hi - lo))) {
            lo = a;
            hi = b;
            found = true;
        }
    }
    if (!found) {
        // 这一步之前就不在任何控件里（停在正文）：整下不动 —— 表单模式下正文没有插入点，
        //   方向键不该把它带进去。
        if (now == before) return false;
        if (!caret_.setCaretAtFlow(before)) return false;
        return false;
    }
    FlowPos target = now;
    if (target < lo) target = lo;
    if (target > hi) target = hi;
    // 夹出来的落点仍可能不可插入：区间中间嵌着一层只读控件时，夹回外层区间还是落在只读的
    //   内层里。那种情形整下不动 —— 一步之前的那个位置一定是可插入的。
    if (!insertableAtFlow(target)) target = before;
    if (!caret_.setCaretAtFlow(target)) return false;
    return target != before;
}

bool EditorEngine::moveCaretBy(int dcp, bool extend) {
    // 移动本身在解析器里；"换占位集合要重排"归引擎 —— 聚焦档下显示哪些括号由光标位置定。
    const int plan_before = bracketPlanId();
    const FlowPos before = caretFlowPos();
    if (!caret_.moveCaretBy(dcp, extend)) return false;
    // 扩展选区不受落点限制（选区可以盖住只读区），单步移动要收进可插入位置。
    const bool moved = extend ? true : confineCaret(before);
    relayoutIfBracketPlanChanged(plan_before);
    return moved;
}

bool EditorEngine::moveCaretToLineStart(bool extend) {
    const int plan_before = bracketPlanId();
    const FlowPos before = caretFlowPos();
    if (!caret_.moveCaretToLineStart(extend)) return false;
    if (!extend) confineCaret(before);
    relayoutIfBracketPlanChanged(plan_before);
    // 行首/行尾是 (偏移, 侧别) 投影出来的，投影要吸附簇 ⇒ 重排换了簇表就得再投影一次。
    caret_.reprojectCaret(extend);
    return true;
}

bool EditorEngine::moveCaretToLineEnd(bool extend) {
    const int plan_before = bracketPlanId();
    const FlowPos before = caretFlowPos();
    if (!caret_.moveCaretToLineEnd(extend)) return false;
    if (!extend) confineCaret(before);
    relayoutIfBracketPlanChanged(plan_before);
    caret_.reprojectCaret(extend);
    return true;
}

bool EditorEngine::moveCaretToDocStart(bool extend) {
    const int plan_before = bracketPlanId();
    const FlowPos before = caretFlowPos();
    if (!caret_.moveCaretToDocStart(extend)) return false;
    if (!extend) confineCaret(before);
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretToDocEnd(bool extend) {
    const int plan_before = bracketPlanId();
    const FlowPos before = caretFlowPos();
    if (!caret_.moveCaretToDocEnd(extend)) return false;
    if (!extend) confineCaret(before);
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretVertical(int dir, bool extend) {
    const int plan_before = bracketPlanId();
    const FlowPos before = caretFlowPos();
    if (!caret_.moveCaretVertical(dir, extend)) return false;
    if (!extend) confineCaret(before);
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

void EditorEngine::showCaret(bool on) { caret_visible_ = on; }

bool EditorEngine::insertCodepoints(int para, size_t offset, const std::vector<uint32_t>& cps) {
    return edit_.insertCodepoints(para, offset, cps);
}

bool EditorEngine::insertCodepoint(int para, size_t offset, uint32_t cp) {
    return edit_.insertCodepoint(para, offset, cp);
}

bool EditorEngine::insertTextUtf8(int para, size_t offset, const std::string& utf8) {
    return edit_.insertTextUtf8(para, offset, utf8);
}

bool EditorEngine::insertTextAndRender(int para, size_t offset, const std::string& utf8) {
    if (!insertTextUtf8(para, offset, utf8)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::typeTextUtf8(const std::string& utf8) { return edit_.typeTextUtf8(utf8); }

bool EditorEngine::backspace() { return edit_.backspace(); }

bool EditorEngine::deleteForward() { return edit_.deleteForward(); }

bool EditorEngine::splitParagraph() { return edit_.splitParagraph(); }

bool EditorEngine::mergeWithPreviousParagraph() { return edit_.mergeWithPreviousParagraph(); }

bool EditorEngine::mergeWithNextParagraph() { return edit_.mergeWithNextParagraph(); }

bool EditorEngine::typeTextAndRender(const std::string& utf8) {
    if (!typeTextUtf8(utf8)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::backspaceAndRender() {
    if (!backspace()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::deleteForwardAndRender() {
    if (!deleteForward()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::splitParagraphAndRender() {
    if (!splitParagraph()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::mergeWithPreviousParagraphAndRender() {
    if (!mergeWithPreviousParagraph()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::mergeWithNextParagraphAndRender() {
    if (!mergeWithNextParagraph()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

/**
 * 表单：区间标注加编辑策略。
 *
 * 这一节一行都不碰排版管线。控件不参与断行/分页，只做两件事：给一段已有的文字挂上
 * "类型加可编辑性"（Control / ControlSet），以及在表单模式下把这段文字的几何画成
 * "字段"的样子（controlRects）。这正是选区间模型而不是矩形模型换来的。
 *
 * 控件集合没有独立的写入口：它是投影产物的派生结果（见 exportControls），
 * 换一份字段名单 = 换一棵树 = 重新装载一次文档。
 */

void EditorEngine::setEditMode(EditMode m) {
    if (mode_ == m) return;
    // 切模式就是切能力：表单与只读模式里没有图片拖拽框，选中的那份也随之作废。
    image_sel_ = kInvalidNodeId;
    const int plan_before = bracketPlanId();
    mode_ = m;
    // 编辑标记只读不画，而只读未必改内容（隐藏字段一个都没有时页面指纹不动）。
    //   所以照"光栅签名是唯一判据"那条（TRAPS 128）重算一次：该作废就作废。
    {
        const uint64_t sig = rasterSignatureNow();
        if (sig != raster_sig_) {
            raster_sig_ = sig;
            page_dirty_rects_.clear();
        }
    }

    // 切模式要重编译，而且整篇重排：隐藏控件的可见性口径随模式变（文档模式是设计态、
    //   表单与只读是呈现态），所以块流本身可能变 —— 隐藏字段的子树被摘除或放回，段数都可能
    //   跟着变。不重编译的话，呈现态下那些内容还留在版面上，而它们已经不可寻址。
    //   不能走后缀复用：摘除/放回发生在文档任何位置，脏区不是一个后缀。
    rebuildFromTree();
    markAllLayoutDirty();

    // 切模式会改变占位集合本身（只读模式一个都不占，文档与表单都要占），
    //   所以这里可能也要重排（Focused 档下还有"光标被夹进最近字段"这一层）。
    relayoutIfBracketPlanChanged(plan_before);
}

int EditorEngine::controlAtPt(float x_pt_doc, float y_pt_doc) const {
    TextPos p{};
    if (!hitTestPt(x_pt_doc, y_pt_doc, &p)) return -1;
    // include_end = false：点在内容上才算"点进了这个控件"。
    // 点在字段末尾之后一位、视觉上已经在框外，就不该算。
    return controls_.innermostAt(p, /*include_end=*/false);
}

bool EditorEngine::removeControl(int index, bool keep_content, FlowPos caret_before) {
    // "有没有可编辑的东西"是引擎的装载状态，命令层只管"编辑怎么做" —— 这一条留在门面。
    if (!loaded_) return false;
    return edit_.removeControl(index, keep_content, caret_before);
}

bool EditorEngine::setControlProp(int index, ControlProp prop, bool value) {
    if (!loaded_) return false;  // 没装载过文档：没有控件可改
    const EditCommand::PropResult r = edit_.setControlProp(index, prop, value);
    if (r == EditCommand::PropResult::Failed) return false;
    // hidden 改的是呈现态的流结构（整棵摘除），影响面不可定位 —— 整篇排版作废。
    if (r == EditCommand::PropResult::Structural) markAllLayoutDirty();
    return true;
}

bool EditorEngine::removeControlAndRender(int index, bool keep_content) {
    if (!removeControl(index, keep_content)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::setControlPropAndRender(int index, ControlProp prop, bool value) {
    if (!loaded_) return false;
    const EditCommand::PropResult r = edit_.setControlProp(index, prop, value);
    if (r == EditCommand::PropResult::Failed) return false;
    if (r == EditCommand::PropResult::Structural) markAllLayoutDirty();
    // 幂等（值没变）时状态与呈现一个字都没动 —— 跟着白跑一次重排重绘没有意义，
    //   也会把探针的"一次操作重画了多少"记账弄脏。
    if (r == EditCommand::PropResult::Unchanged) return true;
    relayout();
    repaintAfterEdit();
    return true;
}

std::vector<EditorEngine::ControlRect> EditorEngine::controlRects(int index) const {
    std::vector<ControlRect> out;
    if (index < 0 || index >= controls_.size()) return out;
    const ControlRange& cr = controls_.at(index).range;

    for (const LineBox& lb : layout_.lines) {
        if (lb.paragraph < cr.start.para || lb.paragraph > cr.end.para) continue;

        // 这一行里被区间覆盖的码点范围 [lo, hi)
        uint32_t lo = lb.first_cp;
        uint32_t hi = lb.last_cp;
        if (lb.paragraph == cr.start.para && cr.start.offset > lo)
            lo = static_cast<uint32_t>(cr.start.offset);
        if (lb.paragraph == cr.end.para && cr.end.offset < hi)
            hi = static_cast<uint32_t>(cr.end.offset);
        if (lo >= hi) continue;

        // 左右边缘不能用同一个函数取，这是最容易写错的地方：
        //   左边缘取"lo 那个簇自己的起点"（对）；
        //   右边缘若也取"hi 那个簇的起点"，拿到的是区间内最后一个字的左边缘，
        //   会整整少掉一个字的宽度（症状：框比字窄一格，看着像没框住）。
        //   所以右边缘取"最后一个 cp < hi 的簇的右边缘"。
        // 跳过占位簇：括号在内容之外（[内容]），控件矩形只该框住内容。
        //   不跳的话矩形会把前缀的括号宽度也算进来，左边缘会左移一个括号 ——
        //   于是"点在字段矩形中心"这类断言会差几像素，而画面看着又是对的。
        float left = lb.inline_end_pt;
        for (const LineCluster& c : lb.clusters) {
            if (c.cp >= lo && !c.placeholder) {
                left = c.inline_pt;
                break;
            }
        }
        float right = left;
        for (size_t i = 0; i < lb.clusters.size(); ++i) {
            if (lb.clusters[i].cp >= hi) break;
            right = (i + 1 < lb.clusters.size()) ? lb.clusters[i + 1].inline_pt : lb.inline_end_pt;
        }
        if (!(right > left)) continue;

        const float a0 = lb.inline_origin_pt + left;
        const float a1 = lb.inline_origin_pt + right;
        Rect r;
        if (!lb.vertical) {
            r.x = a0;
            r.y = lb.block_lo_pt;
            r.w = a1 - a0;
            r.h = lb.block_hi_pt - lb.block_lo_pt;
        } else {
            r.x = lb.block_lo_pt;
            r.y = a0;
            r.w = lb.block_hi_pt - lb.block_lo_pt;
            r.h = a1 - a0;
        }
        out.push_back(ControlRect{lb.page, r});
    }
    return out;
}

bool EditorEngine::focusControl(bool forward) {
    std::vector<TextPos> starts;
    for (const Control& c : controls_.all()) {
        if (!c.read_only) starts.push_back(c.range.start);
    }
    if (starts.empty()) return false;
    std::sort(starts.begin(), starts.end(),
              [](const TextPos& a, const TextPos& b) { return a < b; });

    TextPos target{};
    bool found = false;
    if (forward) {
        for (const TextPos& s : starts) {
            if (caret() < s) {
                target = s;
                found = true;
                break;
            }
        }
        if (!found) target = starts.front();  // 到头绕回第一个（表单里 Tab 就该循环）
    } else {
        for (auto it = starts.rbegin(); it != starts.rend(); ++it) {
            if (*it < caret()) {
                target = *it;
                found = true;
                break;
            }
        }
        if (!found) target = starts.back();
    }
    if (target == caret()) return false;
    const int plan_before = bracketPlanId();
    setCaretByText(target);
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::indent() { return edit_.indent(); }

bool EditorEngine::outdent() { return edit_.outdent(); }

bool EditorEngine::indentAndRender() {
    if (!indent()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::outdentAndRender() {
    if (!outdent()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::focusControlAndRender(bool forward) {
    if (!focusControl(forward)) return false;
    // 必须收尾重绘，不能只改状态。
    //
    //   focusControl 会跨字段移动光标，导致占位变化与整行重排。若只改状态，
    //   JS 侧紧接着读 editStatsJson() 拿到的是上一次呈现留下的账（脏矩形属于上一次
    //   事件），用它贴只会覆盖画布的一小块，形成"引擎里的页是新的、画布是新旧混合"。
    //
    //   规矩（整个 wasm 接口面都遵守）：凡是会改引擎状态的入口，返回宿主前必须把
    //   画面收尾。typeText/backspace/moveCaret
    //   都是 *AndRender，唯独 focusControl 漏了。
    repaintAfterEdit();
    return true;
}

bool EditorEngine::tabCaret(bool backward) {
    if (blocks().empty()) return false;
    // 文档模式与只读模式都不消费 Tab：后者要放行给宿主（吞掉会把键盘用户困在编辑器里），
    //   前者的 Tab 该是制表符 —— 但那一步还没落地（排版层没有制表位），所以文档模式现在
    //   由宿主当缩进处理，见 EngineConfig::tab_spaces。
    if (mode_ != EditMode::Form) return false;

    const int plan_before = bracketPlanId();
    const TextPos before = caret();
    if (!flow_.has_value()) return false;
    const FlowPos from = flowPosAtSide(caret().para, caret().offset, caretSide());
    if (from < 0) return false;
    const rev::TabStop stop = rev::tabFrom(document(), mode_, from, backward);
    // handled 为假 = 已经没有下一个（上一个）可编辑控件了：放行给宿主，不绕回第一个。
    if (!stop.handled) return false;
    // 目标落在控件的内容起点上，共享边界上"前一个控件的末尾"与"这个控件的开头"是
    //   同一个偏移 —— 位置与侧别都由目标流位置一次算准（重推会推成前一个控件，
    //   Tab 跳过去就打不进字）。
    setCaretAtFlow(stop.target);
    if (caret() == before) return false;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::tabCaretAndRender(bool backward) {
    if (!tabCaret(backward)) return false;
    // 与 focusControlAndRender 同一条纪律：跨字段移动会让占位变化、整行重排，
    //   只改状态会让宿主拿上一次的脏矩形去上屏，画布成新旧混合体。
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::moveCaretAndRender(int dcp, bool extend) {
    if (!moveCaretBy(dcp, extend)) return false;
    // 光标移动不重排 —— 这就是增量的意义：
    //   页位图一个字都没变，只需要把两个小矩形标脏。
    //
    // 这里用裸 repaint()，刻意不把光标拉成可见（repaintAfterEdit 才会）。
    //   理由：移动光标不是编辑，光标该继续按自己的相位闪；把它强行点亮会让
    //   "按着方向键"时看不出闪烁。而"隐藏相位 + 宿主补救一次 present"这条
    //   双 present 路径是安全的 —— 页脏矩形累积到宿主读走为止，第二次的矩形
    //   挤不掉第一次的账（见 pageDirtyRects 的注释、单测 [12]）。
    //   顺带一条：单测里"光标隐藏时写入量更小（只擦不画）"这条断言，
    //   量的正是这条路径 —— 把它改成 repaintAfterEdit 会让它失去意义。
    repaint();
    return true;
}

bool EditorEngine::moveCaretToLineStartAndRender(bool extend) {
    if (!moveCaretToLineStart(extend)) return false;
    repaint();  // 不重排 —— 和 moveCaretAndRender 同一条增量路径（也不点亮光标，同上）
    return true;
}

bool EditorEngine::moveCaretToLineEndAndRender(bool extend) {
    if (!moveCaretToLineEnd(extend)) return false;
    repaint();  // 同 moveCaretAndRender：不重排、不点亮光标
    return true;
}

bool EditorEngine::moveCaretToDocStartAndRender(bool extend) {
    if (!moveCaretToDocStart(extend)) return false;
    repaint();
    return true;
}

bool EditorEngine::moveCaretToDocEndAndRender(bool extend) {
    if (!moveCaretToDocEnd(extend)) return false;
    repaint();
    return true;
}

bool EditorEngine::moveCaretVerticalAndRender(int dir, bool extend) {
    if (!moveCaretVertical(dir, extend)) return false;
    repaint();
    return true;
}

bool EditorEngine::selectAllAndRender() {
    if (!selectAll()) return false;
    repaint();
    return true;
}

bool EditorEngine::setSelectionAndRender(Range r) {
    if (!setSelection(r)) return false;
    repaint();
    return true;
}

bool EditorEngine::selectCellAtParaAndRender(int para) {
    if (!selectCellAtPara(para)) return false;
    repaint();
    return true;
}

/**
 * 整格内容的流区间。嵌套表格取最短的那一格 = 最内层（见头文件）。
 */
bool EditorEngine::cellFlowRangeAt(int para, FlowPos* lo, FlowPos* hi) const {
    const LayoutCell* c = layoutCellAt(para, nullptr);
    if (c == nullptr) return false;
    return cellFlowRangeOf(*c, lo, hi);
}

/**
 * 最内层那一格：同一段可能落在好几格里（外层格包着内层表），取块数最少的那个。
 */
const LayoutCell* EditorEngine::layoutCellAt(int para, const LayoutTable** table_out) const {
    const LayoutCell* best = nullptr;
    const LayoutTable* best_table = nullptr;
    for (const LayoutTable& t : tables_) {
        for (const LayoutCell& c : t.cells) {
            if (c.block_count <= 0) continue;
            if (para < c.first_block || para >= c.first_block + c.block_count) continue;
            if (best == nullptr || c.block_count < best->block_count) {
                best = &c;
                best_table = &t;
            }
        }
    }
    if (table_out != nullptr) *table_out = best_table;
    return best;
}

/**
 * 一格的内容区间：首块起点到末块末端。
 */
bool EditorEngine::cellFlowRangeOf(const LayoutCell& c, FlowPos* lo, FlowPos* hi) const {
    if (c.block_count <= 0) return false;
    const int first = c.first_block;
    const int last_b = first + c.block_count - 1;
    if (first < 0 || last_b >= static_cast<int>(block_flow_.size()) ||
        last_b >= static_cast<int>(block_flow_span_.size()))
        return false;
    const FlowPos a = block_flow_[static_cast<size_t>(first)];
    const FlowPos b =
        block_flow_[static_cast<size_t>(last_b)] + block_flow_span_[static_cast<size_t>(last_b)];
    if (b <= a) return false;
    if (lo) *lo = a;
    if (hi) *hi = b;
    return true;
}

/**
 * 按点选整格（三击走这条）：点在格里的任何位置都算，不限于左侧的边距带。
 *
 * 与 hitTestPt 的 out_cell 分开：那一对是"单击落在边距带上"这个手势的判据（点字上不该
 * 变成整格），三击则明确是"我要这一格"，点在哪都成立。
 */
bool EditorEngine::selectCellAtPt(float x_pt_doc, float y_pt_doc) {
    TextPos p{};
    if (!hitTestPt(x_pt_doc, y_pt_doc, &p)) return false;
    return selectCellAtPara(p.para);
}

/**
 * 命中测试：几何的反向半边 —— 由落点反查 (段, 偏移) + 侧别 + 归属控件。
 *
 * 收的是文档坐标：命中测试问的是"文档里的哪个位置"，与窗口开在哪一页无关。
 *   拿窗口内坐标去反查会让页归属整体错位（页在文档里的位置 = 页号 x 步距）。
 */

bool EditorEngine::hitTestPt(float x_pt_doc, float y_pt_doc, TextPos* out, int* out_owner,
                             int* out_side, int* out_line_aff, int* out_cell_para,
                             bool clamp) const {
    if (out_owner) *out_owner = -1;
    if (out_side) *out_side = 0;
    if (out_line_aff) *out_line_aff = 0;
    if (out_cell_para) *out_cell_para = -1;
    if (!out || layout_.lines.empty()) return false;

    const float page_w = cfg_.page.width_pt;
    const float zoom = cfg_.zoom;
    const int page_px_h = pagePxHeight();
    const int step_px = page_px_h + pageGapPx();

    int page = 0;
    float local_y = y_pt_doc;
    if (step_px > 0 && zoom > 0.0f) {
        // 页号按宿主摆页用的那个像素步距折，不按磅步距：宿主把页摆在
        //   pagePxHeight + pageGapPx 这些整数像素上，而磅步距是未取整的页高 + 页间隙
        //   （本机 855.89 磅对 1142 px / zoom = 856.5 磅）。用磅步距折，误差随页号累积
        //   —— 2000 段文档第 160 页上实测偏下约 130 px（0.6 磅/页 x 160 页），
        //   表象就是"最后几页点哪都偏下"。乘 zoom 折成设备像素再取页号，两边就一致。
        const float y_px = y_pt_doc * zoom;
        page = std::max(0, static_cast<int>(std::floor(y_px / static_cast<float>(step_px))));
        float local_px = y_px - static_cast<float>(page) * static_cast<float>(step_px);
        // 落在页间空隙里，或者横向出了页外 —— 没有"这一页的哪一行"可言。
        //   拖选那一路允许夹取：指针拖出画布之后仍要能选，落在缝里就按最近的那条边算，
        //   否则"拖到页外选区就不动了"。单击那一路不夹：点在灰底上本来就不该挪光标。
        if (clamp) {
            // 夹的是内容区，不是页框：四边页边距里没有正文，夹到页框等于让落点停在
            //   那一页最靠近的一行上。跨页表格里那一行常常在另一列（高的那格一路排到
            //   页底），于是"顺着这一列往下拖"会把焦点甩到首列，矩形包络铺满整行。
            float lo_px = 0.0f;
            float hi_px = static_cast<float>(page_px_h);
            const PageMargins& pm = cfg_.page.margins;
            if (!cfg_.page.continuous && cfg_.page.height_pt > pm.top + pm.bottom) {
                lo_px = pm.top * zoom;
                hi_px = (cfg_.page.height_pt - pm.bottom) * zoom;
            }
            local_px = std::max(lo_px, std::min(local_px, hi_px));
            if (x_pt_doc < 0.0f) x_pt_doc = 0.0f;
            if (x_pt_doc > page_w) x_pt_doc = page_w;
        } else {
            if (local_px < 0.0f || local_px > static_cast<float>(page_px_h)) return false;
            if (x_pt_doc < 0.0f || x_pt_doc > page_w) return false;
        }
        local_y = local_px / zoom;
    }

    // 块轴：找最近的一行。
    //   连页下按文档像素比：一行可以跨条带，它跨过去的那一段落在下一条带里，
    //   而那个位置的页号已经不是行盒的页号 —— 按页号筛会把这一点判给下一条带上
    //   离得最近的另一行（每跨一条带就有一条这样的窄带点不中）。
    //   比的是"渲染位置"（页号 x 像素步距 + 页内磅 x zoom），与宿主把页摆在哪无关 ——
    //   与绘制用的是同一套映射。
    const bool cont = cfg_.page.continuous;
    const LineBox* best = nullptr;
    float best_d = 1e30f;
    float best_dx = 1e30f;
    int best_rank = 2;
    // 表格优先：先看这一点落在哪个格框里，再只在那一格的块区间里挑行。
    //   跨页切开的一行，续排片段上有些格的内容已经在上一页结束 —— 它们在这一页没有行盒，
    //   只按行盒找会把这一点判给同一行里唯一还有行盒的那一格（表象是"点空格子，
    //   光标跳进旁边有字的格"）。格框是行片段 + 列边界给的，与有没有行盒无关。
    const LayoutCell* cell_hit = nullptr;
    {
        float cell_area = 1e30f;
        for (const PlacedTable& pt : layout_.placed_tables) {
            if (x_pt_doc < pt.x - 0.5f || x_pt_doc > pt.x + pt.w + 0.5f) continue;
            for (const PlacedRow& pr : pt.placed) {
                if (pr.page != page) continue;
                if (local_y < pr.y - 0.01f || local_y > pr.y + pr.h + 0.01f) continue;
                int col = -1;
                const int n_col = static_cast<int>(pt.col_x.size()) - 1;
                for (int c = 0; c < n_col; ++c) {
                    const float lo = pt.x + pt.col_x[static_cast<size_t>(c)];
                    const float hi = pt.x + pt.col_x[static_cast<size_t>(c) + 1];
                    if (x_pt_doc >= lo - 0.01f && x_pt_doc <= hi + 0.01f) {
                        col = c;
                        break;
                    }
                }
                if (col < 0) continue;
                const LayoutCell* lc = pt.cellAt(pr.row, col, nullptr, nullptr);
                if (lc == nullptr) continue;
                // 嵌在别的表里时取最内层那一张：面积最小的框就是最里面那格。
                const float area = pt.w * pr.h;
                if (area < cell_area) {
                    cell_area = area;
                    cell_hit = lc;
                }
            }
        }
    }
    if (cell_hit != nullptr) {
        // 这一格的块区间里挑最近的一行：先比页号（同一页优先），再比块轴距离。
        const int first = cell_hit->first_block;
        const int last = first + std::max(cell_hit->block_count, 1);
        int pick_pd = 1 << 20;
        for (const LineBox& lb : layout_.lines) {
            if (lb.paragraph < first || lb.paragraph >= last) continue;
            const int pd = std::abs(lb.page - page);
            const float d =
                lb.vertical ? 0.0f
                            : (local_y < lb.block_lo_pt
                                   ? lb.block_lo_pt - local_y
                                   : (local_y > lb.block_hi_pt ? local_y - lb.block_hi_pt : 0.0f));
            if (pd < pick_pd || (pd == pick_pd && d < best_d - 0.01f)) {
                pick_pd = pd;
                best_d = d;
                best = &lb;
            }
        }
        // 这一格一条行盒都没有（格刚被清空、排版还没跟上）：退回按行盒找，
        //   宁可落点粗一点，也不要让这一下点空。
        if (best == nullptr) cell_hit = nullptr;
    }
    for (const LineBox& lb : layout_.lines) {
        if (cell_hit != nullptr) break;
        float bp = 0.0f;
        float lo = 0.0f;
        float hi = 0.0f;
        float local_bp = 0.0f;  // 与行盒同一个页内口径：判"点是否落在格子里"要用它
        if (lb.vertical) {
            bp = x_pt_doc;
            local_bp = x_pt_doc;
            lo = lb.block_lo_pt;
            hi = lb.block_hi_pt;
        } else if (cont) {
            const float base = static_cast<float>(lb.page) * static_cast<float>(step_px);
            bp = y_pt_doc * zoom;
            local_bp = (bp - base) / zoom;
            lo = base + lb.block_lo_pt * zoom;
            hi = base + lb.block_hi_pt * zoom;
        } else {
            if (lb.page != page) continue;
            bp = local_y;
            local_bp = local_y;
            lo = lb.block_lo_pt;
            hi = lb.block_hi_pt;
        }
        const float d = (bp < lo) ? (lo - bp) : (bp > hi ? bp - hi : 0.0f);
        // 块轴距离并列时用行内轴距离定夺。表格让"同一块轴位置上有多个行盒"成为常态
        //   （一行的几个格在同一个 y 上），只按块轴比会一律选中先出现的那一格 ——
        //   表象是"点第二格的字，光标跳到第一格"。正文里同一 y 不会有两条行盒，
        //   所以这一条在无表格时不会改变任何选择。
        const float a0 = (lb.cell_hi_pt > lb.cell_lo_pt) ? lb.cell_lo_pt : lb.inline_origin_pt;
        const float a1 = (lb.cell_hi_pt > lb.cell_lo_pt) ? lb.cell_hi_pt
                                                         : (lb.inline_origin_pt + lb.inline_end_pt);
        const float ipos = lb.vertical ? y_pt_doc : x_pt_doc;
        const float dx = (ipos < a0) ? (a0 - ipos) : (ipos > a1 ? ipos - a1 : 0.0f);
        // 落在某个格的框内时，别的格的行盒一概不参选。格的纵向范围是整格（含行高
        //   撑出来的空白），所以"点空格子的下半部分"仍归这一格，而不是跳到同一行里
        //   行盒更多的那一格上 —— 按块轴距离比，多行格的低位行盒永远更近。
        int rank = 1;
        if (lb.cell_box_hi_pt > lb.cell_box_lo_pt && lb.cell_bottom_pt > lb.cell_top_pt &&
            ipos >= lb.cell_box_lo_pt && ipos <= lb.cell_box_hi_pt && local_bp >= lb.cell_top_pt &&
            local_bp <= lb.cell_bottom_pt) {
            rank = 0;
        }
        if (rank < best_rank ||
            (rank == best_rank &&
             (d < best_d - 0.01f || (std::fabs(d - best_d) <= 0.01f && dx < best_dx)))) {
            best_rank = rank;
            best_d = d;
            best_dx = dx;
            best = &lb;
        }
    }
    if (!best) return false;

    // 行内轴：找最近的插入点。势力范围与簇边界的口径在 hitTestInLine 里（上下键也走它）。
    //   竖排的行内轴是 y，页内位置要按"选中那一行自己的页号"折 —— 跨条带时它与
    //   点击位置的页号可以差一条带。
    const float ip = best->vertical ? (cont ? (y_pt_doc * zoom - static_cast<float>(best->page) *
                                                                     static_cast<float>(step_px)) /
                                                  zoom
                                            : local_y)
                                    : x_pt_doc;
    uint32_t cp = 0;
    int side = 0;
    caret_.hitTestInLine(*best, ip, &cp, &side);

    TextPos p{best->paragraph, cp};
    // 行别：命中的这一行是不是"以这个偏移开头"。是的话，同一个位置也是上一行的末尾
    //   （折行处一行有两个候选），落点必须贴着这一行 —— 否则点行首会画到上一行行尾。
    //   段首那一行不算：它前面没有上一行，贴哪儿都一样。
    const int line_aff = (cp == best->first_cp && best->first_cp != 0) ? 1 : 0;
    // 侧别是落点的完整身份（同一个偏移上有几个位置就看它）；归属只是它的一个投影，
    //   给宿主显示"当前在哪个字段"用。两者一起交出去：宿主搬运的是侧别。
    if (out_side) *out_side = side;
    if (out_line_aff) *out_line_aff = line_aff;
    int owner = -1;
    if (!control_node_ids_.empty() && flow_.has_value()) {
        const size_t para = static_cast<size_t>(best->paragraph);
        if (para < block_flow_.size()) {
            const FlowPos hit_flow = block_flow_[para] + caret_.relAt(best->paragraph, cp, side);
            const int ci = innermostControlAtFlow(*flow_, control_node_ids_, hit_flow);
            owner = ci >= 0 ? ci : LineBox::kInlineOffsetOuter;
        }
    }
    // 落在行内内容的起止之外（页边距、行末那一段空白）：落点照旧吸附到最近的字，
    //   但"这一下点的是哪个控件"不能认。跨行控件尤其明显 —— 控件第二行只剩一枚括号时，
    //   在左边距上随手一点会被算成"点在控件上"，把某个选项直接切掉。
    //   归属只在行内范围之内成立；范围之外按"不在任何控件里"报。
    {
        const float inline_lo = best->inline_origin_pt;
        const float inline_hi = inline_lo + best->inline_end_pt;
        if (owner >= 0 && (ip < inline_lo - 0.5f || ip > inline_hi + 0.5f)) {
            owner = LineBox::kInlineOffsetOuter;
        }
    }
    // 命中即落点：光标位置不受控件约束，所以这里不再做"就近吸附到最近字段"。
    //   吸附会把"点字段外正文"改写成"停在字段端点"，于是画出来的光标不在点的地方，
    //   表象就是"点了没反应"。归属照实返回，编辑侧自己会按策略拒绝。
    if (out_owner) *out_owner = owner;
    *out = p;

    // 格子的边距带：落在格内、且在文字左沿之前的那一条。点在这里是"选整格"，
    //   点在同一行的字上仍是文字光标 —— 两套操作靠这一条几何分开，而不是靠"选了多少"。
    //   宽度的两头取格的外框与文字左沿：外框比内容区多一层内边距，按内容区算会让
    //   贴着格线的那一窄条点不出整格。
    //   空格也只给左边这一窄条：整格都算边距带的话，点空格就成了"选整格"，
    //   连光标都放不进去（用户要的是"点一下落光标，拖或三击才选格"）。
    if (out_cell_para != nullptr && best->cell_box_hi_pt > best->cell_box_lo_pt &&
        best->cell_bottom_pt > best->cell_top_pt) {
        float text_start = best->inline_origin_pt;
        for (const LineCluster& lc : best->clusters) {
            if (!lc.placeholder) {
                text_start += lc.inline_pt;
                break;
            }
        }
        if (ip >= best->cell_box_lo_pt && ip < text_start) *out_cell_para = best->paragraph;
    }
    return true;
}

bool EditorEngine::insertTable(int rows, int cols, bool header) {
    if (!loaded_) return false;
    return edit_.insertTable(rows, cols, header);
}

bool EditorEngine::insertTableAndRender(int rows, int cols, bool header) {
    if (!insertTable(rows, cols, header)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::insertControl(const ControlSpec& spec, const std::string& label) {
    if (!loaded_) return false;
    return edit_.insertControl(spec, label);
}

bool EditorEngine::insertControlAndRender(const ControlSpec& spec, const std::string& label) {
    if (!insertControl(spec, label)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

}  // namespace heditor
