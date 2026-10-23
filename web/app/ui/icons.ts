/// <reference types="vite/client" />
// 图标：一个图形一个文件（web/app/icons/*.svg），打包时按原文读进来。
//
// 为什么是文件而不是内联字符串：图形是这一页唯一"非逻辑"的东西，放在 .ts 里既没法预览，
//   也没法让设计工具改。按原文读进来之后，图形里写的 currentColor 仍然跟随文字颜色，
//   CSS 只要管颜色，不用管图标本身。
//
// 用 glob 而不是逐个 import：加一个图标只该新增一个文件，不该再改一行代码。
const FILES = import.meta.glob<string>('../icons/*.svg', {
  query: '?raw',
  import: 'default',
  eager: true,
});

const BY_NAME = new Map<string, string>();
for (const [path, text] of Object.entries(FILES)) {
  const name = path.slice(path.lastIndexOf('/') + 1, -'.svg'.length);
  BY_NAME.set(name, text);
}

/**
 * 取一个图标的标记文本。
 * @param name 文件名（不含扩展名）
 * @returns SVG 文本；没有这个图标时为空串
 */
export function iconHtml(name: string): string {
  return BY_NAME.get(name) ?? '';
}

/**
 * 有哪些图标（诊断与排错用：名字写错时能立刻看出有没有这个文件）。
 * @returns 图标名，按字母序
 */
export function iconNames(): string[] {
  return [...BY_NAME.keys()].sort();
}
