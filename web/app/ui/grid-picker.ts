// 表格尺寸选择器：一张会随指针长亮的网格，点哪一格就插多大的表。
//
// 为什么不用两个下拉框（行数、列数）：尺寸是"看起来多大"的一件事，网格把它画出来了 ——
//   两个下拉框要先在脑子里拼一次。Word 与 WPS 都是这张网格，用的人不用学。
//
// 组件只产出"选了多少行多少列"，不碰编辑器：动作由调用方给（见 main.ts 的动作表）。

/**
 * 造这张网格要的参数。
 */
export interface GridPickerOptions {
  /**
   * 初始行数（高亮到这一格）。
   */
  rows: number;

  /**
   * 初始列数。
   */
  cols: number;

  /**
   * 网格最多画几行（也是可选的上限）。
   */
  maxRows: number;

  /**
   * 网格最多画几列。
   */
  maxCols: number;

  /**
   * 是否带标题行（首行做表头）。
   */
  header: boolean;

  /**
   * 选中之后做什么。
   * @param rows 行数
   * @param cols 列数
   * @param header 首行是否表头
   */
  onPick(rows: number, cols: number, header: boolean): void;

  /**
   * 收工：面板该关掉了（选完立刻关，与点空白关闭同一条时机）。
   */
  dismiss(): void;
}

/**
 * 造一个表格尺寸选择器。
 * @param o 参数
 * @returns 组件根元素（update 只重读标题行那一行文字）
 */
export function createGridPicker(o: GridPickerOptions): { el: HTMLElement; update(): void } {
  const el = document.createElement('div');
  el.className = 'heditor-picker';

  const head = document.createElement('div');
  head.className = 'heditor-picker__head';
  el.appendChild(head);

  const grid = document.createElement('div');
  grid.className = 'heditor-picker__grid';
  grid.style.gridTemplateColumns = `repeat(${o.maxCols}, 18px)`;
  el.appendChild(grid);

  const foot = document.createElement('label');
  foot.className = 'heditor-picker__foot';
  const box = document.createElement('input');
  box.type = 'checkbox';
  box.checked = o.header;
  box.dataset.act = 'table-header';
  const footText = document.createElement('span');
  foot.append(box, footText);
  el.appendChild(foot);

  let rows = Math.max(1, Math.min(o.rows, o.maxRows));
  let cols = Math.max(1, Math.min(o.cols, o.maxCols));

  /**
   * 按当前 rows / cols 重画那行读数。
   *
   * 不给默认尺寸涂长亮：那几格比别的亮，看着像"这几格坏了 / 已经选中了"，
   *   而指针还没扫过任何一格。长亮只在指针扫过时给（--hover），读数由标题那一行说。
   */
  const paint = (): void => {
    head.textContent = `${rows} × ${cols}`;
    footText.textContent = '标题行（首行做表头）';
    for (const cell of Array.from(grid.children)) {
      cell.classList.remove('heditor-picker__cell--on', 'heditor-picker__cell--hover');
    }
  };

  for (let r = 1; r <= o.maxRows; ++r) {
    for (let c = 1; c <= o.maxCols; ++c) {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'heditor-picker__cell';
      b.dataset.row = String(r);
      b.dataset.col = String(c);
      b.title = `${r} × ${c}`;
      // 悬停只改高亮，不写回 rows / cols：指针扫过不等于选中，
      //   否则移开时高亮会停在一个用户没打算选的位置上。
      b.addEventListener('pointerenter', () => {
        head.textContent = `${r} × ${c}`;
        for (const cell of Array.from(grid.children)) {
          const x = cell as HTMLElement;
          const on = Number(x.dataset.row) <= r && Number(x.dataset.col) <= c;
          x.classList.toggle('heditor-picker__cell--hover', on);
        }
      });
      // 按下就选：等 click 会被外面的 pointerdown 关闭处理抢在前面。
      b.addEventListener('pointerdown', (ev) => {
        ev.preventDefault();
        o.onPick(r, c, box.checked);
        o.dismiss();
      });
      grid.appendChild(b);
    }
  }
  grid.addEventListener('pointerleave', () => {
    for (const cell of Array.from(grid.children)) {
      cell.classList.remove('heditor-picker__cell--hover');
    }
    paint();
  });
  box.addEventListener('change', () => {
    o.header = box.checked;
  });

  paint();

  return {
    el,
    /**
     * 重读状态（面板每次打开都是新的一份，这里只为与其它组件同形）。
     */
    update(): void {
      paint();
    },
  };
}
