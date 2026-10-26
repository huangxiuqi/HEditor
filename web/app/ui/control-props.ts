// 控件属性面板：字段表由库算（describeControl），这里只管画与提交。
//
// 之前这里抄着"哪种类型有哪些项、每项落到哪个写入口"那份知识；现在它在库里，宿主换一份
//   界面（原生控件、组件库、框架）都能拿到同一张字段表与同一条提交路径。
import type { Component } from './types';
import type { ControlSummary, Editor } from '../../src/index';
import { createPropsPanel } from './props-panel';

/**
 * 面板参数。
 */
export interface ControlPropsOptions {
  /**
   * 打开面板那一刻的控件（form() 的那一项）。
   */
  control: ControlSummary;

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
 * 造一块控件属性面板。
 * @param o 参数
 * @returns 组件
 */
export function createControlProps(o: ControlPropsOptions): Component {
  const form = o.editor.describeControl(o.control);

  const panel = createPropsPanel({
    title: form.title,
    fields: form.fields,
    apply: (v) => {
      void (async () => {
        const changed = await o.editor.applyControlForm(o.control.index, v);
        o.say(changed ? '控件属性已更新' : '控件属性没有变化');
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
