/**
 * 控件：对外视图、写值与命中。
 *
 * 与图片那份分开，理由是两者的事实来源不同：图片只有"资源表 + 节点上的尺寸"，控件要看
 * 节点上的整份 ControlSpec（类型、值、选项、策略）。写成两个文件，各自只有一件事。
 *
 * 值的写入走命令层的 setControlValue / toggleControl：值与显示内容在同一次提交里改，
 * 撤销一下回到改之前 —— 于是不存在"值改了、画面还是旧的"这一档中间态。
 */
#include <string>
#include <utility>
#include <vector>

#include "core/doc/document.h"
#include "core/engine/control_spans.h"
#include "core/engine/engine.h"

namespace heditor {

namespace {

/**
 * 从节点上取控件记录。
 * @param n 投影节点
 * @return 记录；不是控件为 nullptr
 */
const ControlSpec* specOf(const doc::Node* n) {
    if (n == nullptr || n->kind != NodeKind::ContentControl) return nullptr;
    return std::get_if<ControlSpec>(&n->payload);
}

}  // namespace

std::vector<EditorEngine::ControlView> EditorEngine::controlViews() const {
    std::vector<ControlView> out;
    if (!loaded_) return out;
    out.reserve(control_node_ids_.size());
    for (size_t i = 0; i < control_node_ids_.size(); ++i) {
        const doc::Node* n = doc::findNode(document(), control_node_ids_[i]);
        const ControlSpec* spec = specOf(n);
        if (spec == nullptr) continue;
        ControlView v;
        v.index = static_cast<int>(i);
        v.id = control_node_ids_[i];
        v.key = spec->key;
        v.controlType = spec->controlType;
        v.shape = spec->valueShape;
        v.value = spec->value;
        for (const doc::Ref& child : n->children) {
            if (child->kind != NodeKind::TextRun) continue;
            v.content = codepointsToUtf8(std::get<doc::RunPayload>(child->payload).text);
            break;
        }
        v.readonly = spec->readonly;
        v.required = spec->required;
        v.placeholder = spec->appearance.placeholder;
        if (const ChoiceDetail* c = choiceOf(*spec)) {
            v.options = c->options;
            v.multi = c->multi;
            v.label = c->label;
            v.group = c->group;
            v.glyph_color = spec->appearance.glyph.color;
        } else if (const DropdownDetail* d = dropdownOf(*spec)) {
            v.options = d->options;
            v.free_input = d->free_input;
        } else if (const DateDetail* d = dateOf(*spec)) {
            v.format = d->format;
            v.date_units = d->units;
        }
        out.push_back(std::move(v));
    }
    return out;
}

bool EditorEngine::setControlSpec(int index, const ControlPatch& patch) {
    if (!loaded_) return false;
    return edit_session_.edit.setControlSpec(index, patch);
}

bool EditorEngine::setControlSpecAndRender(int index, const ControlPatch& patch) {
    if (!setControlSpec(index, patch)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::setControlValue(int index, const ControlValue& value) {
    if (!loaded_) return false;
    return edit_session_.edit.setControlValue(index, value);
}

bool EditorEngine::setControlValueAndRender(int index, const ControlValue& value) {
    if (!setControlValue(index, value)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::toggleControl(int index, int option) {
    if (!loaded_) return false;
    return edit_session_.edit.toggleControl(index, option);
}

bool EditorEngine::toggleControlAndRender(int index, int option) {
    if (!toggleControl(index, option)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

int EditorEngine::controlOptionAtPt(float x_pt_doc, float y_pt_doc) const {
    if (!loaded_) return -1;
    TextPos p{};
    int owner = -1;
    int side = 0;
    if (!hitTestPt(x_pt_doc, y_pt_doc, &p, &owner, &side)) return -1;
    if (owner < 0) return -1;
    return controlOptionAtPos(p, side);
}

int EditorEngine::controlOptionAtPos(TextPos p, int side) const {
    if (!loaded_) return -1;
    const FlowPos f = edit_session_.caret.flowPosAtSide(p.para, p.offset, side);
    if (f < 0) return -1;
    FlowPos lo = 0;
    FlowPos hi = 0;
    if (!flow_.has_value()) return -1;
    const int index = resolveInnermostControl(&projection_.structure.controls, *flow_,
                                              control_node_ids_, f, &lo, &hi);
    if (index < 0) return -1;
    const doc::Node* n = doc::findNode(document(), control_node_ids_[static_cast<size_t>(index)]);
    const ControlSpec* spec = specOf(n);
    if (spec == nullptr) return -1;
    std::vector<std::pair<int, int>> spans;
    if (!controlOptionSpans(*spec, &spans)) return -1;
    const int offset = f - lo;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (offset >= spans[i].first && offset < spans[i].second) return static_cast<int>(i);
    }
    // 落在两个选项之间的空隙里：按最近的左边那一个算 —— "点到选项这一行"就该切它，
    //   而不是因为差一个空格判成什么都没点。
    for (size_t i = spans.size(); i > 0; --i) {
        if (offset >= spans[i - 1].second) return static_cast<int>(i - 1);
    }
    return 0;
}

}  // namespace heditor
