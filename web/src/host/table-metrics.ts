/**
 * 表格格线拖拽的几何规则：拖完一条线，列宽与行高各是多少。
 *
 * 这两条规则是"拖哪条线改哪几列"的全部决定，而它们各带一个反直觉的约束
 *   （总宽不变、最右那条只加宽最后一列）。写在拖拽的事件处理里就只能靠读代码发现，
 *   所以单独放一处，让边界一眼可见。
 */

// 列宽下限（磅）。比它更窄时，格里的字会挤成一列竖条。
export const MIN_COL_WIDTH_PT = 12;

// 行高下限（磅）。
export const MIN_ROW_HEIGHT_PT = 8;

/**
 * 拖一条列边界之后的整份列宽。
 *
 * 相邻两列总宽不变：拖一条边界不该把整张表撑宽或缩窄（Word 同一条）。最右那条边界
 *   只有左边一列可以改 —— 按两列分摊会把表宽钉在原处，拖起来像"拖不动"。
 *
 * @param widths 拖之前的列宽（磅）
 * @param i 边界序号；等于 widths.length 表示最右边那条
 * @param dx 位移（磅，向右为正）
 * @returns 新的列宽数组（不改原数组）
 */
export function columnWidthsAfterDrag(widths: number[], i: number, dx: number): number[] {
  const next = widths.slice();
  if (i >= widths.length) {
    next[i - 1] = Math.max(MIN_COL_WIDTH_PT, widths[i - 1] + dx);
    return next;
  }
  const total = widths[i - 1] + widths[i];
  const left = Math.max(MIN_COL_WIDTH_PT, Math.min(total - MIN_COL_WIDTH_PT, widths[i - 1] + dx));
  next[i - 1] = left;
  next[i] = total - left;
  return next;
}

/**
 * 拖一条行边界之后的行高。
 * @param h0 拖之前的高度（磅）
 * @param dy 位移（磅，向下为正）
 * @returns 新高度（磅）
 */
export function rowHeightAfterDrag(h0: number, dy: number): number {
  return Math.max(MIN_ROW_HEIGHT_PT, h0 + dy);
}
