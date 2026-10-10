/**
 * 引擎门面与编辑会话。
 *
 * 这里放三样东西：生命周期与配置（init / setConfig / setBlocks）、把排版结果暴露给宿主的
 * 查询（帧、光标、命中、控件）、以及编辑入口（插入删除、分段合并、缩进）。
 * 排版调度在 engine_layout.cpp、呈现与光栅增量在 engine_paint.cpp、审计判据在
 * engine_audit.cpp、诊断输出在 engine_diag.cpp —— 拆开是因为它们的变更理由不同。
 *
 * 编辑入口这一段将来会搬到命令层（见 docs/正式化架构与迁移路线.md 阶段 2），
 * 所以现在不为它做进一步的职责拆分。
 */

#include "core/engine/engine.h"

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

namespace ced {

namespace {


const std::vector<uint32_t> kEmptyStarts;


}  // namespace

/** 生命周期：字体加载与引擎初始化。 */

bool EditorEngine::init(const std::vector<std::string>& fontPaths) {
    for (const auto& p : fontPaths) {
        if (fonts_.addFace(p) < 0) return false;
    }
    if (fonts_.faceCount() <= 0) return false;
    base_valid_ = false;

    // 帧缓冲与底图共享同一份字形缓存：同一个字形在两块图上光栅出来的
    //   mask 一模一样，各存一份纯属浪费。而这份缓存正对着绘制成本的大头 ——
    //   实测 4 页文档 @1.5× 的 paint 里有 91.5% 花在字形光栅上（121 µs/字形）。
    if (!glyph_cache_) glyph_cache_ = std::make_shared<GlyphCache>();
    bitmap_.setGlyphCache(glyph_cache_);
    base_.setGlyphCache(glyph_cache_);
    return true;
}

void EditorEngine::disableGlyphCache() {
    bitmap_.setGlyphCache(nullptr);
    base_.setGlyphCache(nullptr);
    glyph_cache_.reset();
    base_valid_ = false;  // 底图要按"无缓存"的路径重建一遍
}

void EditorEngine::setConfig(const EngineConfig& cfg) {
    // 改"显示括号的范围"会改变占位（括号进文档流）→ 必须重排。
    //   不重排的话括号会按新档画、正文却按旧档排 —— 画的用的还是旧的簇坐标，
    //   表象是括号盖住字/槽里空一条，而且看不出来是哪一边错了。
    //   （其它配置项不动排版结构：zoom / 页度量只影响几何，底图作废就够了 ——
    //     宿主改那两项之后本来就会立刻重排重绘。）
    const bool plan_changed = (cfg.form_brackets != cfg_.form_brackets);
    // 页度量改了整篇的断行位置都会变，没有可定位的影响面 → 整篇标脏。
    const bool page_changed = (cfg.page != cfg_.page);
    cfg_ = cfg;
    if (cfg_.zoom < 0.25f) cfg_.zoom = 0.25f;
    if (cfg_.zoom > 8.0f) cfg_.zoom = 8.0f;
    if (cfg_.caret_width_pt < 0.4f) cfg_.caret_width_pt = 0.4f;
    base_valid_ = false;  // 缩放/页度量变了，底图作废
    caret_rect_px_prev_ = Rect{};
    if (plan_changed) markLayoutDirtyForBrackets();
    if (page_changed) markAllLayoutDirty();
    if (plan_changed || page_changed) relayout();
}

void EditorEngine::setBlocks(std::vector<BlockSpec> blocks) {
    blocks_ = std::move(blocks);
    caret_ = TextPos{0, 0};
    caret_rect_px_prev_ = Rect{};
    base_valid_ = false;
    // 整份文档换了：段数与内容都可能全变，没有可复用的前缀。
    markAllLayoutDirty();
}

size_t EditorEngine::frameBytes() const { return bitmap_.byteSize(); }

size_t EditorEngine::documentCodepoints() const {
    size_t n = 0;
    for (const BlockSpec& b : blocks_) n += b.cp_text.size();
    return n;
}

/** 光标：位置设置、字素簇吸附与光标矩形查询。 */

size_t EditorEngine::snapToCluster(int para, size_t offset) const {
    if (para < 0 || para >= static_cast<int>(blocks_.size())) return 0;
    const size_t len = blocks_[static_cast<size_t>(para)].cp_text.size();
    if (offset > len) offset = len;
    if (para < static_cast<int>(layout_.para_cluster_starts.size())) {
        const std::vector<uint32_t>& starts =
            layout_.para_cluster_starts[static_cast<size_t>(para)];
        if (!starts.empty()) return clusterFloor(starts, offset);
    }
    return offset;
}

const LineBox* EditorEngine::findLineFor(const TextPos& pos) const {
    const LineBox* best = nullptr;
    for (const LineBox& lb : layout_.lines) {
        if (lb.paragraph != pos.para) continue;
        if (pos.offset >= lb.first_cp && pos.offset <= lb.last_cp) return &lb;
        if (lb.first_cp <= pos.offset)
            best = &lb;
        else if (!best)
            best = &lb;
    }
    return best;
}

bool EditorEngine::caretRectFor(const TextPos& pos, int* page_out, Rect* out) const {
    const LineBox* lb = findLineFor(pos);
    if (!lb) return false;
    // 归属提示只对"当前光标"这一处有意义（它是点画布时定下来的，见 setCaretAt）。
    //   其它位置（选区另一端、诊断打印）没有这个信息，走中性规则。
    const int hint = (pos == caret_) ? caretFieldHint() : -1;
    const float off = lb->inlineOffsetFor(static_cast<uint32_t>(pos.offset), hint);
    const float t = cfg_.caret_width_pt;
    Rect r;
    if (!lb->vertical) {
        // 横排：光标是竖条，贴在行盒左/右，高度 = 行盒高
        r.x = lb->inline_origin_pt + off - t * 0.5f;
        r.y = lb->block_lo_pt;
        r.w = t;
        r.h = std::max(0.0f, lb->block_hi_pt - lb->block_lo_pt);
    } else {
        // 竖排（含侧倒）：光标是横条，贴在列盒上/下，宽度 = 列盒宽
        r.x = lb->block_lo_pt;
        r.y = lb->inline_origin_pt + off - t * 0.5f;
        r.w = std::max(0.0f, lb->block_hi_pt - lb->block_lo_pt);
        r.h = t;
    }
    if (page_out) *page_out = lb->page;
    if (out) *out = r;
    return true;
}

bool EditorEngine::caretRectPt(Rect* out) const {
    int page = 0;
    return caretRectFor(caret_, &page, out);
}

Rect EditorEngine::caretRectPx() const {
    int page = 0;
    Rect r{};
    if (!caretRectFor(caret_, &page, &r)) return Rect{};
    const float zoom = cfg_.zoom;
    const int page_px_h = std::max(1, static_cast<int>(std::lround(cfg_.page.height_pt * zoom)));
    const int gap_px = std::max(0, static_cast<int>(std::lround(cfg_.page_gap_pt * zoom)));
    const float off_y = static_cast<float>((page - base_first_page_) * (page_px_h + gap_px));
    Rect px;
    px.x = std::floor(r.x * zoom);
    px.y = std::floor(r.y * zoom + off_y);
    px.w = std::max(1.0f, std::ceil(r.w * zoom));
    px.h = std::max(1.0f, std::ceil(r.h * zoom));
    return px;
}

void EditorEngine::setCaret(int para, size_t offset) {
    // 无侧别的入口：把上一次点击带进来的归属提示清掉。
    caret_field_ = -1;
    if (blocks_.empty()) {
        caret_ = TextPos{0, 0};
        return;
    }
    if (para < 0) para = 0;
    if (para >= static_cast<int>(blocks_.size())) para = static_cast<int>(blocks_.size()) - 1;
    const int plan_before = bracketPlanId();
    // 光标位置不受控件约束：字段外、只读字段里都能停。约束编辑的是 EditPolicy
    //   （插 / 删 / 分段），不是光标 —— 两者混起来会得到"字段外的正文连点都点不进去"
    //   这种与编辑边界无关的怪现象。所以这里除了吸附到字素簇之外不改写任何东西。
    caret_ = TextPos{para, snapToCluster(para, offset)};
    // 占位计划变了 → 必须重排（占位进文档流）。
    //   All 档下计划与光标无关 → 这一句恒为 no-op（光标移动是真增量的地基）。
    relayoutIfBracketPlanChanged(plan_before);
}

/**
 * 带归属的定位（点画布）。
 *
 * 为什么位置本身不够：共享边界上"前一个字段的末尾"与"后一个字段的开头"是同一个
 * 偏移，两枚括号紧挨着排在同一列上。只看偏移，归属只能按"最内层、同深度取下标
 * 小者"来定，恒定为前一个字段。于是点后一个字段的开头时，光标虽然停住了，字却会
 * 落进前一个字段；前一个字段只读时更直接：编辑被拒，看起来就像点不进去。
 *
 * 浏览器里点的是具体某一枚括号，这项信息由 hitTestPt 带出来（占位簇记着
 * owner_control），这里把它记在光标上，供画光标（决定贴闭括号之内还是开括号之后，
 * 见 LineBox::inlineOffsetFor）与编辑归属判定两处使用。提示只在光标仍停在该位置上
 * 时才有效，一按方向键或一打字位置就变，它随之失效。
 */
void EditorEngine::setCaretAt(int para, size_t offset, int field) {
    caret_field_ = -1;
    if (blocks_.empty()) {
        caret_ = TextPos{0, 0};
        return;
    }
    if (para < 0) para = 0;
    if (para >= static_cast<int>(blocks_.size())) para = static_cast<int>(blocks_.size()) - 1;
    const int plan_before = bracketPlanId();
    caret_ = TextPos{para, snapToCluster(para, offset)};
    // 提示只在它真的覆盖落点时才认 —— 否则它描述的是别处的事，不能拿来指挥编辑。
    if (field >= 0 && field < controls_.size() &&
        rangeAllowsInsert(controls_.at(field).range, caret_)) {
        caret_field_ = field;
        caret_field_at_ = caret_;
    }
    relayoutIfBracketPlanChanged(plan_before);
}

bool EditorEngine::moveCaretBy(int dcp) {
    if (blocks_.empty() || dcp == 0) return false;
    const int plan_before = bracketPlanId();

    TextPos p = caret_;
    if (p.para < 0) p.para = 0;
    if (p.para >= static_cast<int>(blocks_.size())) p.para = static_cast<int>(blocks_.size()) - 1;
    p.offset = snapToCluster(p.para, p.offset);

    const int steps = std::abs(dcp);
    for (int k = 0; k < steps; ++k) {
        const bool have_starts = p.para < static_cast<int>(layout_.para_cluster_starts.size()) &&
                                 !layout_.para_cluster_starts[static_cast<size_t>(p.para)].empty();
        const std::vector<uint32_t>& starts =
            have_starts ? layout_.para_cluster_starts[static_cast<size_t>(p.para)] : kEmptyStarts;

        TextPos next = p;
        if (dcp < 0) {
            if (next.offset > 0) {
                if (have_starts) {
                    const size_t prev = clusterFloor(starts, next.offset - 1);
                    next.offset = (prev < next.offset) ? prev : (next.offset - 1);
                } else {
                    --next.offset;
                }
            } else if (next.para > 0) {
                --next.para;
                next.offset = blocks_[static_cast<size_t>(next.para)].cp_text.size();
            } else {
                break;
            }
        } else {
            const size_t len = blocks_[static_cast<size_t>(next.para)].cp_text.size();
            if (next.offset < len) {
                if (have_starts) {
                    const size_t nx = clusterCeil(starts, next.offset);
                    next.offset = (nx > next.offset) ? nx : (next.offset + 1);
                } else {
                    ++next.offset;
                }
            } else if (next.para + 1 < static_cast<int>(blocks_.size())) {
                ++next.para;
                next.offset = 0;
            } else {
                break;
            }
        }
        // 逐格走，唯一的停步条件是"前面没有下一格了"（段首/段尾）。
        //   这里不再判"下一步可停吗"（表单模式下不许离开出发字段），那条约束已经撤掉：
        //   光标位置不再受控件约束，方向键能走出字段、也能跨过只读字段。
        p = next;
    }

    if (p == caret_) return false;
    caret_ = p;
    // 只有 Focused 档下跨字段移动才会走到"要重排"（All 档计划恒定）。
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretToLineStart() {
    const LineBox* lb = findLineFor(caret_);
    if (!lb) return false;
    const int plan_before = bracketPlanId();
    // 行首就是行首，不再往字段边界上收：光标位置不受控件约束（见 setCaret）。
    const TextPos p{lb->paragraph, lb->first_cp};
    if (p == caret_) return false;
    caret_ = p;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretToLineEnd() {
    const LineBox* lb = findLineFor(caret_);
    if (!lb) return false;
    const int plan_before = bracketPlanId();
    const TextPos p{lb->paragraph, lb->last_cp};
    if (p == caret_) return false;
    caret_ = p;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

void EditorEngine::showCaret(bool on) { caret_visible_ = on; }

/** 编辑：插入与删除，先过 EditPolicy 判定，再按字素簇吸附。 */

bool EditorEngine::insertCodepoints(int para, size_t offset, const std::vector<uint32_t>& cps) {
    if (cps.empty()) return false;
    if (para < 0 || para >= static_cast<int>(blocks_.size())) return false;
    // 插入前必须吸附：否则一串 e + U+0301 中间被插进字符，基字与标记就分家了。
    const size_t at = snapToCluster(para, offset);

    // 策略判定用的是吸附之后的位置 —— 判定用的位置和真正落笔的位置
    //   必须是同一个。否则会出现"策略放行了一个它根本没看过的位置"这种漏洞：
    //   吸附把 at 挪到簇边界之后，落笔点可能已经跨到另一个控件里去了。
    // 归属提示只在"落笔点就是光标现在待的地方"时才采纳：insertCodepoints 也能被
    //   宿主或测试从别的位置调进来，那时光标上的提示与该次编辑无关。
    const TextPos ins{para, at};
    const int owner_hint = (ins == caret_) ? caretFieldHint() : -1;
    if (!EditPolicy::canInsert(controls_, policy_.mode(), ins, owner_hint)) {
        ++blocked_edits_;
        return false;
    }

    textbuf::insertAt(blocks_[static_cast<size_t>(para)].cp_text, at, cps);
    // 区间是活的：端点跟随插入移动（在控件内插则变长，在控件前插则平移）。
    //   漏掉这一步，控件会与它标注的内容脱节：既拒绝本该允许的输入，
    //   又允许本该拒绝的，且区间看起来仍然有效。
    adjustControlsOnInsert(controls_, para, at, cps.size(), owner_hint);
    caret_ = TextPos{para, at + cps.size()};
    // 内容只在这一段变了：下一轮排版从这个段续排，前面的段原样复用。
    markLayoutDirty(para);
    return true;
}

bool EditorEngine::insertCodepoint(int para, size_t offset, uint32_t cp) {
    return insertCodepoints(para, offset, std::vector<uint32_t>{cp});
}

bool EditorEngine::insertTextUtf8(int para, size_t offset, const std::string& utf8) {
    return insertCodepoints(para, offset, utf8ToCodepoints(utf8));
}

bool EditorEngine::typeTextUtf8(const std::string& utf8) {
    return insertCodepoints(caret_.para, caret_.offset, utf8ToCodepoints(utf8));
}

bool EditorEngine::backspace() {
    if (blocks_.empty()) return false;
    TextPos p = caret_;
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);
    // 段首退格 = 并入上一段（跨段编辑）。策略拒绝时返回 false，与其它被拒编辑同一口径：
    //   宿主据此判断"没变"，光标也不会动。
    if (p.offset == 0) return mergeWithPreviousParagraph();

    // 往回删一个簇，不是删一个码点。
    //   e + U+0301 会被一次删掉；只删 U+0301 会留下一个孤立的组合重音。
    const size_t at = snapToCluster(p.para, p.offset - 1);
    // 策略：整个删除区间必须落在同一个可编辑控件内。
    // 这一条同时挡掉两个方向 —— 从控件外往里删、从控件里往外删。
    if (!EditPolicy::canErase(controls_, policy_.mode(), TextPos{p.para, at},
                              TextPos{p.para, p.offset})) {
        ++blocked_edits_;
        return false;
    }
    const size_t erased =
        textbuf::eraseRange(blocks_[static_cast<size_t>(p.para)].cp_text, at, p.offset);
    if (erased == 0) return false;
    adjustControlsOnErase(controls_, p.para, at, p.offset);
    markLayoutDirty(p.para);
    caret_ = TextPos{p.para, at};
    return true;
}

bool EditorEngine::deleteForward() {
    if (blocks_.empty()) return false;
    TextPos p = caret_;
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);

    const size_t len = blocks_[static_cast<size_t>(p.para)].cp_text.size();
    // 段末 Delete = 把下一段并进来（与"段首退格并入上一段"对称）。
    if (p.offset >= len) return mergeWithNextParagraph();

    size_t next = p.offset + 1;
    if (p.para < static_cast<int>(layout_.para_cluster_starts.size()) &&
        !layout_.para_cluster_starts[static_cast<size_t>(p.para)].empty()) {
        const size_t ceil_cp =
            clusterCeil(layout_.para_cluster_starts[static_cast<size_t>(p.para)], p.offset);
        if (ceil_cp > p.offset) next = ceil_cp;
    }
    if (next > len) next = len;

    if (!EditPolicy::canErase(controls_, policy_.mode(), TextPos{p.para, p.offset},
                              TextPos{p.para, next})) {
        ++blocked_edits_;
        return false;
    }

    const size_t erased =
        textbuf::eraseRange(blocks_[static_cast<size_t>(p.para)].cp_text, p.offset, next);
    if (erased == 0) return false;
    adjustControlsOnErase(controls_, p.para, p.offset, next);
    markLayoutDirty(p.para);
    caret_ = TextPos{p.para, p.offset};
    return true;
}

/**
 * 回车：在光标处把这一段拆成两段（跨段编辑）。
 *
 * 新段的样式复制自原段（字体、字号、书写模式、行距、段后距、颜色、回退链、语言），
 * 只有 page_break_before 刻意清掉：段落分隔符不该附带一次翻页 ——
 * 否则在文档中间按一次回车会把后面整篇推到新页上。
 */
bool EditorEngine::splitParagraph() {
    if (blocks_.empty()) return false;
    TextPos p = caret_;
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);

    if (!EditPolicy::canSplit(controls_, policy_.mode(), p)) {
        ++blocked_edits_;
        return false;
    }

    BlockSpec& head = blocks_[static_cast<size_t>(p.para)];
    BlockSpec tail = head;  // 样式整体继承，只改文本与翻页标记
    tail.page_break_before = false;
    tail.cp_text.assign(head.cp_text.begin() + static_cast<ptrdiff_t>(p.offset),
                        head.cp_text.end());
    head.cp_text.resize(p.offset);

    // 控件端点先适配、再插段：适配函数按"段号 + 偏移"定位，段一插进去就对不上了。
    adjustControlsOnSplitPara(controls_, p);
    blocks_.insert(blocks_.begin() + p.para + 1, std::move(tail));

    markLayoutDirty(p.para);
    // 段数变了：后缀的段号整体后移，而"后缀复用"的判据是"第 from+1 段开始时的状态" ——
    // 现在第 from+1 段已经是另一段了，那个比较毫无意义（碰巧相等就会把整个后缀错当成
    // 没变）。所以段数一变就禁止复用，只保留"前缀复用 + 从脏段续排"。
    para_count_changed_ = true;
    caret_ = TextPos{p.para + 1, 0};
    return true;
}

/**
 * 合并的一般形式：把 head + 1 段并进 head 段。三种入口（段首退格 / 段末 Delete /
 * 显式调用）只是"从哪一端触发"不同，落到文档上的操作是同一个。
 */
bool EditorEngine::mergeParagraphInto(int head) {
    if (head < 0 || head + 1 >= static_cast<int>(blocks_.size())) return false;
    const size_t prev_len = blocks_[static_cast<size_t>(head)].cp_text.size();
    const TextPos join{head, prev_len};

    if (!EditPolicy::canMerge(controls_, policy_.mode(), join)) {
        ++blocked_edits_;
        return false;
    }

    BlockSpec& prev = blocks_[static_cast<size_t>(head)];
    const std::vector<uint32_t>& moved = blocks_[static_cast<size_t>(head + 1)].cp_text;
    prev.cp_text.insert(prev.cp_text.end(), moved.begin(), moved.end());
    // 端点适配在 erase 之前：它按段号定位，段一删就对不上了。
    adjustControlsOnMergePara(controls_, head, prev_len);
    blocks_.erase(blocks_.begin() + head + 1);

    markLayoutDirty(head);
    para_count_changed_ = true;  // 理由同分段
    caret_ = join;
    return true;
}

bool EditorEngine::mergeWithPreviousParagraph() {
    if (blocks_.empty()) return false;
    TextPos p = caret_;
    if (p.para <= 0 || p.para >= static_cast<int>(blocks_.size())) return false;
    // 只有光标确实在段首才合并（否则退格该删的是前一个簇）。
    if (snapToCluster(p.para, p.offset) != 0) return false;
    return mergeParagraphInto(p.para - 1);
}

bool EditorEngine::mergeWithNextParagraph() {
    if (blocks_.empty()) return false;
    TextPos p = caret_;
    if (p.para < 0 || p.para + 1 >= static_cast<int>(blocks_.size())) return false;
    const size_t len = blocks_[static_cast<size_t>(p.para)].cp_text.size();
    if (snapToCluster(p.para, p.offset) != len) return false;
    return mergeParagraphInto(p.para);
}

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
 */

void EditorEngine::setControls(std::vector<Control> list) {
    // 旧集合的最靠前段也要算进来：删掉一个控件同样要把它所在的段重排一次，
    // 否则那一处的括号占位会留在旧的排版结果里（括号不画了、宽度却还占着）。
    int dirty_lo = minControlParagraph();

    controls_.clear();
    for (const Control& c : list) controls_.add(c);
    controls_.normalize();

    const int lo_new = minControlParagraph();
    if (lo_new >= 0 && (dirty_lo < 0 || lo_new < dirty_lo)) dirty_lo = lo_new;
    if (dirty_lo >= 0) {
        markLayoutDirty(dirty_lo);
    } else {
        // 新旧都没有控件：占位集合恒为空，排版结果其实没变。这里按全量处理，
        // 是因为没有可定位的影响面 —— 用一次多余的排版换"不会漏"。
        markAllLayoutDirty();
    }

    // 控件参与排版了（字段的边界要占一个括号的宽度、把正文挤开），所以必须重排。
    //
    //   顺序：先把光标夹到新集合内、再重排 —— 反过来的话，第一次排版用的
    //   还是旧光标，占位名单就会错一个。
    //
    //   前提：调用本函数时 layout_ 必须已与 blocks_ 同步。
    //     刚 setBlocks 过却还没 relayout 的话，控件区间会投影到上一次的行盒上
    //     （症状："4 个字的字段给了 2 个矩形块"，探针当场抓到过）。
    //     规矩是：谁改了文档，谁负责 relayout。
    relayout();

    // 标记画在帧上、不进底图 → 页指纹不受影响，只要求下一次整帧重贴。
    frame_needs_full_refresh_ = true;
}

void EditorEngine::clearControls() { setControls({}); }

void EditorEngine::setEditMode(EditMode m) {
    if (policy_.mode() == m) return;
    const int plan_before = bracketPlanId();
    policy_.setMode(m);

    // 切模式会改变占位集合本身（文档模式一个都不占、表单模式要占），
    //   所以这里可能也要重排（Focused 档下还有"光标被夹进最近字段"这一层）。
    relayoutIfBracketPlanChanged(plan_before);

    // 切模式只影响帧上的标记（画 / 不画），底图一个字都不变 ——
    //   所以不动页指纹，只要求下一次整帧重贴。
    //   如果哪天标记改回"画进底图"，这里必须换成重算页指纹 ——
    //     否则切回文档模式后括号会残留，而"内容没变 → 跳过重画"会让它一直在。
    //     对应的断言是单测里"切回文档模式后一个像素都不剩"那一条。
    frame_needs_full_refresh_ = true;
}

int EditorEngine::controlAtPt(float x_pt_frame, float y_pt_frame) const {
    TextPos p{};
    if (!hitTestPt(x_pt_frame, y_pt_frame, &p)) return -1;
    // include_end = false：点在内容上才算"点进了这个控件"。
    // 点在字段末尾之后一位、视觉上已经在框外，就不该算。
    return controls_.innermostAt(p, /*include_end=*/false);
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
        //   inlineOffsetFor(lo) 给的是"lo 那个字的左边缘"（对）；
        //   而 inlineOffsetFor(hi) 给的是"hi 那个字的左边缘" —— 那是区间内最后一个字
        //   的左边缘，会整整少掉一个字的宽度（症状：框比字窄一格，看着像没框住）。
        //   右边缘必须取"最后一个 cp < hi 的簇的右边缘"。
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
            if (caret_ < s) {
                target = s;
                found = true;
                break;
            }
        }
        if (!found) target = starts.front();  // 到头绕回第一个（表单里 Tab 就该循环）
    } else {
        for (auto it = starts.rbegin(); it != starts.rend(); ++it) {
            if (*it < caret_) {
                target = *it;
                found = true;
                break;
            }
        }
        if (!found) target = starts.back();
    }
    if (target == caret_) return false;
    const int plan_before = bracketPlanId();
    caret_ = target;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

