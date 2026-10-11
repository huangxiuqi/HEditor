#include "core/doc/control_api.h"

#include <algorithm>
#include <map>
#include <utility>

#include "core/doc/subtree.h"
#include "core/utils/utf8.h"

namespace heditor {

namespace {

/**
 * 按元素类型把一段内容的文字拼进 out。取值与将来的选区复制走同一套规则，所以两者不会不一致。
 *
 * 隐藏的单个节点不贡献文字：取值取的是用户在表单与打印里能看到的那份。这个判断只作用于被遍历到
 * 的孩子，所以"读一个自己隐藏着的控件"仍然读得到它的内容（宿主按需决定怎么用）。
 */
void appendVisible(const NodeTree& tree, NodeId id, std::string* out);

void appendVisibleChildren(const NodeTree& tree, const Node* n, std::string* out);

void appendVisible(const NodeTree& tree, NodeId id, std::string* out) {
    const Node* n = tree.find(id);
    if (n == nullptr || n->hidden) return;

    if (isText(n->kind)) {
        for (uint32_t cp : n->text) appendUtf8(out, cp);
        return;
    }
    switch (n->kind) {
        case NodeKind::ParagraphMark:
        case NodeKind::Break:
        case NodeKind::PageBreak:
            out->push_back('\n');
            return;
        case NodeKind::Tab:
            out->push_back('\t');
            return;
        case NodeKind::Field:
        case NodeKind::Image:
        case NodeKind::Formula:
            // 域取缓存值、图片与公式取替代文本 —— 都还没落地，先贡献空串。
            // 与选区复制同一条规则，将来补的时候两处一起补。
            return;
        default:
            break;
    }
    appendVisibleChildren(tree, n, out);
}

/**
 * 容器（含控件）的内容按流顺序拼接。嵌套控件的标签就是它内部的内容，所以自然一起取出来 ——
 * "标签是控件内部的只读内容"这条设计在取值上不需要任何额外机制。
 */
void appendVisibleChildren(const NodeTree& tree, const Node* n, std::string* out) {
    for (const Node* c : n->children) appendVisible(tree, c->id, out);
}

/**
 * 整体设值的前提：内部是纯文本（文本节点与段落标记）。
 *
 * 含子控件时拒绝，不静默把子控件冲掉 —— 整体替换会把它的 key、值、校验设置一起删掉，
 * 而调用方通常只是想改一段文字。图片、公式这类原子落在同一条判据下。
 */
bool pureText(const NodeTree& tree, NodeId id, std::string* why) {
    const Node* n = tree.find(id);
    if (n == nullptr) {
        *why = "控件不存在";
        return false;
    }
    for (const Node* c : n->children) {
        if (c->kind == NodeKind::ContentControl) {
            *why = "控件内含嵌套字段（key=" + c->control.key + "），不能整体设值";
            return false;
        }
        if (!isText(c->kind) && !isParagraphMark(c->kind)) {
            *why = std::string("控件内含 ") + nodeKindName(c->kind) + "，不能整体设值";
            return false;
        }
        if (!pureText(tree, c->id, why)) return false;
    }
    return true;
}

/**
 * 把"构建时算出来的流位置"搬到"这一步真正应用时的坐标系"。
 *
 * 事务的约定是每步的坐标都是它自己那一步开始时的状态（transaction.h），而批量是一次把所有
 * 步骤建出来的 —— 中间那些步骤会改变文档长度，前面某一步改短了，后面几步记的绝对流位置就偏了。
 * 所以每收一步，就把后面要记的流位置按这一步的映射推过去。这不是补丁：坐标属于哪一时刻，
 * 就得按那个时刻算。
 */
FlowPos through(const std::vector<Step>& prev, FlowPos pos) {
    for (const Step& s : prev) pos = mapPos(s, pos, Assoc::Before);
    return pos;
}

/**
 * 收一步，并把它的流位置推到当前时刻。
 */
void pushStep(std::vector<Step>* out, Step step) {
    if (auto* ins = std::get_if<InsertNode>(&step)) {
        ins->at_flow = through(*out, ins->at_flow);
    } else if (auto* rm = std::get_if<RemoveNode>(&step)) {
        rm->at_flow = through(*out, rm->at_flow);
    }
    out->push_back(std::move(step));
}

ControlInfo describe(const NodeTree& tree, NodeId id) {
    const Node* n = tree.find(id);
    ControlInfo info;
    info.id = id;
    info.key = n->control.key;
    info.controlType = n->control.controlType;
    info.shape = n->control.valueShape;
    info.readonly = n->control.readonly;
    info.required = n->control.required;
    info.hidden = n->hidden;
    info.structureLock = n->control.structureLock;
    info.appearance = n->control.appearance;
    info.validateRef = n->control.validateRef;
    info.value = getValue(tree, id);
    return info;
}

Reading readControl(const NodeTree& tree, const Node* n) {
    Reading out;
    out.shape = n->control.valueShape;
    out.ok = true;
    if (out.shape == ValueShape::Text) {
        appendVisibleChildren(tree, n, &out.text);
        return out;
    }
    out.flag = n->control.value.flag;
    out.text = n->control.value.text;
    out.items = n->control.value.items;
    return out;
}

/**
 * 两次取值是不是同一个值。按 key 判断一致性用它。
 */
bool sameReading(const Reading& a, const Reading& b) {
    return a.shape == b.shape && a.flag == b.flag && a.text == b.text && a.items == b.items;
}

/**
 * 值编辑的目标解析：按 key 时展开成同 key 的全部控件（文档顺序）。
 */
void resolveTargets(const ControlIndex& index, const ValueEdit& e, bool by_key,
                    std::vector<NodeId>* out) {
    if (by_key || e.id == kInvalidNodeId) {
        for (NodeId id : index.byKey(e.key)) out->push_back(id);
        return;
    }
    out->push_back(e.id);
}

/**
 * 属性编辑的目标解析，同上。
 */
void resolveTargets(const ControlIndex& index, const PropEdit& e, bool by_key,
                    std::vector<NodeId>* out) {
    if (by_key || e.id == kInvalidNodeId) {
        for (NodeId id : index.byKey(e.key)) out->push_back(id);
        return;
    }
    out->push_back(e.id);
}

/**
 * 顺序去重：同一个目标出现多次时只留最后一次（后者覆盖前者，与逐条调用一致）。
 * 返回保留下来的下标，按原顺序。
 */
std::vector<int> keepLastPerTarget(const std::vector<NodeId>& resolved) {
    std::map<NodeId, int> last;
    for (size_t i = 0; i < resolved.size(); ++i) last[resolved[i]] = static_cast<int>(i);

    std::vector<int> kept;
    for (size_t i = 0; i < resolved.size(); ++i) {
        if (last[resolved[i]] == static_cast<int>(i)) kept.push_back(static_cast<int>(i));
    }
    return kept;
}

}  // namespace

Reading getValue(const NodeTree& tree, NodeId id) {
    Reading out;
    const Node* n = tree.find(id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) {
        out.error = "取值的对象不是内容控件";
        return out;
    }
    return readControl(tree, n);
}

Reading getValueByKey(const NodeTree& tree, const ControlIndex& index, const std::string& key) {
    Reading out;
    const std::vector<NodeId>& ids = index.byKey(key);
    if (ids.empty()) {
        out.error = "没有这个业务编码的控件：" + key;
        return out;
    }
    out = getValue(tree, ids.front());
    for (size_t i = 1; i < ids.size(); ++i) {
        const Reading other = getValue(tree, ids[i]);
        if (!sameReading(out, other)) {
            out.inconsistent = true;
            break;
        }
    }
    return out;
}

std::vector<std::string> inconsistentKeys(const NodeTree& tree, const ControlIndex& index) {
    std::vector<std::string> out;
    for (NodeId id : controlIds(tree)) {
        const std::string* key = index.keyOf(id);
        if (key == nullptr) continue;
        if (out.end() != std::find(out.begin(), out.end(), *key)) continue;

        const std::vector<NodeId>& ids = index.byKey(*key);
        if (ids.size() < 2) continue;
        const Reading first = getValue(tree, ids.front());
        for (size_t i = 1; i < ids.size(); ++i) {
            if (!sameReading(first, getValue(tree, ids[i]))) {
                out.push_back(*key);
                break;
            }
        }
    }
    return out;
}

std::vector<ControlInfo> listControls(const NodeTree& tree) {
    std::vector<ControlInfo> out;
    for (NodeId id : controlIds(tree)) out.push_back(describe(tree, id));
    return out;
}

std::vector<ControlInfo> getProperties(const NodeTree& tree, const std::vector<NodeId>& ids) {
    std::vector<ControlInfo> out;
    if (ids.empty()) return listControls(tree);
    for (NodeId id : ids) {
        const Node* n = tree.find(id);
        if (n == nullptr || n->kind != NodeKind::ContentControl) continue;
        out.push_back(describe(tree, id));
    }
    return out;
}

ControlApi::ControlApi(NodeTree& tree, UndoStack& undo, ControlIndex& index)
    : tree_(tree), undo_(undo), index_(index) {}

std::vector<ControlInfo> ControlApi::listControls() const {
    std::vector<ControlInfo> out;
    for (NodeId id : controlIds(tree_)) out.push_back(describe(tree_, id));
    return out;
}

std::vector<Reading> ControlApi::getValues(const std::vector<NodeId>& ids) const {
    std::vector<Reading> out;
    if (ids.empty()) {
        for (NodeId id : controlIds(tree_)) out.push_back(getValue(tree_, id));
        return out;
    }
    for (NodeId id : ids) out.push_back(getValue(tree_, id));
    return out;
}

Reading ControlApi::getValueByKey(const std::string& key) const {
    return heditor::getValueByKey(tree_, index_, key);
}

std::vector<ControlInfo> ControlApi::getProperties(const std::vector<NodeId>& ids) const {
    std::vector<ControlInfo> out;
    if (ids.empty()) {
        for (NodeId id : controlIds(tree_)) out.push_back(describe(tree_, id));
        return out;
    }
    for (NodeId id : ids) {
        if (tree_.find(id) == nullptr || !index_.has(id)) continue;
        out.push_back(describe(tree_, id));
    }
    return out;
}

std::vector<std::string> ControlApi::inconsistentKeys() const {
    return heditor::inconsistentKeys(tree_, index_);
}

bool ControlApi::buildTextReplace(NodeId target, const std::string& text, std::vector<Step>* steps,
                                  std::string* error) {
    const Node* n = tree_.find(target);
    if (n == nullptr || n->kind != NodeKind::ContentControl) {
        *error = "设值的对象不是内容控件";
        return false;
    }
    if (n->control.valueShape != ValueShape::Text) {
        *error = std::string("这个控件的值在属性上（") + valueShapeName(n->control.valueShape) +
                 "），不是内容";
        return false;
    }
    if (!pureText(tree_, target, error)) return false;

    // 先把现有内容逐个摘掉（从后往前，前面的下标才不失效），再插一个新文本节点。
    // 用节点级步骤而不是区间删除：内容里可能有段落标记，逐个子节点摘更直接，也不受
    // "区间编辑只认文本与段落标记"的限制。
    const Node* ctrl = tree_.find(target);
    for (int i = static_cast<int>(ctrl->children.size()) - 1; i >= 0; --i) {
        const NodeBuild b = makeRemoveNode(tree_, Pos{target, i});
        if (!b.ok) {
            *error = b.error;
            return false;
        }
        steps->push_back(b.step);
    }
    if (!text.empty()) {
        NodeSpec spec;
        spec.kind = NodeKind::TextRun;
        for (size_t i = 0; i < text.size();) {
            size_t next = i;
            const uint32_t cp = decodeUtf8(text, i, &next);
            spec.text.push_back(cp);
            i = next == i ? i + 1 : next;
        }
        const NodeBuild b = makeInsertNode(tree_, Pos{target, 0}, spec);
        if (!b.ok) {
            *error = b.error;
            return false;
        }
        steps->push_back(b.step);
    }
    return true;
}

BatchResult ControlApi::flush() {
    BatchResult out = pending_result_;
    out.ok = true;

    if (!pending_.empty()) {
        Transaction tx;
        tx.steps = pending_;
        tx.label = "批量设值";
        const StepResult r = undo_.commit(std::move(tx));
        if (!r.ok) {
            out.ok = false;
            out.error = r.error;
            pending_.clear();
            pending_result_ = BatchResult{};
            return out;
        }
        // 索引跟上：提交之后按步骤维护；跟不上的那一步触发一次重建。
        for (const Step& s : pending_) {
            if (!index_.onStep(tree_, s)) {
                index_.rebuild(tree_);
                break;
            }
        }
    }

    pending_.clear();
    pending_result_ = BatchResult{};
    return out;
}

void ControlApi::beginBatch() { in_batch_ = true; }

BatchResult ControlApi::commitBatch() {
    in_batch_ = false;
    return flush();
}

BatchResult ControlApi::applyValues(const std::vector<ValueEdit>& edits, bool by_key) {
    BatchResult out;

    // 先解析目标并顺序去重：同一个控件出现多次时只留最后一次。
    std::vector<int> owner;
    std::vector<NodeId> resolved;
    for (const ValueEdit& e : edits) {
        std::vector<NodeId> one;
        resolveTargets(index_, e, by_key, &one);
        for (NodeId id : one) {
            resolved.push_back(id);
            owner.push_back(static_cast<int>(&e - edits.data()));
        }
    }
    if (resolved.empty()) {
        out.ok = true;  // 没有目标：与"改了 0 个"同义
        return out;
    }
    const std::vector<int> kept = keepLastPerTarget(resolved);

    std::vector<Step> steps;
    for (int k : kept) {
        const ValueEdit& e = edits[static_cast<size_t>(owner[static_cast<size_t>(k)])];
        const NodeId target = resolved[static_cast<size_t>(k)];
        const Node* n = tree_.find(target);
        if (n == nullptr || n->kind != NodeKind::ContentControl) {
            out.error = "设值的目标不是内容控件";
            return out;
        }

        std::vector<Step> one;
        if (n->control.valueShape == ValueShape::Text) {
            std::string why;
            if (!buildTextReplace(target, e.value.text, &one, &why)) {
                out.error = why;
                return out;
            }
        } else {
            // 选择类的值在记录上，一条步骤改掉，与属性变更共用同一条撤销路径。
            SetControlValue s;
            s.id = target;
            s.value = e.value;
            s.before = n->control.value;
            one.push_back(Step{s});
        }
        for (const Step& s : one) pushStep(&steps, s);
        ++out.changed;
    }

    if (in_batch_) {
        for (const Step& s : steps) pending_.push_back(s);
        pending_result_.changed += out.changed;
        out.ok = true;
        return out;
    }

    pending_ = steps;
    pending_result_ = BatchResult{};
    BatchResult committed = flush();
    committed.changed = out.changed;
    return committed;
}

BatchResult ControlApi::applyProps(const std::vector<PropEdit>& edits, bool by_key) {
    BatchResult out;

    struct Item {
        NodeId id = kInvalidNodeId;
        ControlProp prop = ControlProp::Readonly;
        bool value = false;
    };
    std::vector<Item> wanted;

    for (const PropEdit& e : edits) {
        std::vector<NodeId> targets;
        resolveTargets(index_, e, by_key, &targets);
        const std::pair<bool, ControlProp> fields[4] = {
            {e.hidden.has_value(), ControlProp::Hidden},
            {e.readonly.has_value(), ControlProp::Readonly},
            {e.required.has_value(), ControlProp::Required},
            {e.structureLock.has_value(), ControlProp::StructureLock},
        };
        const std::optional<bool> values[4] = {e.hidden, e.readonly, e.required, e.structureLock};
        for (NodeId id : targets) {
            for (int i = 0; i < 4; ++i) {
                if (!fields[i].first) continue;
                Item item;
                item.id = id;
                item.prop = fields[i].second;
                item.value = values[i].value();
                wanted.push_back(item);
            }
        }
    }

    // 同一个 (控件, 属性) 出现多次时只留最后一次。
    std::map<std::pair<NodeId, int>, int> last;
    for (size_t i = 0; i < wanted.size(); ++i) {
        last[{wanted[i].id, static_cast<int>(wanted[i].prop)}] = static_cast<int>(i);
    }

    std::vector<Step> steps;
    for (size_t i = 0; i < wanted.size(); ++i) {
        const Item& item = wanted[i];
        if (last[{item.id, static_cast<int>(item.prop)}] != static_cast<int>(i)) continue;

        const Node* n = tree_.find(item.id);
        if (n == nullptr || n->kind != NodeKind::ContentControl) {
            out.error = "设属性的目标不是内容控件";
            return out;
        }
        if (item.prop == ControlProp::Hidden) out.needs_relayout = true;

        SetControlProp s;
        s.id = item.id;
        s.prop = item.prop;
        s.value = item.value;
        switch (item.prop) {
            case ControlProp::Hidden:
                s.before = n->hidden;
                break;
            case ControlProp::Readonly:
                s.before = n->control.readonly;
                break;
            case ControlProp::Required:
                s.before = n->control.required;
                break;
            case ControlProp::StructureLock:
                s.before = n->control.structureLock;
                break;
        }
        steps.push_back(Step{s});
        ++out.changed;
    }
    if (in_batch_) {
        for (const Step& s : steps) pending_.push_back(s);
        pending_result_.changed += out.changed;
        pending_result_.needs_relayout = pending_result_.needs_relayout || out.needs_relayout;
        out.ok = true;
        return out;
    }

    pending_ = steps;
    pending_result_ = BatchResult{};
    BatchResult committed = flush();
    committed.changed = out.changed;
    committed.needs_relayout = out.needs_relayout;
    return committed;
}

BatchResult ControlApi::setValues(const std::vector<ValueEdit>& edits) {
    return applyValues(edits, /*by_key=*/false);
}

BatchResult ControlApi::setValuesByKey(const std::vector<ValueEdit>& edits) {
    return applyValues(edits, /*by_key=*/true);
}

BatchResult ControlApi::setProperties(const std::vector<PropEdit>& edits) {
    return applyProps(edits, /*by_key=*/false);
}

BatchResult ControlApi::setPropertiesByKey(const std::vector<PropEdit>& edits) {
    return applyProps(edits, /*by_key=*/true);
}

}  // namespace heditor
