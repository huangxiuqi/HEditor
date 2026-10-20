/**
 * 步骤 → 修订：把命令层算好的一批步骤落到不可变树上。
 *
 * 它是模型层 applyStep + normalizeDocument 的替代物，两条语义必须逐条相同 —— 引擎换数据源
 * 之后，"同一串编辑得到同一份文档"这条判据全靠这里。差别只有一处：模型层就地改树、撤销靠
 * 逆步骤，这一层每步产出一份新修订、撤销靠拿回旧值，所以步骤里记的 detached 身份不再需要
 * （重做回到的是同一份修订，身份自然不丢）。
 *
 * 任一步失败时整批不生效：返回的是失败，不是"应用了一半的文档"。
 */
#pragma once

#include <vector>

#include "core/doc/steps.h"
#include "core/rev/rev.h"

namespace heditor::rev {

/**
 * 把一批步骤按顺序落到 doc 上，最后按模型层同一套规则规范化一次。
 *
 * 步骤的坐标是它自己应用那一刻的坐标：前一步改了长度，后一步的绝对流位置就过期了。
 * 批次由命令层负责映射（见 pushMappedStep），这里只按顺序照做。
 */
EditResult applySteps(const Doc& doc, const std::vector<Step>& steps);

/**
 * 规范化：删掉空 run、合并相邻同字符属性的 run。与模型层的 normalizeDocument 同一条规则，
 * 自底向上。它不改段落数与流位置，只改节点个数。
 */
Doc normalize(const Doc& doc);

}  // namespace heditor::rev
