// 竖排按钮列：与大按钮并排时，两个 27 高的小按钮叠起来正好与 54 高的那个同高。
//
// 单独拆一件而不是在组里写死"第二个开始竖排"：竖排是布局意图，不是位置巧合 ——
//   按位置猜的话，往组里插一个按钮就会把整组的意思改掉。
import { createButton } from './button';
import type { Component, Stack } from './types';

/**
 * 造一列按钮。
 * @param stack 这一列的描述
 * @returns 组件（update 转发给每个按钮）
 */
export function createStack(stack: Stack): Component {
  const el = document.createElement('div');
  el.className = 'heditor-ribbon__stack';

  const kids = stack.items.map((item) => createButton(item));
  for (const kid of kids) el.appendChild(kid.el);

  return {
    el,
    /**
     * 重读这一列的回显。
     */
    update(): void {
      for (const kid of kids) kid.update();
    },
  };
}
