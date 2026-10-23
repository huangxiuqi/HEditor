// 组组件：一排动作 + 组名。
//
// 组只做两件事：把子组件摆成一行、把 update 转发下去。它不解释自己的内容 ——
//   所以"加一个组"只需要给一份描述，不需要在这里加分支。
import { createButton } from './button';
import { createChoice } from './choice';
import { createStack } from './stack';
import { isChoice, isStack, type Component, type Group, type Item } from './types';

/**
 * 造一项（按钮 / 下拉框 / 竖排按钮列）。
 * @param item 描述
 * @returns 组件
 */
function makeItem(item: Item): Component {
  if (isStack(item)) return createStack(item);
  if (isChoice(item)) return createChoice(item);
  return createButton(item);
}

/**
 * 造一组动作。
 * @param group 组描述
 * @returns 组件（update 转发给子组件）
 */
export function createGroup(group: Group): Component {
  const el = document.createElement('div');
  el.className = 'heditor-ribbon__group';

  const row = document.createElement('div');
  row.className = 'heditor-ribbon__items';
  const kids = group.items.map(makeItem);
  for (const kid of kids) row.appendChild(kid.el);

  const cap = document.createElement('div');
  cap.className = 'heditor-ribbon__caption';
  cap.textContent = group.label;

  el.append(row, cap);

  return {
    el,
    /**
     * 重读这一组每一项的回显。
     */
    update(): void {
      for (const kid of kids) kid.update();
    },
  };
}
