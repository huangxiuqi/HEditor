/**
 * 不变量检查器：把第二节的条目逐条变成可调用的断言。
 *
 * 第二节说"任何编辑操作之后都要重新满足"，所以它必须能被生产代码调用（不只是单测），
 * 返回一张违规清单而不是断言失败 —— 宿主拿到清单才能在人面前说清该次编辑破坏了什么。
 *
 * 规则分两类，这个区分决定了它能抓到什么。一类约束树的字段（种类、父子链、文本长度、
 * ID），这类能被真实的编辑破坏，也能在单测里手工构造反例，是检查器真正在防的东西。
 * 另一类约束从树算出来的量（流位置区间、段落范围），数据层构造不出反例，因为那些量
 * 本来就由同一份孩子顺序算出来；它们的价值是守住算出它们的代码 ——
 * flow.cpp 或 para.cpp 改错时，这类规则会先亮。两类都要跑，但读结果的人要知道差别。
 *
 * 现在拿不到输入的条目进 pending（每条写明等哪一步带来输入），并且 checked 会报出
 * 实际跑过的规则 —— 调用方因此看得见"哪些查了、哪些还没"，
 * 检查器不可能靠"静默少查几条"变成永远全绿。
 */
#pragma once

#include <string>
#include <vector>

#include "core/doc/node.h"

namespace heditor {

/**
 * 一条违规。rule 是规则名（用于按行定位与测试断言），node 与 at 给出位置，
 * detail 写清"实际是什么、期望是什么"。
 */
struct Violation {
    std::string rule;
    NodeId node = kInvalidNodeId;
    int at = -1;  // 相关槽位；-1 表示与槽位无关
    std::string detail;
};

/**
 * 一条暂时无法判定的条目：它需要什么输入才能查，以及那个输入由哪一步带来。
 */
struct PendingRule {
    std::string rule;
    std::string waiting_for;
};

/**
 * 检查结果。ok() 只看违规清单；pending 为空才是"第二节全部条目都已落地"。
 */
struct InvariantReport {
    std::vector<Violation> violations;
    std::vector<std::string> checked;
    std::vector<PendingRule> pending;

    bool ok() const { return violations.empty(); }
};

/**
 * 跑一遍全部可判定的不变量。空报告 = 全绿。
 *
 * 不需要可见性口径参数：检查器自己同时看编辑态与呈现态两个流口径，
 * 要验的关系是"呈现态被摘除的节点，正好是自身或祖先声明了隐藏的那些"。
 */
InvariantReport checkInvariants(const NodeTree& tree);

}  // namespace heditor
