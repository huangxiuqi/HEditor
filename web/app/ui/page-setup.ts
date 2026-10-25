// 页面设置面板：纸张、方向、四边页边距。
//
// 为什么是面板而不是四个下拉框：四边页边距是一组要"一起看"的数（上下、左右成对），
//   摊进工具条会占掉半条，而它们只在排版时改一次。面板与表格尺寸选择器同一个入口形状。
//
// 单位：界面上写厘米（人按厘米想页边距），传给编辑器的是磅 —— 折算只在这一层做。
import type { Component } from './types';
import type { PageSetup } from '../../src/index';
/**
 * 1 厘米 = 28.3465 磅（72 / 2.54）。
 */
const CM_PT = 72 / 2.54;

/**
 * 纸张候选。与库里的 PAPER_PT 同键 —— 名字写错时库会当场抛 TypeError（不是静默回落 A4）。
 */
const PAPERS: Array<{ value: PageSetup['paper']; text: string }> = [
  { value: 'A4', text: 'A4（21 × 29.7 厘米）' },
  { value: 'A3', text: 'A3（29.7 × 42 厘米）' },
  { value: 'A5', text: 'A5（14.8 × 21 厘米）' },
  { value: 'B5', text: 'B5（17.6 × 25 厘米）' },
  { value: 'Letter', text: 'Letter（21.6 × 27.9 厘米）' },
  { value: 'Legal', text: 'Legal（21.6 × 35.6 厘米）' },
  { value: 'Custom', text: '自定义' },
];

/**
 * 面板参数。
 */
export interface PageSetupOptions {
  /**
   * 当前生效的页面设置（打开面板时读一次）。
   */
  current(): PageSetup;

  /**
   * 当前页面的实际宽高（磅，纵向口径）。选"自定义"时用它预填：固定规格那几档的尺寸
   * 在面板里不用再算一遍，而页面此刻多大只有宿主知道。
   */
  pageSizePt(): { w: number; h: number };

  /**
   * 应用一份新的页面设置（已经是归一之后的形状）。
   * @param next 新的页面设置
   */
  apply(next: PageSetup): void;

  /**
   * 收工：面板该关掉了。
   */
  dismiss(): void;
}

/**
 * 一行"标签 + 数字输入"。
 * @param label 标签文字
 * @param act data-act（判据靠它定位输入框）
 * @param value 初值（厘米）
 * @returns 行元素与它的输入框
 */
function field(label: string, act: string, value: number): { row: HTMLElement; input: HTMLInputElement } {
  const row = document.createElement('label');
  row.className = 'heditor-page-setup__row';

  const text = document.createElement('span');
  text.className = 'heditor-page-setup__label';
  text.textContent = label;

  const input = document.createElement('input');
  input.className = 'heditor-page-setup__input';
  input.type = 'number';
  input.min = '0';
  input.step = '0.1';
  input.value = value.toFixed(2);
  input.dataset.act = act;

  const unit = document.createElement('span');
  unit.className = 'heditor-page-setup__unit';
  unit.textContent = '厘米';

  row.append(text, input, unit);
  return { row, input };
}

/**
 * 造一个页面设置面板。
 * @param o 参数
 * @returns 组件（update 只重读一遍输入框的值）
 */
