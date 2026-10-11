#include "core/doc/style.h"

#include <algorithm>

namespace heditor {

namespace {

/**
 * 一条样式链，按加入顺序从叶到根。叶在前（它最后覆盖）。
 *
 * 重复出现同一个名字就是成环；那时在重复处截断并记一条诊断，而不是转圈或抛出去 ——
 * 成环是文档损坏，但"能算多少算多少再加一条可发现的信号"比整篇算不出来有用。
 */
std::vector<const StyleDef*> chainOf(const StyleSheet& sheet, StyleKind kind,
                                     const std::string& name, int limit,
                                     std::vector<std::string>* diagnostics) {
    std::vector<const StyleDef*> chain;
    std::string cursor = name;
    while (!cursor.empty()) {
        if (static_cast<int>(chain.size()) >= limit) {
            if (diagnostics != nullptr) {
                diagnostics->push_back("样式继承链超过 " + std::to_string(limit) + " 层，在 " +
                                       cursor + " 处截断");
            }
            break;
        }
        const StyleDef* def = sheet.find(kind, cursor);
        if (def == nullptr) {
            if (diagnostics != nullptr) {
                diagnostics->push_back(std::string(styleKindName(kind)) + "样式不存在：" + cursor +
                                       "，按已解析到的部分降级");
            }
            break;
        }
        const bool seen = std::any_of(chain.begin(), chain.end(),
                                      [def](const StyleDef* d) { return d->name == def->name; });
        if (seen) {
            if (diagnostics != nullptr) {
                diagnostics->push_back("样式继承成环：" + cursor + " 已在链上，在此截断");
            }
            break;
        }
        chain.push_back(def);
        cursor = def->based_on;
    }
    return chain;
}

/**
 * 把一条链叠成两个 patch：从根往叶（链尾往链首），叶最后叠上所以它说了算。
 *
 * 两个结果都是可空的：解析段落格式时只叠段落级属性，解析字符格式时只叠字符级属性。
 */
void foldChain(const std::vector<const StyleDef*>& chain, ParaFormatPatch* para, CharFormat* run) {
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (para != nullptr) *para = mergeParaFormatPatch(*para, (*it)->para);
        if (run != nullptr) *run = mergeCharFormat(*run, (*it)->run);
    }
}

}  // namespace

const char* styleKindName(StyleKind kind) {
    switch (kind) {
        case StyleKind::Paragraph:
            return "段落";
        case StyleKind::Character:
            return "字符";
    }
    return "（未知）";
}

void StyleSheet::setDocDefaults(ParaFormatPatch para, CharFormat run) {
    doc_defaults_para_ = std::move(para);
    doc_defaults_run_ = std::move(run);
    ++version_;
}

bool StyleSheet::add(const StyleDef& def) {
    std::vector<StyleDef>& table = def.kind == StyleKind::Paragraph ? paragraphs_ : characters_;
    const auto it = std::find_if(table.begin(), table.end(),
                                 [&def](const StyleDef& s) { return s.name == def.name; });
    bool replaced = false;
    if (it == table.end()) {
        table.push_back(def);
    } else {
        *it = def;
        replaced = true;
    }
    ++version_;
    return replaced;
}

bool StyleSheet::remove(StyleKind kind, const std::string& name) {
    std::vector<StyleDef>& table = kind == StyleKind::Paragraph ? paragraphs_ : characters_;
    const auto it = std::find_if(table.begin(), table.end(),
                                 [&name](const StyleDef& s) { return s.name == name; });
    if (it == table.end()) return false;
    table.erase(it);
    ++version_;
    return true;
}

const StyleDef* StyleSheet::find(StyleKind kind, const std::string& name) const {
    const std::vector<StyleDef>& table = kind == StyleKind::Paragraph ? paragraphs_ : characters_;
    const auto it = std::find_if(table.begin(), table.end(),
                                 [&name](const StyleDef& s) { return s.name == name; });
    return it == table.end() ? nullptr : &*it;
}