/**
 * 表单标记的"计划"指纹：编辑模式 + （Focused 档下的）聚焦字段 + 每个控件的投影矩形。
 * 这就是 presentInternal 里"变了就整帧重贴"的判据。
 * 它必须只包含会影响标记的东西 —— 多塞了会让整帧重贴变得频繁
 * （整帧重贴本身不贵，但它会把增量重绘的账面数字冲掉），
 * 少塞了则会把陈旧标记留在画面上。
 * All 档（默认）下括号与光标无关，所以不把聚焦字段塞进去：
 * 塞了的话光标每跨一次字段就整帧重贴一次 —— 那正是"光标移动 = 真增量"
 * 最想避免的开销，而且换不来任何画面差异（这么写是量出来的，
 * 不是推出来的：记号里多一项，跨字段时的 repainted_pixels 会立刻跳起来）。
 */
uint64_t EditorEngine::markPlanSignature() const {
    uint64_t h = kFnvBasis;
    h = fnvMix(h, static_cast<uint64_t>(static_cast<int>(policy_.mode())));
    if (cfg_.form_brackets == FormBracketScope::Focused) {
        h = fnvMix(h, static_cast<uint64_t>(static_cast<int64_t>(focusedControlIndex() + 1)));
    }
    for (int ci = 0; ci < controls_.size(); ++ci) {
        const Control& c = controls_.at(ci);
        h = fnvMix(h, c.read_only ? 1ULL : 0ULL);
        for (const ControlRect& cr : controlRects(ci)) {
            h = fnvMix(h, static_cast<uint64_t>(cr.page));
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(cr.r_pt.x))));
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(cr.r_pt.y))));
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(cr.r_pt.w))));
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(cr.r_pt.h))));
        }
    }
    return h;
}

