// 状态栏：左边页数 / 字数加一行状态，右边缩放（减 / 滑块 / 加 / 百分比）。
//
// 它自己不认识编辑器：缩放只是两个回调（一步一格、直接给百分比），页数与字数由宿主
//   算好喂进来。这样这一件在没有实例的页面上（比如"两个实例"那一页）也能挂，
//   只是那几处不接电。
import { iconHtml } from './icons';

/**
 * 状态栏的两个写入口。
 */
export interface StatusBar {
  /**
   * 根元素。
   */
  el: HTMLElement;

  /**
   * 写左边那行状态。
   * @param text 要说的话
   */
  say(text: string): void;

  /**
   * 读左边那行状态。
   * @returns 状态栏文字
   */
  text(): string;

  /**
   * 改右边的百分比读数与滑块位置。
   * @param pct 百分比（100 = 原大）
   */
  setZoomPct(pct: number): void;

  /**
   * 写左边的页数与字数。
   * @param pages 总页数
   * @param words 总字数（字符数，不计空白）
   */
  setCounts(pages: number, words: number): void;
}

/**
 * 造状态栏。
 *
 * 回调留空时右边那组只是画着不动：这一页没有编辑器，缩放无从谈起。
 * @param onStep 按减号 / 加号（参数是方向：-1 缩小、1 放大）
 * @param onSet 拖滑块（参数是百分比）
 * @returns 状态栏
 */
export function createStatusBar(
  onStep?: (dir: number) => void,
  onSet?: (pct: number) => void,
): StatusBar {
  const el = document.createElement('footer');
  el.className = 'heditor-status';

  // 页数与字数：左下角常驻的两个数。它们与右边那行状态分开放 —— 那一行是"这一次
  //   操作说了什么"，会变；这两个数是文档的规模，只在内容变了之后才动。
  const counts = document.createElement('span');
  counts.className = 'heditor-status__counts';
  const pageCount = document.createElement('span');
  pageCount.className = 'heditor-status__count';
  pageCount.dataset.count = 'pages';
  pageCount.textContent = '1 页';
  const wordCount = document.createElement('span');
  wordCount.className = 'heditor-status__count';
  wordCount.dataset.count = 'words';
  wordCount.textContent = '0 字';
  counts.append(pageCount, wordCount);

  const statusText = document.createElement('span');
  // 这个 id 是页面对外的约定（状态栏文字），类名只管样式。
  statusText.id = 'status';
  statusText.className = 'heditor-status__text';
  statusText.textContent = '启动中…';

  const right = document.createElement('span');
  right.className = 'heditor-status__right';

  const out = makeStep('out', '缩小', -1, onStep);
  const range = document.createElement('input');
  range.className = 'heditor-status__range';
  range.type = 'range';
  range.min = '37';
  range.max = '225';
  range.step = '12.5';
  range.value = '100';
  range.title = '缩放';
  range.addEventListener('input', () => {
    onSet?.(Number(range.value));
  });
  const into = makeStep('in', '放大', 1, onStep);

  const pct = document.createElement('b');
  pct.className = 'heditor-status__pct';
  pct.textContent = '100%';

  right.append(out.el, range, into.el, pct);
  el.append(counts, statusText, right);

  return {
    el,

    /**
     * 写左边那行状态。
     * @param text 要说的话
     */
    say(text: string): void {
      statusText.textContent = text;
    },

    /**
     * 读左边那行状态。
     * @returns 状态栏文字
     */
    text(): string {
      return statusText.textContent ?? '';
    },

    /**
     * 改右边的百分比读数与滑块位置。
     * @param value 百分比
     */
    setZoomPct(value: number): void {
      range.value = String(value);
      pct.textContent = Math.round(value) + '%';
    },

    /**
     * 写左边的页数与字数。
     * @param pages 总页数
     * @param words 总字数（字符数，不计空白）
     */
    setCounts(pages: number, words: number): void {
      pageCount.textContent = Math.max(1, Math.round(pages)) + ' 页';
      wordCount.textContent = words.toLocaleString('zh-CN') + ' 字';
    },
  };
}

/**
 * 造一个"减 / 加"按钮。
 * @param dir 方向（写进 data-zoom）
 * @param label 悬停提示
 * @param step 传给 onStep 的方向
 * @param onStep 回调
 * @returns 按钮元素
 */
function makeStep(
  dir: string,
  label: string,
  step: number,
  onStep?: (dir: number) => void,
): { el: HTMLButtonElement } {
  const el = document.createElement('button');
  el.className = 'heditor-status__step';
  el.dataset.zoom = dir;
  el.title = label;
  el.innerHTML = iconHtml(dir === 'in' ? 'zoom-in' : 'zoom-out');
  el.addEventListener('click', () => {
    onStep?.(step);
  });
  return { el };
}
