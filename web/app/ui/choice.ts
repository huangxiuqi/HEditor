// 下拉框组件：图标（可选）+ 选项。
//
// 名字不进版面：那一格里已经写着值（"18"、"1.5"），再压一行说明文字就会把这一格撑成
//   两行，而旁边那些图标按钮只有一行 —— 一行与两行混着摆，组名就对不齐了。
//   名字改走悬停提示。
import { iconHtml } from './icons';
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

  const sel = document.createElement('select');
  sel.dataset.act = item.act;
  sel.title = item.label;
  for (const o of item.options) {
    const opt = document.createElement('option');
    opt.value = o.value;
    opt.textContent = o.text;
    sel.appendChild(opt);
  }
  el.appendChild(sel);

  sel.addEventListener('change', () => {
    void item.run(sel.value);
  });

  return {
    el,
    /**
     * 重读当前值。
     */
    update(): void {
      sel.value = item.value();
    },
  };
}
