/**
 * 宿主 API：取值、设值、批量与属性（见 docs/文档模型设计.md 第七节"值的读写"）。
 *
 * 原则是"句柄用 id 与 key，不用节点引用"：listControls 返回轻量描述而不是节点指针 ——
 * 节点会因编辑失效（位置平移、分裂与合并），而 id 与 key 在编辑之后仍然有效。
 *
 * 四个 setter 的入参都是数组，数组本身就是批量；一次调用合成一个事务，于是一次编译、一次标脏、
 * 一次排版、一次上屏、一个撤销步骤。要按 id 与按 key 混着设、或者值与属性一起设，用
 * beginBatch / commitBatch 把几次调用包住 —— 仍然是一个撤销步骤。
 *
 * 属性入口只收四个运行时状态与策略（hidden / readonly / required / structureLock）。设计期属性
 * （controlType / format / options / appearance）不走它 —— 改它们等于重新定义控件；标签也不走它，
 * 标签是内容，改它走正常编辑步骤。
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/doc/control_index.h"
#include "core/doc/control_spec.h"
#include "core/doc/edit_capability.h"
#include "core/doc/node.h"
#include "core/doc/transaction.h"

namespace heditor {

/**
 * 一次取值的结果。文本类控件把内容拼进 text（段落标记拼成换行），选择类控件读属性上的值。
 */
struct Reading {
    bool ok = false;
    std::string error;
    ValueShape shape = ValueShape::Text;
    std::string text;                // Text 的内容、Enum 的选项值、Resource 的资源 id
    bool flag = false;               // Bool
    std::vector<std::string> items;  // List
    bool inconsistent = false;       // 按 key 取时：同 key 的值不一致
};

/**
 * 控件描述：listControls 与 getProperties 的条目。轻量，不含节点指针。
 */
struct ControlInfo {
    NodeId id = kInvalidNodeId;
    std::string key;
    std::string controlType;
    ValueShape shape = ValueShape::Text;
    bool readonly = false;
    bool required = false;
    bool hidden = false;
    bool structureLock = true;
    Appearance appearance;
    std::string validateRef;
    Reading value;
};

/**
 * 一次设值请求。按 id 精确（只改那一个控件）或按 key 覆盖同编码的全部。
 */
struct ValueEdit {
    NodeId id = kInvalidNodeId;
    std::string key;
    ControlValue value;
};

/**
 * 一次设属性请求。三个属性各是可选值：只改给出来的那几项，没给的不动（三态）。
 * 同一批里同一个 (控件, 属性) 出现多次时按出现顺序后者覆盖前者。
 */
struct PropEdit {
    NodeId id = kInvalidNodeId;
    std::string key;
    std::optional<bool> hidden;
    std::optional<bool> readonly;
    std::optional<bool> required;
    std::optional<bool> structureLock;
};

/**
 * 一次批量调用的结果。changed 数的是控件数（属性按项数），用来验证"设 1000 个与设 1 个
 * 是同一量级"这条判据。
 */
struct BatchResult {
    bool ok = false;
    std::string error;
    int changed = 0;
    /**
     * 这一批改了流结构（含 hidden），调用方要重排：呈现态的流位置与段落序号都会变。
     * 只改 readonly / required / structureLock 的批次不重排 —— 它们不改结构。
     */
    bool needs_relayout = false;
};

/**
 * 取一个控件的值。文本类递归拼接整棵子树的可见文字（含嵌套控件内部的文字）。
 */
Reading getValue(const NodeTree& tree, NodeId id);

/**
 * 按 key 取。同 key 有多个控件时取文档顺序的第一个，并把不一致标出来 —— 不静默取一个：
 * 不一致是合法状态（一个 key 可以绑多个控件），但用户与宿主都该能发现它。
 */
Reading getValueByKey(const NodeTree& tree, const ControlIndex& index, const std::string& key);

/**
 * 同 key 值不一致的清单，每个 key 一条（按 key 去重）。宿主拿它做一次提示，不弹窗、不阻塞。
 */
std::vector<std::string> inconsistentKeys(const NodeTree& tree, const ControlIndex& index);

/**
 * 列出全部控件（文档顺序）。
 */
std::vector<ControlInfo> listControls(const NodeTree& tree);

/**
 * 读回属性：与 listControls 同一份描述，只是可以按 id 过滤。ids 为空表示"全部"。
 */
std::vector<ControlInfo> getProperties(const NodeTree& tree, const std::vector<NodeId>& ids);

/**
 * 宿主门面。持有文档、撤销栈与控件索引的引用，把"一次调用 = 一个事务"这件事收在一处。
 *
 * 索引在这里面维护（提交之后把每个步骤喂给 ControlIndex 的 onStep），所以调用方不必手工同步；
 * 跟不上的步骤（序列化还原出来的 InsertNode）会触发一次重建。
 */
class ControlApi {
public:
    ControlApi(NodeTree& tree, UndoStack& undo, ControlIndex& index);

    /**
     * 只读的文档视图。裁决门要拿它确认等待期间目标有没有失效（控件被删、被换成别的元素）。
     */
    const NodeTree& tree() const { return tree_; }

    std::vector<ControlInfo> listControls() const;

    /**
     * 取指定几个控件的值。ids 为空表示"全部"。
     */
    std::vector<Reading> getValues(const std::vector<NodeId>& ids) const;

    Reading getValueByKey(const std::string& key) const;

    /**
     * 读回属性（与 listControls 同一份描述，只是可按 id 过滤）。ids 为空表示"全部"。
     */
    std::vector<ControlInfo> getProperties(const std::vector<NodeId>& ids) const;

    /**
     * 同 key 值不一致的清单，每个 key 一条。
     */
    std::vector<std::string> inconsistentKeys() const;

    BatchResult setValues(const std::vector<ValueEdit>& edits);
    BatchResult setValuesByKey(const std::vector<ValueEdit>& edits);
    BatchResult setProperties(const std::vector<PropEdit>& edits);
    BatchResult setPropertiesByKey(const std::vector<PropEdit>& edits);

    /**
     * 把随后的几次调用并成一个事务。beginBatch 之后各 setter 只攒步骤，commitBatch 一次提交。
     */
    void beginBatch();

    /**
     * 提交攒下的步骤。没有攒下任何步骤时算成功（幂等）。
     */
    BatchResult commitBatch();

private:
    /**
     * 设值的共同实现。by_key 为真时只按 key 解析目标（忽略 edit.id），为假时按 id 或 key。
     */
    BatchResult applyValues(const std::vector<ValueEdit>& edits, bool by_key);

    BatchResult applyProps(const std::vector<PropEdit>& edits, bool by_key);

    /**
     * 把 target 的内容整体换成 text（文本类控件专用）。内部不是纯文本时拒绝并给原因。
     */
    bool buildTextReplace(NodeId target, const std::string& text, std::vector<Step>* steps,
                          std::string* error);

    /**
     * 提交攒下的步骤：一个事务、一次规范化，并把索引跟上。
     */
    BatchResult flush();

    NodeTree& tree_;
    UndoStack& undo_;
    ControlIndex& index_;

    std::vector<Step> pending_;
    BatchResult pending_result_;
    bool in_batch_ = false;
};

}  // namespace heditor
