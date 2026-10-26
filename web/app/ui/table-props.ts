// 表格属性面板：表 / 行 / 格三层记录放在同一张表单里。
//
// 为什么是一张而不是三张：这三层的属性在用户眼里是同一件事（"这张表长什么样"），分开成
//   三个菜单项的话，"把这一行设成标题行"要先进"行属性"，而用户此刻只知道"我在表里"。
//
// 只提交真的变了的那些项：一次"确定"通常只改一组，不比较就一次改三层的话，撤销要按三次
//   才回到原样 —— 而用户按的是一下"确定"。
import type { Component } from './types';
import type { Editor, TableOptions, TableRowOptions, TableState } from '../../src/index';
import { createPropsPanel } from './props-panel';

/**
 * 面板参数。
 */
export interface TablePropsOptions {
  /**
   * 打开面板那一刻的现状（菜单那一拍读的）。
   */
  state: TableState;

  /**
   * 编辑器（应用改动走它）。
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
  const st = o.state;
  // -1 是"没有绝对尺寸"（自动），面板上写 0：用户不该看到 -1 磅这种数。
  const widthNow = st.widthPt > 0 ? st.widthPt : 0;
  const heightNow = st.heightPt > 0 ? st.heightPt : 0;

  const panel = createPropsPanel({
    title: `表格属性（${st.rows} × ${st.cols}）`,
    fields: [
      { kind: 'heading', text: '表格' },
      {
        kind: 'number',
        key: 'tableWidth',
        label: '宽度',
        value: Math.round(widthNow * 100) / 100,
        unit: '磅（0 = 自动）',
        min: 0,
        step: 6,
      },
      {
        kind: 'select',
        key: 'tableAlign',
        label: '水平位置',
        value: st.align,
        options: [
          { value: 'start', text: '左' },
          { value: 'center', text: '居中' },
          { value: 'end', text: '右' },
        ],
      },
      { kind: 'heading', text: `第 ${st.row + 1} 行` },
      { kind: 'toggle', key: 'rowHeader', label: '标题行', value: st.rowHeader },
      { kind: 'toggle', key: 'rowSplit', label: '允许断行', value: st.rowSplit },
      { kind: 'toggle', key: 'rowBreak', label: '行前分页', value: st.rowBreak },
      {
        kind: 'number',
        key: 'rowHeight',
        label: '行高',
        value: Math.round(heightNow * 100) / 100,
        unit: '磅（0 = 按内容）',
        min: 0,
        step: 6,
      },
      {
        kind: 'select',
        key: 'rowRule',
        label: '行高规则',
        value: st.heightRule,
        options: [
          { value: 'auto', text: '按内容' },
          { value: 'atLeast', text: '不小于' },
          { value: 'exact', text: '固定' },
        ],
      },
      { kind: 'heading', text: st.rowSpan > 1 || st.colSpan > 1 ? '单元格（已合并）' : '单元格' },
      {
        kind: 'select',
        key: 'cellVAlign',
        label: '垂直对齐',
        value: st.vAlign,
        options: [
          { value: 'top', text: '顶端' },
          { value: 'middle', text: '居中' },
          { value: 'bottom', text: '底端' },
        ],
      },
    ],
    apply: (v) => {
      const table: TableOptions = {};
      if (String(v['tableAlign'] ?? st.align) !== st.align) {
        table.align = v['tableAlign'] as TableOptions['align'];
      }
      const width = Number(v['tableWidth']);
      if (Number.isFinite(width) && width !== widthNow) table.widthPt = Math.max(0, width);

      const row: TableRowOptions = {};
      if (!!v['rowHeader'] !== st.rowHeader) row.header = !!v['rowHeader'];
      if (!!v['rowSplit'] !== st.rowSplit) row.split = !!v['rowSplit'];
      if (!!v['rowBreak'] !== st.rowBreak) row.breakBefore = !!v['rowBreak'];
      const height = Number(v['rowHeight']);
      if (Number.isFinite(height) && height !== heightNow) row.heightPt = Math.max(0, height);
      if (String(v['rowRule'] ?? st.heightRule) !== st.heightRule) {
        row.heightRule = v['rowRule'] as TableRowOptions['heightRule'];
      }

      const vAlign = String(v['cellVAlign'] ?? st.vAlign);
      const cellChanged = vAlign !== st.vAlign;

      // 只发真的变了的层：一次"确定"通常只动一层（撤销按一次就回到原样），
      //   同时改两层时就是两步 —— 比"永远发三层"那种写法少两步空撤销。
      void (async () => {
        let changed = false;
        if (Object.keys(table).length > 0) changed = (await o.editor.setTable(table)) || changed;
        if (Object.keys(row).length > 0) changed = (await o.editor.setTableRow(row)) || changed;
        if (cellChanged) {
          changed =
            (await o.editor.setTableCell({ vAlign: vAlign as 'top' | 'middle' | 'bottom' })) ||
            changed;
        }
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
