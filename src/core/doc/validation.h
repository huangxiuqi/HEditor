/**
 * 校验状态与异步裁决门（见 docs/文档模型设计.md 第七节"与业务交互"）。
 *
 * 钩子允许异步，但语义是"持有候选变更，裁决通过才应用"，不是"先应用、结果回来再回滚"。
 * 后者就是"先勾上再弹回去"的那一帧，必须避免。所以等待期间候选放在文档之外：文档不变、
 * 撤销栈不变、不排版也不上屏，控件只进 pending 展示、不显示新值。
 *
 * 这一层只管状态机与状态表，不管钩子本身：钩子是宿主的函数，名字由控件记录的 validateRef
 * 给出（绑定是数据、代码是程序）。宿主发起等待、拿到结果后调 allow 或 deny，超时由宿主计时
 * 后调 expire —— 模型层没有时钟，也不假装有。
 *
 * 只拦用户交互，不拦编程设值：setValues 是 program 源，不进这一层。这同时保证批量路径完全
 * 同步，批次里不会出现"一半在等异步、一半已提交"的混合状态。
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "core/doc/control_api.h"
#include "core/doc/node.h"

namespace heditor {

/**
 * 一个控件的校验状态。它是运行时状态，不落盘 —— 存进文档会让红框跟着打印出来。
 */
enum class ValidationState : uint8_t {
    None = 0,     // 没校验过
    Pending = 1,  // 异步等待期间
    Invalid = 2,  // 未通过（由 Deny 置位，或宿主主动置位）
};

const char* validationStateName(ValidationState s);

struct Validation {
    ValidationState state = ValidationState::None;
    std::string message;
};

/**
 * 裁决的结果。四态而不是布尔：Dropped 是"该等待已作废"（被后一次交互取消、或目标已失效），
 * 它既不是通过也不是拒绝，更不该被当成失败去提示用户。
 */
enum class GateOutcome : uint8_t {
    Applied = 0,  // 通过，候选变更已按普通事务应用
    Denied = 1,   // 拒绝（含超时），值保持原样
    Dropped = 2,  // 这次等待已作废，什么都没发生
    Failed = 3,   // 裁决通过但应用失败，原因在 message 上
};

struct GateResult {
    GateOutcome outcome = GateOutcome::Dropped;
    std::string message;
};

/**
 * 发起等待的结果。token 为 0 表示没发起（目标不存在或不是控件）。
 */
struct BeginResult {
    bool ok = false;
    int64_t token = 0;
    std::string error;
};

/**
 * 异步裁决门。持有候选变更与每控件的校验状态。
 *
 * 一个控件同时最多一个等待：同一控件再次交互时前一次按取消丢弃（最后交互为准），
 * 旧结果到达时返回 Dropped，不产生任何应用也不误报失败。
 */
class ChangeGate {
public:
    explicit ChangeGate(ControlApi& api);

    /**
     * 登记一个候选变更：控件进 pending，文档与撤销栈一概不动。按 id 定位目标 —— 用户交互
     * 针对的就是眼前这一个控件，按 key 展开成多个之后就没法回答"结果是给谁的"。
     */
    BeginResult begin(const ValueEdit& edit);

    /**
     * 取消某个控件的等待（最后交互为准）。返回是否真有等待被取消。
     */
    bool cancel(NodeId target);

    /**
     * 裁决通过：确认目标仍然是同一个控件之后，把候选变更按普通事务应用。
     */
    GateResult allow(int64_t token);

    /**
     * 裁决拒绝：丢弃候选，并把控件置为 invalid 带上原因。
     */
    GateResult deny(int64_t token, const std::string& reason);

    /**
     * 超时按拒绝处理，不静默放行。放行一颗从未校验过的复选框比不放行更危险。
     */
    GateResult expire(int64_t token);

    /**
     * 主动置位（宿主拿到晚到的校验结果时用）。它不需要对应的事务 —— 异步校验的结果有时
     * 晚于一次编辑，宿主拿到结果时已经没有事件了。
     */
    void setValidation(NodeId id, ValidationState state, const std::string& message);

    Validation stateOf(NodeId id) const;

    bool isPending(NodeId id) const;
    int pendingCount() const { return static_cast<int>(pending_.size()); }

    /**
     * 清掉已经不在文档里的目标：等待的锚点失效就丢弃，校验状态一并删除。
     */
    void sweepStale();

private:
    struct Candidate {
        int64_t token = 0;
        ValueEdit edit;
    };

    /**
     * 把等待摘下来（从两张表里删掉），找不到时返回 false。
     */
    bool take(int64_t token, Candidate* out);

    ControlApi& api_;
    int64_t next_token_ = 1;
    std::unordered_map<int64_t, Candidate> pending_;
    std::unordered_map<NodeId, int64_t> by_target_;
    std::unordered_map<NodeId, Validation> states_;
};

}  // namespace heditor