export function createPageSetup(o: PageSetupOptions): Component {
  const el = document.createElement('div');
  el.className = 'heditor-page-setup';

  const now = o.current();

  const paper = document.createElement('select');
  paper.className = 'heditor-page-setup__select';
  paper.dataset.act = 'page-paper';
  for (const p of PAPERS) {
    const opt = document.createElement('option');
    opt.value = p.value;
    opt.textContent = p.text;
    paper.appendChild(opt);
  }
  paper.value = now.paper;

  const orient = document.createElement('select');
  orient.className = 'heditor-page-setup__select';
  orient.dataset.act = 'page-orient';
  for (const v of [
    { value: 'portrait', text: '纵向' },
    { value: 'landscape', text: '横向' },
  ]) {
    const opt = document.createElement('option');
    opt.value = v.value;
    opt.textContent = v.text;
    orient.appendChild(opt);
  }
  orient.value = now.landscape ? 'landscape' : 'portrait';

  const top = field('上', 'page-top', now.margins.top / CM_PT);
  const bottom = field('下', 'page-bottom', now.margins.bottom / CM_PT);
  const left = field('左', 'page-left', now.margins.left / CM_PT);
  const right = field('右', 'page-right', now.margins.right / CM_PT);

  const head = document.createElement('div');
  head.className = 'heditor-page-setup__head';
  head.textContent = '页面设置';

  const grid = document.createElement('div');
  grid.className = 'heditor-page-setup__grid';
  grid.append(top.row, bottom.row, left.row, right.row);

  const apply = document.createElement('button');
  apply.type = 'button';
  apply.className = 'heditor-page-setup__apply';
  apply.dataset.act = 'page-apply';
  apply.textContent = '应用';
  // 按下就应用：等 click 会被外面的 pointerdown（关闭面板）抢在前面。
  apply.addEventListener('pointerdown', (ev) => {
    ev.preventDefault();
    const cm = (input: HTMLInputElement): number => Number(input.value) * CM_PT;
    const custom =
      paper.value === 'Custom'
        ? { w: cm(customW.input), h: cm(customH.input) }
        : undefined;
    o.apply({
      paper: paper.value as PageSetup['paper'],
      landscape: orient.value === 'landscape',
      ...(custom ? { custom } : {}),
      margins: {
        top: cm(top.input),
        right: cm(right.input),
        bottom: cm(bottom.input),
        left: cm(left.input),
      },
    });
    o.dismiss();
  });

  // 自定义宽高：只在纸张选"自定义"时露出来（固定规格的尺寸是规格本身的一部分）。
  const customW = field('宽', 'page-width', o.pageSizePt().w / CM_PT);
  const customH = field('高', 'page-height', o.pageSizePt().h / CM_PT);
  const customBox = document.createElement('div');
  customBox.className = 'heditor-page-setup__grid';
  customBox.append(customW.row, customH.row);

  /**
   * 按当前纸张档位决定"自定义宽高"那一行露不露。
   */
  const syncCustom = (): void => {
    customBox.hidden = paper.value !== 'Custom';
  };
  paper.addEventListener('change', syncCustom);

  const paperRow = document.createElement('div');
  paperRow.className = 'heditor-page-setup__row';
  const paperLabel = document.createElement('span');
  paperLabel.className = 'heditor-page-setup__label';
  paperLabel.textContent = '纸张';
  paperRow.append(paperLabel, paper);

  const orientRow = document.createElement('div');
  orientRow.className = 'heditor-page-setup__row';
  const orientLabel = document.createElement('span');
  orientLabel.className = 'heditor-page-setup__label';
  orientLabel.textContent = '方向';
  orientRow.append(orientLabel, orient);

  el.append(head, paperRow, customBox, orientRow, grid, apply);
  syncCustom();

  return {
    el,
    /**
     * 重读状态（面板每次打开都是新的一份，这里只为与其它组件同形）。
     */
    update(): void {
      const cur = o.current();
      paper.value = cur.paper;
      orient.value = cur.landscape ? 'landscape' : 'portrait';
      const size = cur.paper === 'Custom' && cur.custom ? cur.custom : o.pageSizePt();
      customW.input.value = (size.w / CM_PT).toFixed(2);
      customH.input.value = (size.h / CM_PT).toFixed(2);
      syncCustom();
      top.input.value = (cur.margins.top / CM_PT).toFixed(2);
      bottom.input.value = (cur.margins.bottom / CM_PT).toFixed(2);
      left.input.value = (cur.margins.left / CM_PT).toFixed(2);
      right.input.value = (cur.margins.right / CM_PT).toFixed(2);
    },
  };
}
