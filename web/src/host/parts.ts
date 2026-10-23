// 宿主骨架的三段：舞台（滚动容器）、页容器（定位祖先）、承接层（IME 的 textarea）。
//
// 用类名而不是 id：同一个页面上可以有几个编辑器实例，id 只能有一个 —— 两份骨架带同一个
//   id 时，document.getElementById 只会给第一个实例，而"库自己用哪一份"取决于它是按
//   容器找还是按文档找。按类名 + 容器作用域找，多实例天然各看各的。
//
// 只认类名，不认旧骨架的 id：项目在开发阶段，没有已经按老标记写好的页面要照顾，留一条
//   兼容路径就多一条要测的分支。
/**
 * 舞台的类名。
 */
export const PART_STAGE = 'heditor-stage';
/**
 * 页容器的类名。
 */
export const PART_PAGES = 'heditor-pages';
/**
 * 承接层的类名。
 */
export const PART_IME = 'heditor-ime';

/**
 * 输入法预编辑串的显示层：组合中的字画在这里（承接层是透明的，浏览器画在它里面的
 * 组合串用户看不见）。它不是骨架的必要部分，找不到就当"没有这一层"。
 */
export const PART_PREEDIT = 'heditor-preedit';

/**
 * 找舞台（滚动容器）。
 * @param root 作用域（通常就是挂载点，绝不传 document）
 * @returns 元素；没有就为 null
 */
export function stageOf(root: ParentNode): HTMLElement | null {
  return root.querySelector<HTMLElement>('.' + PART_STAGE);
}

/**
 * 找页容器（画布的定位祖先）。
 * @param root 作用域
 * @returns 元素；没有就为 null
 */
export function pagesOf(root: ParentNode): HTMLElement | null {
  return root.querySelector<HTMLElement>('.' + PART_PAGES);
}

/**
 * 找承接层（真正的 textarea）。
 * @param root 作用域
 * @returns 元素；没有就为 null
 */
export function imeOf(root: ParentNode): HTMLTextAreaElement | null {
  return root.querySelector<HTMLTextAreaElement>('.' + PART_IME);
}
