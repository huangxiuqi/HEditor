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
#include "core/doc/node.h"
#include "core/doc/transaction.h"
#include "core/rev/control_spec.h"

namespace heditor {
/**
 * 一次取值的结果。文本类控件把内容拼进 text（段落标记拼成换行），选择类控件读属性上的值。
 */
struct Reading {
    /**
     * 取到了没有。
     */
    bool ok = false;

    /**
     * 失败原因（控件不存在、值读不出来）。
     */
    std::string error;

    /**
     * 值形状，决定读下面哪几个字段。
     */
    ValueShape shape = ValueShape::Text;

    /**
     * Text 的内容、Enum 的选项值、Resource 的资源 id。
     */
    std::string text;

    /**
     * Bool 的值。
     */
    bool flag = false;

    /**
     * List 的项。
     */
    std::vector<std::string> items;

    /**
     * 按 key 取时：同 key 的值不一致。
     */
    bool inconsistent = false;
};

/**
 * 控件描述：listControls 与 getProperties 的条目。轻量，不含节点指针。
 */
struct ControlInfo {
    /**
     * 控件节点 id（编辑之后仍然有效）。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 控件的编码 key（一个 key 可以绑多个控件）。
     */
    std::string key;

    /**
     * 控件类型（设计期属性，这里只回显）。
     */
    std::string controlType;

    /**
     * 值形状。
     */
    ValueShape shape = ValueShape::Text;

    /**
     * 只读：编辑被拒。
     */
    bool readonly = false;

    /**
     * 必填：留空时校验报错。
     */
    bool required = false;

    /**
     * 隐藏：内容不在呈现态的流里。
     */
    bool hidden = false;

    /**
     * 结构锁定：整个控件作为一个不可分单位。
     */
    bool structureLock = true;

    /**
     * 外观（设计期属性，这里只回显）。
     */
    Appearance appearance;

    /**
     * 校验规则引用（设计期属性）。
     */
    std::string validateRef;

    /**
     * 当前值。
     */
    Reading value;
};

/**
 * 一次设值请求。按 id 精确（只改那一个控件）或按 key 覆盖同编码的全部。
 */
struct ValueEdit {
    /**
     * 按 id 精确设值时的目标；kInvalid 表示按 key。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 按 key 覆盖同编码的全部。
     */
    std::string key;

    /**
     * 新的值。
     */
    ControlValue value;
};

/**
 * 一次设属性请求。三个属性各是可选值：只改给出来的那几项，没给的不动（三态）。
 * 同一批里同一个 (控件, 属性) 出现多次时按出现顺序后者覆盖前者。
 */
struct PropEdit {
    /**
     * 目标控件；kInvalid 表示按 key。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 按 key 覆盖同编码的全部。
     */
    std::string key;

    /**
     * 改隐藏；没给这一项就不动。
     */
    std::optional<bool> hidden;

    /**
     * 改只读；没给这一项就不动。
     */
    std::optional<bool> readonly;

    /**
     * 改必填；没给这一项就不动。
     */
    std::optional<bool> required;

    /**
     * 改结构锁定；没给这一项就不动。
     */
    std::optional<bool> structureLock;
};

/**
 * 一次批量调用的结果。changed 数的是控件数（属性按项数），用来验证"设 1000 个与设 1 个
 * 是同一量级"这条判据。
 */
struct BatchResult {
    /**
     * 整批成了没有（有一条失败就整批不生效）。
     */
    bool ok = false;

    /**
     * 失败原因。
     */
    std::string error;

    /**
     * 改了几个控件（属性按项数）。
     */
    int changed = 0;

    /**
     * 这一批改了流结构（含 hidden），调用方要重排：呈现态的流位置与段落序号都会变。
     * 只改 readonly / required / structureLock 的批次不重排 —— 它们不改结构。
     */
    bool needs_relayout = false;
};

/**
 * 取一个控件的值。文本类递归拼接整棵子树的可见文字（含嵌套控件内部的文字）。
 * @param tree 文档
 * @param id 控件节点号
 * @return 取值结果（文本类会拼整棵子树的可见文字）
 */
Reading getValue(const NodeTree& tree, NodeId id);

/**
 * 按 key 取。同 key 有多个控件时取文档顺序的第一个，并把不一致标出来 —— 不静默取一个：
 * 不一致是合法状态（一个 key 可以绑多个控件），但用户与宿主都该能发现它。
 * @param tree 文档
 * @param index 控件索引
 * @param key 业务编码
 * @return 取值结果；同 key 不一致时带标记
 */
Reading getValueByKey(const NodeTree& tree, const ControlIndex& index, const std::string& key);

/**
 * 同 key 值不一致的清单，每个 key 一条（按 key 去重）。宿主拿它做一次提示，不弹窗、不阻塞。
 * @param tree 文档
 * @param index 控件索引
 * @return 不一致的 key 清单（每个 key 一条）
 */
std::vector<std::string> inconsistentKeys(const NodeTree& tree, const ControlIndex& index);

/**
 * 列出全部控件（文档顺序）。
 * @param tree 文档
 * @return 全部控件，按文档顺序
 */
std::vector<ControlInfo> listControls(const NodeTree& tree);

/**
 * 读回属性：与 listControls 同一份描述，只是可以按 id 过滤。ids 为空表示"全部"。
 * @param tree 文档
 * @param ids 目标控件；空表示全部
 * @return 每个目标一条描述
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

    /**
     * 接管文档、撤销栈与索引的引用（都不持有所有权）。
     * @param tree 文档
     * @param undo 撤销栈
     * @param index 控件索引
     */
    ControlApi(NodeTree& tree, UndoStack& undo, ControlIndex& index);

