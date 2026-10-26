// 表格属性面板：字段表由库算（describeTable），这里只管画与提交。
//
// 表 / 行 / 格三层写在一张表单里，是因为它们在用户眼里是同一件事；三层各走各的写入口、
//   一层里改几项也只提交一次，这些都在 applyTableForm 里 —— 宿主不必自己分。
import type { Component } from './types';
import type { Editor, TableState } from '../../src/index';
import { createPropsPanel } from './props-panel';

/**
 * 面板参数。
 */
export interface TablePropsOptions {
  /**
   * 打开面板那一刻的现状（判断能不能开面板用的那一拍）。
   */
  state: TableState;

  /**
   * 编辑器。
   */
  editor: Editor;

  /**
   * 关掉面板。
   */
  dismiss(): void;

  /**
   * 往状态栏写一句话。
   * @param text 要说的话
   */
  say(text: string): void;
}

/**
 * 造一块表格属性面板。
 * @param o 参数
 * @returns 组件
 */
export function createTableProps(o: TablePropsOptions): Component {
  const form = o.editor.describeTable(o.state);

  const panel = createPropsPanel({
    title: form.title,
    fields: form.fields,
    apply: (v) => {
      void (async () => {
        const changed = await o.editor.applyTableForm(v);
        o.say(changed ? '表格属性已更新' : '表格属性没有变化');
      })();
    },
    dismiss: o.dismiss,
  });

  return {
    el: panel.el,
    /**
     * 面板每次打开都是新的一份，这里只为与其它组件同形。
     */
    update(): void {
      panel.update();
    },
  };
}
