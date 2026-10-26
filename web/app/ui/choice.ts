// 下拉框组件：图标（可选）+ 选项。
//
// 名字不进版面：那一格里已经写着值（"18"、"1.5"），再压一行说明文字就会把这一格撑成
//   两行，而旁边那些图标按钮只有一行 —— 一行与两行混着摆，组名就对不齐了。
//   名字改走悬停提示。
//
// 交互与外观走 ui/select：原生 <select> 在这一格上装不了搜索，也统一不到本页那一套。
import { iconHtml } from './icons';
import { createSelect } from './select';
import type { Choice, Component } from './types';

/**
 * 造一个下拉框。
 * @param item 下拉框描述
 * @returns 组件（update 重读当前值）
 */
export function createChoice(item: Choice): Component {
  const el = document.createElement('label');
  el.className = item.wide
    ? 'heditor-ribbon__choice heditor-ribbon__choice--wide'
    : 'heditor-ribbon__choice';
  el.title = item.label;

  if (item.icon) {
    const pic = document.createElement('span');
    pic.className = 'heditor-ribbon__choice-icon';
    pic.innerHTML = iconHtml(item.icon);
    el.appendChild(pic);
  }

  const sel = createSelect({
    options: [],
    value: '',
    act: item.act,
    title: item.label,
    pick: (next) => {
      void item.run(next);
    },
  });
  el.appendChild(sel.el);

  /**
   * 上一批候选的指纹：候选是函数型时每次回显都要重算，没变就不重建。
   */
  let lastKey = '\u0000';

  /**
   * 把候选写进下拉。
   */
  function fill(): void {
    const list = typeof item.options === 'function' ? item.options() : item.options;
    const key = list.map((o) => `${o.value}\u0000${o.text}`).join('\u0001');
    if (key === lastKey) return;
    lastKey = key;
    sel.setOptions(list);
  }
  fill();

  return {
    el,
    /**
     * 重读当前值。
     */
    update(): void {
      // 候选先补齐：字体那一格的候选是"已注册清单"，注册了新字体这一拍才看得见。
      fill();
      // 候选列是固定档位，而文档里可以出现别的值（别处存来的 15 磅、样式层给的字体）。
      //   值不在候选里时下拉原样把它显示出来 —— 显示成空白看起来就像"没有回显"。
      sel.setValue(String(item.value()));
    },
  };
}
