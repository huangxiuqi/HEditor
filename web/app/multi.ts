/// <reference types="vite/client" />
// 同页两个实例：一个同线程、一个 Worker。
//
// 这一页守的是"多实例"这件事本身：两份引擎（一份在页面线程、一份在另一条线程）同时
//   跑，文本、光标、撤销栈、页画布互不影响；销毁其中一个，另一个照常编辑。
//   判据见 scripts/multi_probe.mjs。
import { createEditor } from '../src/index';
import type { Editor } from '../src/index';
import { mountChrome } from './ui';

declare global {
  interface Window {
    __demoA?: Editor;
    __demoB?: Editor;
    __editors?: Record<string, Editor>;
    __demoReady?: boolean;
  }
}

const TEXT_A = ['实例 A（inline）', '', '这一栏的编辑不该影响右边。'].join('\n');
const TEXT_B = ['实例 B（worker）', '', '这一栏的引擎在另一条线程上跑。'].join('\n');

const chrome = mountChrome(document.getElementById('bar')!, 'HEditor · 两个实例', []);

/**
 * 造一个实例并把示例文本放进去。
 *
 * @param id 容器元素 id
 * @param transport 传输方式
 * @param text 初始文本
 * @returns 编辑器实例
 */
async function make(id: string, transport: 'inline' | 'worker', text: string): Promise<Editor> {
  const container = document.getElementById(id)!;
  const editor = await createEditor({
    container,
    transport,
    chrome: true,
    debug: new URLSearchParams(location.search).has('debug'),
    // Worker 壳是构建产物，位置从页面往上找（与示例页同一口径）。
    assets: transport === 'worker'
      ? { worker: new URL('../dist/heditor_worker.js', new URL('.', location.href)).href }
      : undefined,
  });
  await editor.setText(text);
  return editor;
}

async function boot(): Promise<void> {
  const a = await make('ed-a', 'inline', TEXT_A);
  const b = await make('ed-b', 'worker', TEXT_B);

  window.__demoA = a;
  window.__demoB = b;
  window.__editors = { a, b };

  // 事件处理器要容得下"另一个实例已经被销毁"：这一页支持逐个拆，拆除之后它照样会
  //   收到另一个实例的 change（监听器挂在各自实例上，不共享）。
  const describe = (e: Editor): string => {
    try {
      const g = e.geometry();
      return `${e.paragraphs().length} 段 / ${g.pages} 页`;
    } catch {
      return '已销毁';
    }
  };
  const report = (): void => chrome.say(`A ${describe(a)} · B ${describe(b)}`);
  a.on('change', report);
  b.on('change', report);
  report();

  window.__demoReady = true;

  if (import.meta.hot) {
    import.meta.hot.dispose(() => {
      void a.destroy();
      void b.destroy();
    });
  }
}

void boot();