/**
 * Tab / Shift+Tab：缩进与反缩进
 * 语义先说清楚：Tab 是缩进，不是"跳到下一个控件"。
 * 表单字段之间用鼠标点、或者方向键（光标本来就会跨字段）。理由写在
 * EngineConfig::tab_spaces 那里。
 * 实现上刻意复用"敲字 / 退格"那两条路径：这样策略检查、区间伸缩、
 * 重排、重绘、记账全部自动一致 —— 缩进不该是一条特权通道，
 * 它必须和手打 4 个空格得到的结果逐字节相同（探针里有这条断言）。
 */
bool EditorEngine::indent() {
    const int n = std::max(0, cfg_.tab_spaces);
    if (n == 0) return false;
    std::string spaces(static_cast<size_t>(n), ' ');
    return typeTextUtf8(spaces);
}

bool EditorEngine::outdent() {
    if (blocks_.empty()) return false;
    TextPos p = caret_;
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);

    const std::vector<uint32_t>& cps = blocks_[static_cast<size_t>(p.para)].cp_text;
    // 光标前连续空格的起点（最多 tab_spaces 个；只在本段内找，
    // 不跨段回退 —— 跨段删空格会把上一段的段尾吃掉，那是不对的）。
    size_t at = p.offset;
    int n = 0;
    while (at > 0 && n < std::max(0, cfg_.tab_spaces) && cps[at - 1] == 0x20u) {
        --at;
        ++n;
    }
    if (n == 0) return false;

    // 与 backspace 同一条策略：删掉的区间必须落在同一个可编辑控件内。
    if (!EditPolicy::canErase(controls_, policy_.mode(), TextPos{p.para, at},
                              TextPos{p.para, p.offset})) {
        ++blocked_edits_;
        return false;
    }
    const size_t erased =
        textbuf::eraseRange(blocks_[static_cast<size_t>(p.para)].cp_text, at, p.offset);
    if (erased == 0) return false;
    adjustControlsOnErase(controls_, p.para, at, p.offset);
    markLayoutDirty(p.para);
    caret_ = TextPos{p.para, at};
    return true;
}

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
    //   事件），用它 putImageData 只会贴上画布一小块，形成"引擎帧是新的、画布是新旧
    //   混合"。
    //
    //   规矩（整个 wasm 接口面都遵守）：凡是会改引擎状态的入口，返回宿主前必须把
    //   画面收尾。typeText/backspace/moveCaret
    //   都是 *AndRender，唯独 focusControl 漏了。
    repaintAfterEdit();
    return true;
}

