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
  el.appendChild(sel);

  /**
   * 把候选写进 select。函数型候选每次都重算，变了才重建 —— 重建会丢掉当前选中项，
   * 所以重建之后要把选中项写回去。
   */
  function fill(): void {
    const list = typeof item.options === 'function' ? item.options() : item.options;
    const same = sel.options.length === list.length &&
      list.every((o, i) => sel.options[i].value === o.value && sel.options[i].text === o.text);
    if (same) return;
    const keep = sel.value;
    sel.textContent = '';
    for (const o of list) {
      const opt = document.createElement('option');
      opt.value = o.value;
      opt.textContent = o.text;
      sel.appendChild(opt);
    }
    if (keep) sel.value = keep;
  }
  fill();

  sel.addEventListener('change', () => {
    void item.run(sel.value);
  });

  return {
    el,
    /**
     * 重读当前值。
     */
    update(): void {
      // 临时项先摘掉：它混在候选里，函数型候选的"变没变"就永远判成变了。
      const stale = sel.querySelector('option[data-temp="1"]');
      if (stale) stale.remove();
      // 候选先补齐：字体那一格的候选是"已注册清单"，注册了新字体这一拍才看得见。
      fill();
      const want = String(item.value());
      // 候选列是固定档位，而文档里可以出现别的值（别处存来的 15 磅、样式层给的字体）。
      //   赋一个不存在的值等于把 select 清空，显示成空白 —— 看起来就像"没有回显"。
      //   所以补一个临时项，下次重读时先把它摘掉，免得越攒越多。
      if (want !== '' && !Array.from(sel.options).some((o) => o.value === want)) {
        const extra = document.createElement('option');
        extra.value = want;
        extra.textContent = want;
        extra.dataset.temp = '1';
        sel.appendChild(extra);
      }
      sel.value = want;
    },
  };
}
