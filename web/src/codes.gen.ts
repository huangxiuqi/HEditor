// 由 scripts/gen_codes.py 从 src/core/utils/api_codes.h 生成，不要手改。
// 改常量请改那个头文件，再跑 python scripts/gen_codes.py。

/**
 * 固定纸张的纵向宽高（磅）。横向由 paperSizePt 把宽高对调。
 */
export const PAPER_PT = {
  'A4': { w: 595.276, h: 841.89 },
  'A3': { w: 841.89, h: 1190.551 },
  'A5': { w: 419.528, h: 595.276 },
  'B5': { w: 498.898, h: 708.661 },
  'Letter': { w: 612, h: 792 },
  'Legal': { w: 612, h: 1008 },
} satisfies Record<string, { w: number; h: number }>;

/**
 * 日期单位的位掩码，顺序与日期选择器里的先后一致。
 */
export const DATE_UNIT_BITS: Record<string, number> = {
  'year': 1,
  'month': 2,
  'day': 4,
  'hour': 8,
  'minute': 16,
  'second': 32,
};

/**
 * 图片版式的对外码。anchor 与 wrap 在引擎里是两个字段，对外只有一个码。
 */
export const IMAGE_LAYOUT_CODES: Record<string, number> = {
  'inline': 0,
  'square': 1,
  'topAndBottom': 2,
  'behind': 3,
  'inFront': 4,
};

/**
 * 编辑模式（与 EditMode 枚举同序）。
 */
export const EDIT_MODE_CODES: Record<string, number> = {
  'document': 0,
  'form': 1,
  'readOnly': 2,
};

/**
 * 不可见字符标记（与 MarksMode 枚举同序）。
 */
export const MARKS_MODE_CODES: Record<string, number> = {
  'none': 0,
  'paragraph': 1,
  'all': 2,
};

/**
 * 默认纸张的规格名。空白文档与默认页度量都用它。
 */
export const DEFAULT_PAPER = 'A4';

/**
 * 默认页边距（磅），四边相等（2 厘米）。
 */
export const DEFAULT_MARGIN_PT = 56.7;