bool EditorEngine::moveCaretAndRender(int dcp) {
    if (!moveCaretBy(dcp)) return false;
    // 光标移动不重排 —— 这就是增量的意义：
    //   底图一个字都没变，只需要把两个小矩形贴回去。
    //
    // 这里用裸 repaint()，刻意不把光标拉成可见（repaintAfterEdit 才会）。
    //   理由：移动光标不是编辑，光标该继续按自己的相位闪；把它强行点亮会让
    //   "按着方向键"时看不出闪烁。而"隐藏相位 + 宿主补救一次 present"这条
    //   双 present 路径现在是安全的 —— 上屏走 takeBlitBox() 的累加器，
    //   第二次的小盒子挤不掉第一次的账（见 takeBlitBox 的注释、单测 [12]）。
    //   顺带一条：单测里"光标隐藏时写入量更小（只擦不画）"这条断言，
    //   量的正是这条路径 —— 把它改成 repaintAfterEdit 会让它失去意义。
    repaint();
    return true;
}

bool EditorEngine::moveCaretToLineStartAndRender() {
    if (!moveCaretToLineStart()) return false;
    repaint();  // 不重排 —— 和 moveCaretAndRender 同一条增量路径（也不点亮光标，同上）
    return true;
}

bool EditorEngine::moveCaretToLineEndAndRender() {
    if (!moveCaretToLineEnd()) return false;
    repaint();  // 同 moveCaretAndRender：不重排、不点亮光标
    return true;
}

