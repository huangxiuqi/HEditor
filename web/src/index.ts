/**
 * 库的入口。对外只导出两样东西：createEditor（造一个编辑器，实现见 editor.ts）与
 *   契约里的类型（Editor / EditorOptions / 事件与配置的形状）。
 *
 * 不导出实现类、不导出内部模块。TypeScript 里 export 就是公开面 —— 实现类一旦导出，
 *   它所有非 private 的成员都会进 dist/index.d.ts，那就是"111 个成员"的 TS 版
 *   （见 docs/外部API设计.md §10.3）。
 */
export { createEditor } from './editor';
export type * from './types';
