#include "core/doc/style.h"

namespace heditor {

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
