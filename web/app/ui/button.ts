// 按钮组件：一个动作 + 一个回显。
//
// 大按钮把名字写在图标下面（Word 那一套），小按钮只留图标、名字进 title —— 这不是
//   装饰上的取舍：小按钮一排能放七八个，大按钮一排只能放三四个，而"常按的"就那么几个。
import { iconHtml } from './icons';
import type { Button, Component } from './types';

/**
 * 造一个按钮。
 * @param item 动作描述
 * @returns 组件（update 重读按下态）
 */
export function createButton(item: Button): Component {
  const el = document.createElement('button');
  el.className = item.big
    ? 'heditor-ribbon__button heditor-ribbon__button--big'
    : 'heditor-ribbon__button';
  el.dataset.act = item.act;
  el.title = item.label;

  if (item.icon) el.innerHTML = iconHtml(item.icon);
  if (item.big) {
    const text = document.createElement('span');
    text.textContent = item.label;
    el.appendChild(text);
  }

  el.addEventListener('click', () => {
    void item.run();
  });

  return {
    el,
    /**
     * 重读按下态（item.on 没给就什么都不做：它不是开关）。
     */
    update(): void {
      if (!item.on) return;
      el.classList.toggle('heditor-ribbon__button--on', item.on());
    },
  };
}
