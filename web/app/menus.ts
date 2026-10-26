// 右键菜单的动作表：什么位置上该有哪些动作。
//
// 这一份是"描述层"，与工具条那份（main.ts 的 makeTabs）同一个分工：它只说"有什么动作"，
//   弹层、定位、键盘与子菜单的走向都在 ui 组件里。加一项动作因此只改这里，不碰 DOM。
//
// 三份菜单（表格 / 控件 / 图片）共用一个上下文菜单组件与一块属性面板浮层：它们只差数据，
//   形状（分组、勾选、禁用、子菜单）完全一样。
import type { ControlSummary, ContextTarget, Editor, TableState } from '../src/index';
import { createContextMenu, type MenuEntry } from './ui/context-menu';
import { createControlProps } from './ui/control-props';
import { createGridPicker } from './ui/grid-picker';
import { createLayer } from './ui/layer';
import { createPropsPanel } from './ui/props-panel';
import { createTableProps } from './ui/table-props';
import type { Component } from './ui/types';

/**
 * 菜单要用到的宿主能力。
 */
export interface MenusHost {
  /**
   * 编辑器。
   */
  editor: Editor;

  /**
   * 往状态栏写一句话。
   * @param text 要说的话
   */
  say(text: string): void;

  /**
   * 把焦点还回编辑器。
   */
  focusEditor(): void;

  /**
   * 插一个控件（类型与选项由菜单给）。
   * @param type 业务类型
   * @param opts 其余选项
   */
  insertControl(type: string, opts: Record<string, unknown>): void;

  /**
   * 弹文件选择器插图片。
   */
  pickImage(): void;
}

/**
 * 右键菜单。
 */
export interface Menus {
  /**
   * 在落点处开菜单。
   * @param target 落点归属（库给的）
   * @param at 客户端坐标
   */
  open(target: ContextTarget, at: { x: number; y: number }): Promise<void>;

  /**
   * 收掉（页面销毁时用）。
   */
  destroy(): void;
}

/**
 * 演示用的选项表：值与标签分开，值进数据、标签进纸面（与工具条那份一致）。
 */
const kDemoOptions = [
  { value: 'a', label: '选项甲' },
  { value: 'b', label: '选项乙' },
  { value: 'c', label: '选项丙' },
];

/**
 * 造一份右键菜单。
 * @param host 宿主能力
 * @returns 菜单
 */
