/**
 * 步骤的位置映射：只吃步骤与位置，不看文档树。
 */
#pragma once

#include "core/rev/step_types.h"

namespace heditor {

/**
 * 一步之后某个流位置去了哪里。
 * @param step 那一步
 * @param pos 编辑前的位置
 * @param assoc 边界贴哪一边
 * @returns 编辑后的位置
 */
FlowPos mapPos(const Step& step, FlowPos pos, Assoc assoc);

}  // namespace heditor