    /**
     * 只读的文档视图。裁决门要拿它确认等待期间目标有没有失效（控件被删、被换成别的元素）。
     * @return 文档的只读视图
     */
    const NodeTree& tree() const { return tree_; }

    /**
     * 列出全部控件（文档顺序）。
     * @return 控件描述，不含节点指针
     */
    std::vector<ControlInfo> listControls() const;

    /**
     * 取指定几个控件的值。ids 为空表示"全部"。
     * @param ids 目标控件；空表示全部
     * @return 每个目标一条取值结果
     */
    std::vector<Reading> getValues(const std::vector<NodeId>& ids) const;

    /**
     * 按 key 取值。
     * @param key 控件编码
     * @return 取值结果（同 key 不一致时带标记）
     */
    Reading getValueByKey(const std::string& key) const;

    /**
     * 读回属性（与 listControls 同一份描述，只是可按 id 过滤）。ids 为空表示"全部"。
     * @param ids 目标控件；空表示全部
     * @return 每个目标一条描述
     */
    std::vector<ControlInfo> getProperties(const std::vector<NodeId>& ids) const;

    /**
     * 同 key 值不一致的清单，每个 key 一条。
     * @return 不一致的 key 清单（每个 key 一条，去重）
     */
    std::vector<std::string> inconsistentKeys() const;

    /**
     * 按 id 批量设值（一次调用 = 一个事务）。
     * @param edits 设值请求
     * @return 这一批的结果
     */
    BatchResult setValues(const std::vector<ValueEdit>& edits);

    /**
     * 按 key 批量设值。
     * @param edits 设值请求（忽略 id）
     * @return 这一批的结果
     */
    BatchResult setValuesByKey(const std::vector<ValueEdit>& edits);

    /**
     * 按 id 批量设属性。
     * @param edits 设属性请求
     * @return 这一批的结果
     */
    BatchResult setProperties(const std::vector<PropEdit>& edits);

    /**
     * 按 key 批量设属性。
     * @param edits 设属性请求（忽略 id）
     * @return 这一批的结果
     */
    BatchResult setPropertiesByKey(const std::vector<PropEdit>& edits);

    /**
     * 把随后的几次调用并成一个事务。beginBatch 之后各 setter 只攒步骤，commitBatch 一次提交。
     */
    void beginBatch();

    /**
     * 提交攒下的步骤。没有攒下任何步骤时算成功（幂等）。
     * @return 这一批的结果（没有攒下步骤时 ok = true）
     */
    BatchResult commitBatch();

private:

    /**
     * 设值的共同实现。by_key 为真时只按 key 解析目标（忽略 edit.id），为假时按 id 或 key。
     * @param edits 设值请求
     * @param by_key 只按 key 解析目标
     * @return 这一批的结果
     */
    BatchResult applyValues(const std::vector<ValueEdit>& edits, bool by_key);

    /**
     * 设属性的共同实现。
     * @param edits 设属性请求
     * @param by_key 只按 key 解析目标
     * @return 这一批的结果
     */
    BatchResult applyProps(const std::vector<PropEdit>& edits, bool by_key);

    /**
     * 把 target 的内容整体换成 text（文本类控件专用）。内部不是纯文本时拒绝并给原因。
     * @param target 目标控件
     * @param text 新内容
     * @param steps 攒步骤的容器（追加）
     * @param error 失败原因（调用方给）
     * @return true = 步骤已经攒好
     */
    bool buildTextReplace(NodeId target, const std::string& text, std::vector<Step>* steps,
                          std::string* error);

    /**
     * 提交攒下的步骤：一个事务、一次规范化，并把索引跟上。
     * @return 这一批的结果
     */
    BatchResult flush();

    /**
     * 文档（不持有）。
     */
    NodeTree& tree_;

    /**
     * 撤销栈（不持有）。
     */
    UndoStack& undo_;

    /**
     * 控件索引（不持有，提交之后跟着更新）。
     */
    ControlIndex& index_;

    /**
     * batch 期间攒下的步骤。
     */
    std::vector<Step> pending_;

    /**
     * batch 期间攒下的结果（含第一条失败）。
     */
    BatchResult pending_result_;

    /**
     * 是不是在 batch 里（决定 setter 直接提交还是攒）。
     */
    bool in_batch_ = false;
};

}  // namespace heditor
