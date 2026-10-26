// 页面设置面板：字段表由库算（describePageSetup），这里只管画与提交。
//
// 宽高两项只对自定义纸张有意义：库把它们标成 disabled，纸张一换由 onChange 跟着开合。
//   单位折算（厘米 ↔ 磅）也在库里 —— 它属于值形态，不属于画法。
import type { Component } from './types';
import type { Editor } from '../../src/index';
import { createPropsPanel } from './props-panel';

/**
 * 面板参数。
 */
export interface PageSetupOptions {
  /**
   * 编辑器。
   */
  editor: Editor;

  /**
   * 应用之后叫一声（面板以外还有缩放读数要重读回显）。
   */
  applied(): void;

  /**
   * 收工：面板该关掉了。
   */
  dismiss(): void;
}

/**
 * 造一个页面设置面板。
 * @param o 参数
 * @returns 组件
 */
export function createPageSetup(o: PageSetupOptions): Component {
  const form = o.editor.describePageSetup();

  const panel = createPropsPanel({
    title: form.title,
    fields: form.fields,
    apply: (values) => {
      void (async () => {
        await o.editor.applyPageSetupForm(values);
        o.applied();
      })();
    },
    onChange: (key, values) => {
      if (key !== 'paper') return;
      const custom = values['paper'] === 'Custom';
      panel.disable('width', !custom);
      panel.disable('height', !custom);
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