std::vector<std::string> StyleSheet::names(StyleKind kind) const {
    const std::vector<StyleDef>& table = kind == StyleKind::Paragraph ? paragraphs_ : characters_;
    std::vector<std::string> out;
    out.reserve(table.size());
    for (const StyleDef& s : table) out.push_back(s.name);
    return out;
}

ParaFormat resolveParagraphFormat(const StyleSheet& sheet, const std::string& style_name,
                                  const ParaFormatPatch& inline_patch,
                                  std::vector<std::string>* diagnostics) {
    // 链的层数上限定得比任何真实样式表都宽：它只是防成环与防手工构造的深链把栈吃光。
    const std::vector<const StyleDef*> chain =
        chainOf(sheet, StyleKind::Paragraph, style_name, 64, diagnostics);

    ParaFormatPatch para = sheet.docDefaultsPara();
    foldChain(chain, &para, nullptr);
    para = mergeParaFormatPatch(para, inline_patch);

    // 完整形式带着自己的默认值（12 磅、1.6 倍行距），所以未设置的字段落回它们。
    ParaFormat out;
    applyParaFormatPatch(para, &out);
    return out;
}

CharFormat resolveRunFormat(const StyleSheet& sheet, const std::string& para_style,
                            const std::string& char_style, const CharFormat& inline_marks,
                            std::vector<std::string>* diagnostics) {
    const std::vector<const StyleDef*> para_chain =
        chainOf(sheet, StyleKind::Paragraph, para_style, 64, diagnostics);
    const std::vector<const StyleDef*> char_chain =
        chainOf(sheet, StyleKind::Character, char_style, 64, diagnostics);

    CharFormat run = sheet.docDefaultsRun();
    foldChain(para_chain, nullptr, &run);
    foldChain(char_chain, nullptr, &run);
    return mergeCharFormat(run, inline_marks);
}

StyleCache::StyleCache(const StyleSheet& sheet) : sheet_(sheet), version_(sheet.version()) {}

void StyleCache::syncVersion() {
    if (version_ == sheet_.version()) return;
    // 样式表一改就整体失效：受影响的是全篇引用它的元素，那些元素散在各处，
    // 逐个失效不如整体失效来得可靠（第六节选了整体失效）。
    paragraphs_.clear();
    runs_.clear();
    version_ = sheet_.version();
}

const ParaFormat& StyleCache::paragraph(const NodeTree& tree, NodeId mark_id) {
    syncVersion();
    const auto hit = paragraphs_.find(mark_id);
    if (hit != paragraphs_.end()) {
        ++hits_;
        return hit->second;
    }
    ++computations_;

    static const ParaFormat kFallback;
    const Node* n = tree.find(mark_id);
    if (n == nullptr || !isParagraphMark(n->kind)) {
        diagnostics_.push_back("按段落标记解析的 id 不是段落标记");
        return kFallback;
    }
    const ParaFormat resolved = resolveParagraphFormat(sheet_, n->styleRef, n->para, &diagnostics_);
    return paragraphs_.emplace(mark_id, resolved).first->second;
}

const CharFormat& StyleCache::run(const NodeTree& tree, NodeId run_id,
                                  const std::string& para_style) {
    syncVersion();
    const auto hit = runs_.find(run_id);
    if (hit != runs_.end() && hit->second.first == para_style) {
        ++hits_;
        return hit->second.second;
    }
    ++computations_;

    static const CharFormat kFallback;
    const Node* n = tree.find(run_id);
    if (n == nullptr || !isText(n->kind)) {
        diagnostics_.push_back("按文本解析的 id 不是文本节点");
        return kFallback;
    }
    const CharFormat resolved =
        resolveRunFormat(sheet_, para_style, n->styleRef, n->marks, &diagnostics_);
    runs_[run_id] = {para_style, resolved};
    return runs_[run_id].second;
}

void StyleCache::invalidate(NodeId id) {
    paragraphs_.erase(id);
    runs_.erase(id);
}

void StyleCache::clear() {
    paragraphs_.clear();
    runs_.clear();
    version_ = sheet_.version();
}

}  // namespace heditor
