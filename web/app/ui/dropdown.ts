// 下拉菜单按钮：按下去在按钮下面弹出一列动作。
//
// 这一份只做接线：按钮在 button（图标、大按钮排版、按下时不抢焦点）、菜单在 context-menu
//   （键盘上下、回车、Esc、贴边夹取）。两样都不在这里重写，所以加第二个下拉只改动作表。
//
// 为什么不是"按钮 + 特判"：下拉与右键菜单弹的是同一种东西（一列动作），差的是定位方式 ——
//   差别收在 context-menu 的 openAt 里，这里连坐标都不用算。
import { createButton } from './button';
import { createContextMenu, type ContextMenu } from './context-menu';
import type { Component, Dropdown } from './types';

/**
 * 造一个下拉菜单按钮。
 * @param item 描述
 * @returns 组件（update 转发给按钮）
 */
export function createDropdown(item: Dropdown): Component {
  const btn = createButton({
    act: item.act,
    label: item.label,
    icon: item.icon,
    big: item.big,
    run: () => undefined,
  });

  // 右下角那一撇在样式里（--menu）：同一个按钮既能直接跑、也能点开一列时，
  //   没有一点记号的话它看着像"点了没反应"。
  btn.el.classList.add('heditor-ribbon__button--menu');

  // 菜单按需建：只有一个下拉的页面上，它到第一次点开才往 body 上挂浮层。
  let menu: ContextMenu | null = null;

  btn.el.addEventListener('click', () => {
    if (!menu) menu = createContextMenu(() => item.closed?.());
    if (menu.isOpen()) {
      menu.close();
      return;
    }
    menu.openAt(item.entries(), btn.el);
  });

  return {
    el: btn.el,
    /**
     * 重读按下态：菜单开着也算按下了（与开关型按钮同一套观感）。
     */
    update(): void {
      btn.update();
      btn.el.classList.toggle('heditor-ribbon__button--on', (menu?.isOpen() ?? false) || !!item.on?.());
    },
  };
}
