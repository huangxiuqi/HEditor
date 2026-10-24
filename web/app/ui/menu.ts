// 会开面板的按钮：外观与普通按钮一样，按下去在它下面弹出一块面板。
//
// 为什么不是"按钮 + 一堆特判"：面板里装什么由描述层的 panel 给，这一层只管开关与
//   位置 —— 加第二个会开面板的按钮时只需在动作表里写一条，不必再碰 DOM 与事件。
import { createButton } from './button';
import { createPopover } from './popover';
import type { Component, Menu } from './types';

/**
 * 造一个会开面板的按钮。
 * @param item 描述
 * @returns 组件（update 转发给按钮与面板）
 */
export function createMenu(item: Menu): Component {
  const btn = createButton({
    act: item.act,
    label: item.label,
    icon: item.icon,
    big: item.big,
    on: item.on,
    run: () => undefined,
  });

  const pop = createPopover();

  btn.el.addEventListener('click', () => {
    if (pop.isOpen()) {
      pop.close();
      return;
    }
    pop.el.textContent = '';
    // 面板每次打开都新建一份：尺寸选择器的状态（高亮、标题行开关）不该跨开关累积。
    const panel = item.panel(() => pop.close());
    pop.el.appendChild(panel.el);
    pop.open(btn.el);
    panel.update();
  });

  return {
    el: btn.el,
    /**
     * 重读按钮的按下态（面板开着时按钮也点亮，与开关型按钮同一套观感）。
     */
    update(): void {
      btn.update();
      btn.el.classList.toggle('heditor-ribbon__button--on', pop.isOpen() || !!item.on?.());
    },
  };
}