/** 命中测试：几何的反向半边 —— 帧坐标反查 (段, 偏移) + 归属控件。 */

bool EditorEngine::hitTestPt(float x_pt_frame, float y_pt_frame, TextPos* out,
                             int* out_owner) const {
    if (out_owner) *out_owner = -1;
    if (!out || layout_.lines.empty()) return false;

    const float page_w = cfg_.page.width_pt;
    const float page_h = cfg_.page.height_pt;
    const float stride = page_h + cfg_.page_gap_pt;

    int page = 0;
    float local_y = y_pt_frame;
    if (stride > 0.0f) {
        page = static_cast<int>(std::floor(y_pt_frame / stride));
        if (page < 0) page = 0;
        local_y = y_pt_frame - static_cast<float>(page) * stride;
        // 落在页间空隙里，或者横向出了页外 —— 没有"这一页的哪一行"可言
        if (local_y < 0.0f || local_y > page_h) return false;
        if (x_pt_frame < 0.0f || x_pt_frame > page_w) return false;
    }

    // 块轴：找同一页里最近的一行。
    const LineBox* best = nullptr;
    float best_d = 1e30f;
    for (const LineBox& lb : layout_.lines) {
        if (lb.page != page) continue;
        const float bp = lb.vertical ? x_pt_frame : local_y;
        const float d = (bp < lb.block_lo_pt) ? (lb.block_lo_pt - bp)
                                              : (bp > lb.block_hi_pt ? bp - lb.block_hi_pt : 0.0f);
        if (d < best_d) {
            best_d = d;
            best = &lb;
        }
    }
    if (!best) return false;

    // 行内轴：找最近的插入点。
    //
    //   每个插入点的"势力范围"是它到相邻插入点的中点。
    //   落在某个簇的后半段时，光标会跳到下一个簇的首码点 ——
    //     这就是"字素不可切分"的正确语义：点 e+U+0301 的右半边，
    //     光标跑到它后面，而不是钻进它内部。
    const float ip = best->vertical ? local_y : x_pt_frame;
    const float rel = ip - best->inline_origin_pt;
    const size_t n = best->clusters.size();

    size_t cp = best->last_cp;  // 默认：落在最后半个簇之后 → 行末
    // 命中的簇下标（-1 = 落在行末，没有簇）。归属判定要用它 —— 占位簇自己记着
    //   自己属于哪个控件，那正是"用户点的是哪一枚括号"的答案。
    size_t hit_i = best->clusters.size();
    if (rel < 0.0f) {
        cp = n ? best->clusters.front().cp : best->first_cp;
        hit_i = n ? 0 : best->clusters.size();
    } else {
        for (size_t i = 0; i < n; ++i) {
            const float a = best->clusters[i].inline_pt;
            const float b = (i + 1 < n) ? best->clusters[i + 1].inline_pt : best->inline_end_pt;
            if (rel < (a + b) * 0.5f) {
                cp = best->clusters[i].cp;
                hit_i = i;
                break;
            }
        }
    }

    TextPos p{best->paragraph, cp};
    // 归属：点到的这一簇如果是占位括号，就是那枚括号的主人；
    //   否则（点在真实的字上）取"该位置的内容所属的控件"（半开区间）——
    //   于是点字段第一个字的左半边落在它的开头、归它，而不是归前一个字段。
    //   这两条合起来给出"用户在屏幕上点的是哪一个字段"，与偏移一起交给宿主。
    int owner = -1;
    if (controls_.size() > 0) {
        // 落在最后一个簇之后（行末右侧）时，算作点到了最后一个簇 ——
        //   段尾字段的闭括号占位正好挂在那一头，这样"点字段右括号右边"仍然算它。
        if (hit_i >= best->clusters.size() && !best->clusters.empty())
            hit_i = best->clusters.size() - 1;
        const bool on_slot = hit_i < best->clusters.size() && best->clusters[hit_i].placeholder &&
                             best->clusters[hit_i].owner_control >= 0;
        owner = on_slot ? best->clusters[hit_i].owner_control
                        : controls_.innermostAt(p, /*include_end=*/false);
    }
    // 命中即落点：光标位置不受控件约束，所以这里不再做"就近吸附到最近字段"。
    //   吸附会把"点字段外正文"改写成"停在字段端点"，于是画出来的光标不在点的地方，
    //   表象就是"点了没反应"。归属照实返回，编辑侧自己会按策略拒绝。
    if (out_owner) *out_owner = owner;
    *out = p;
    return true;
}




}  // namespace ced