export function createMenus(host: MenusHost): Menus {
  const editor = host.editor;
  const menu = createContextMenu(() => host.focusEditor());
  // 属性面板与工具条那些面板共用同一块浮层：定位与关闭时机只有一份实现。
  const panel = createLayer('heditor-popover', () => host.focusEditor());

  /**
   * 在指针处开一块面板。
   * @param content 面板内容
   * @param at 客户端坐标
   */
  const openPanel = (content: Component, at: { x: number; y: number }): void => {
    panel.el.textContent = '';
    panel.el.appendChild(content.el);
    panel.openAtPoint(at.x, at.y);
    content.update();
    // 键盘直接落在第一格上：属性面板是"打开就要改"的东西。
    const first = content.el.querySelector('input, select, textarea');
    if (first instanceof HTMLElement) first.focus({ preventScroll: true });
  };

  /**
   * 跑一条表格命令，并把结果说给状态行。
   * @param op 动词
   * @param ok 成功时的说法
   */
  const runTableOp = (op: Parameters<Editor['tableOp']>[0], ok: string): void => {
    void editor.tableOp(op).then((done) => {
      host.say(done ? ok : '这个位置改不了表格（光标不在表里，或当前模式不允许）');
    });
  };

  /**
   * 表格那一组菜单。
   * @param st 现状
   * @param at 客户端坐标（属性面板开在这里）
   * @returns 菜单项
   */
  const tableEntries = (st: TableState, at: { x: number; y: number }): MenuEntry[] => {
    const entries: MenuEntry[] = [
      { kind: 'heading', label: `表格 ${st.rows} × ${st.cols}` },
      { kind: 'action', label: '在上方插入行', run: () => runTableOp('insertRowAbove', '已插入一行') },
      { kind: 'action', label: '在下方插入行', run: () => runTableOp('insertRowBelow', '已插入一行') },
      {
        kind: 'action',
        label: '删除本行',
        disabled: st.rows <= 1,
        run: () => runTableOp('removeRow', '已删除一行'),
      },
      { kind: 'separator' },
      { kind: 'action', label: '在左侧插入列', run: () => runTableOp('insertColumnLeft', '已插入一列') },
      { kind: 'action', label: '在右侧插入列', run: () => runTableOp('insertColumnRight', '已插入一列') },
      {
        kind: 'action',
        label: '删除本列',
        disabled: st.cols <= 1,
        run: () => runTableOp('removeColumn', '已删除一列'),
      },
      { kind: 'separator' },
      {
        kind: 'action',
        label: '合并单元格',
        hint: st.selCells > 1 ? `${st.selRows} × ${st.selCols}` : '先拖选几格',
        disabled: st.selCells < 2,
        run: () => runTableOp('mergeCells', '已合并'),
      },
      {
        kind: 'action',
        label: '拆分单元格',
        disabled: st.rowSpan === 1 && st.colSpan === 1,
        run: () => runTableOp('splitCell', '已拆分'),
      },
      { kind: 'separator' },
      {
        kind: 'action',
        label: '表格属性…',
        run: () =>
          openPanel(
            createTableProps({ state: st, editor, dismiss: () => panel.close(), say: host.say }),
            at,
          ),
      },
    ];
    return entries;
  };

  /**
   * 控件那一组菜单。
   * @param c 控件
   * @param at 客户端坐标
   * @returns 菜单项
   */
  const controlEntries = (c: ControlSummary, at: { x: number; y: number }): MenuEntry[] => {
    const entries: MenuEntry[] = [{ kind: 'heading', label: `控件 ${c.type || 'text'}` }];

    const type = c.type || 'text';
    if (type === 'radioButton' || type === 'checkBox') {
      entries.push({
        kind: 'action',
        label: c.value.flag ? '取消勾选' : '勾选',
        checked: c.value.flag,
        run: () => {
          void editor.toggleControl(c.index, -1);
        },
      });
    } else if (type === 'radioGroup' || type === 'checkBoxGroup') {
      const selected = new Set(type === 'radioGroup' ? [c.value.text] : c.value.items);
      entries.push({
        kind: 'submenu',
        label: '取值',
        items: c.options.map((op, i) => ({
          kind: 'action' as const,
          label: op.label || op.value,
          checked: selected.has(op.value),
          run: () => {
            void editor.toggleControl(c.index, i);
          },
        })),
      });
    } else if (type === 'dropdownList') {
      entries.push({
        kind: 'submenu',
        label: '取值',
        items: c.options.map((op) => ({
          kind: 'action' as const,
          label: op.label || op.value,
          checked: c.value.text === op.value,
          run: () => {
            void editor.setControlValue(c.index, { text: op.value });
          },
        })),
      });
    }

    entries.push({
      kind: 'action',
      label: '属性…',
      run: () =>
        openPanel(
          createControlProps({ control: c, editor, dismiss: () => panel.close(), say: host.say }),
          at,
        ),
    });
    entries.push({ kind: 'separator' });
    entries.push({
      kind: 'action',
      label: '删除控件',
      run: () => {
        void editor.removeControl(c.index).then((done) => host.say(done ? '已删除控件' : '删除被拒'));
      },
    });
    entries.push({
      kind: 'action',
      label: '删除控件、保留内容',
      run: () => {
        void editor
          .removeControl(c.index, { keepContent: true })
          .then((done) => host.say(done ? '已拆掉控件，内容留下' : '删除被拒'));
      },
    });
    return entries;
  };

  /**
   * 图片那一组菜单。
   * @param index 图片下标
   * @param size 这张图现在的显示尺寸（磅）
   * @param at 客户端坐标
   * @returns 菜单项
   */
  const imageEntries = (
    index: number,
    size: { w: number; h: number },
    at: { x: number; y: number },
  ): MenuEntry[] => {
    return [
      { kind: 'heading', label: '图片' },
      {
        kind: 'action',
        label: '大小…',
        run: () =>
          openPanel(
            createPropsPanel({
              title: '图片大小',
              fields: [
                {
                  kind: 'number',
                  key: 'width',
                  label: '宽度',
                  value: Math.round(size.w * 100) / 100,
                  unit: '磅',
                  min: 1,
                  step: 6,
                },
                {
                  kind: 'number',
                  key: 'height',
                  label: '高度',
                  value: Math.round(size.h * 100) / 100,
                  unit: '磅',
                  min: 1,
                  step: 6,
                },
              ],
              apply: (v) => {
                // 高宽按当前值等比/独立设定：两个都给才照给，缺一个就按宽高比推。
                const w = Number(v['width']);
                const h = Number(v['height']);
                if (!(w > 0) && !(h > 0)) return;
                void editor
                  .resizeImage(index, w > 0 ? w : 1, h > 0 ? h : 1)
                  .then((done) => host.say(done ? '图片大小已改' : '改尺寸被拒'));
              },
              dismiss: () => panel.close(),
            }),
            at,
          ),
      },
    ];
  };

  /**
   * 任何位置都有的那一组：撤销、剪贴板、插入。
   * @param at 客户端坐标
   * @returns 菜单项
   */
  const commonEntries = (at: { x: number; y: number }): MenuEntry[] => {
    const insert = (type: string, opts: Record<string, unknown>): void => {
      host.insertControl(type, opts);
    };
    return [
      { kind: 'separator' },
      {
        kind: 'action',
        label: '撤销',
        hint: 'Ctrl+Z',
        run: () => void editor.undo(),
      },
      {
        kind: 'action',
        label: '重做',
        hint: 'Ctrl+Y',
        run: () => void editor.redo(),
      },
      { kind: 'separator' },
      { kind: 'action', label: '剪切', hint: 'Ctrl+X', run: () => void editor.cut() },
      { kind: 'action', label: '复制', hint: 'Ctrl+C', run: () => void editor.copy() },
      { kind: 'action', label: '粘贴', hint: 'Ctrl+V', run: () => void editor.paste() },
      { kind: 'action', label: '全选', hint: 'Ctrl+A', run: () => void editor.selectAll() },
      {
        kind: 'submenu',
        label: '插入',
        items: [
          {
            kind: 'action',
            label: '表格…',
            run: () =>
              openPanel(
                createGridPicker({
                  rows: 3,
                  cols: 3,
                  maxRows: 8,
                  maxCols: 8,
                  header: true,
                  onPick: (rows, cols, header) => {
                    void editor.insertTable({ rows, cols, header });
                  },
                  dismiss: () => panel.close(),
                }),
                at,
              ),
          },
          { kind: 'separator' },
          {
            kind: 'action',
            label: '文本输入',
            run: () => insert('text', { label: '（待填）' }),
          },
          { kind: 'action', label: '数值输入', run: () => insert('number', { label: '0' }) },
          { kind: 'action', label: '单选', run: () => insert('radioButton', { label: '选项甲' }) },
          { kind: 'action', label: '复选', run: () => insert('checkBox', { label: '同意' }) },
          {
            kind: 'action',
            label: '单选组',
            run: () => insert('radioGroup', { options: kDemoOptions }),
          },
          {
            kind: 'action',
            label: '复选组',
            run: () => insert('checkBoxGroup', { options: kDemoOptions }),
          },
          { kind: 'action', label: '下拉', run: () => insert('dropdownList', { options: kDemoOptions }) },
          { kind: 'action', label: '时间选择', run: () => insert('date', {}) },
          { kind: 'separator' },
          { kind: 'action', label: '图片…', run: () => host.pickImage() },
        ],
      },
    ];
  };

  return {
    async open(target, at): Promise<void> {
      const entries: MenuEntry[] = [];
      if (target.kind === 'table') {
        // 现状要现读：菜单里的禁用态（删最后一行、没选两格不能合并）全按它算。
        const st = await editor.tableState();
        if (st.inTable) entries.push(...tableEntries(st, at));
      } else if (target.kind === 'control') {
        const c = editor.form()[target.control];
        if (c) entries.push(...controlEntries(c, at));
      } else if (target.kind === 'image') {
        // 现在的尺寸要现读：面板照着它预填，用户看到的才是"改之前是多大"。
        const boxes = await editor.images();
        const box = boxes[target.image];
        entries.push(...imageEntries(target.image, { w: box?.w ?? 0, h: box?.h ?? 0 }, at));
      }
      entries.push(...commonEntries(at));
      menu.open(entries, at);
    },

    destroy(): void {
      menu.close();
      panel.close();
    },
  };
}
