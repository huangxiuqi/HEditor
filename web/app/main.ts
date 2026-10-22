/// <reference types="vite/client" />
// 示例页：Vite 开发模式下直接 import 编辑器源码（不是 dist 产物）。
//
// 这一份是"外部使用者"的样板：只允许用 createEditor 与实例上的方法。
//   不许出现 window.__heditor / api.read / mod / HEAPU8 / FS，也不许自己接键盘指针 ——
//   输入层在库里。判据：node scripts/api_check.mjs（静态）+ scripts/demo_probe.mjs（运行时）。
//
// 与库的边界只有三处：createEditor 的选项、实例方法、事件。
import { createEditor } from '../src/index';
import type { Editor } from '../src/index';
import { mountChrome, type Action } from './ui';

const DEMO_TEXT = [
  'HEditor 示例文档',
  '',
  '这一页由 Vite 直接跑源码：改 web/src 下的任何文件，保存即热更，不用先构建。',
  '引擎（wasm）、命令表、输入层、画布与滚动都在库里面 —— 页面拿到的是一个编辑器对象。',
  '',
  '可以试试：点画布落光标、敲字、Ctrl+Z 撤销、拖选。这些都没有在本文件里写一行事件处理。',
].join('\n');

const params = new URLSearchParams(location.search);
// 默认走 Worker：它就是"引擎跑在另一条线程上"的那一档，产品里该按它验。要复现
//   同线程那条路显式写 ?transport=inline —— 两条路的差别只在"命令是在事件处理器里
//   同步跑完，还是一次往返之后跑完"，很多时序问题只在其中一条上显形。
const transport = params.get('transport') === 'inline' ? 'inline' : 'worker';
// 探针用 ?glue= 造"产物不存在"这种真实配置错误。产品页不会有这一行。
const glue = params.get('glue');
// Worker 壳是构建产物（经典 worker，要 importScripts 装胶水），开发模式下也用构建好的那一份。
// 它是从页面所在目录往上找 dist/：开发时页面在 web/（根），构建时在 web/dist-app/ —— 两种
// 摆法下 "上一级 + dist" 都指向 web/dist/heditor_worker.js。
const workerShell = new URL('../dist/heditor_worker.js', new URL('.', location.href)).href;

/** 页面与库之间只有这一个句柄，探针也用它。 */
declare global {
  interface Window {
    __demo?: Editor;
    __demoReady?: boolean;
    __demoState?: { changes: number; transport: string };
    __demoStartError?: string;
  }
}

const stage = document.getElementById('ed')!;

let editor: Editor;
let pageMode: 'paged' | 'continuous' = 'paged';

const actions: Action[] = [
  { label: '载入示例', act: 'load', run: () => editor.setText(DEMO_TEXT) },
  { label: '插入一句', act: 'type', run: () => editor.type('（从这里开始编辑）') },
  { label: '撤销', act: 'undo', run: () => editor.undo() },
  { label: '重做', act: 'redo', run: () => editor.redo() },
  {
    label: '放大',
    act: 'zoomIn',
    run: () => editor.setZoom(Math.min(2.25, editor.config().zoom + 0.125)),
  },
  {
    label: '缩小',
    act: 'zoomOut',
    run: () => editor.setZoom(Math.max(0.375, editor.config().zoom - 0.125)),
  },
  {
    label: '切连页',
    act: 'toggleMode',
    run: () => {
      pageMode = pageMode === 'paged' ? 'continuous' : 'paged';
      return editor.configure({ pageMode });
    },
  },
  {
    label: '保存',
    act: 'save',
    run: async () => {
      const json = await editor.save();
      // 示例里只把结果放到控制台；产品里该做的是下载或上传。
      console.log('save:', json.length, 'bytes');
      chrome.say(`已序列化 ${json.length} 字节`);
    },
  },
];

const chrome = mountChrome(document.getElementById('bar')!, 'HEditor', actions);

async function boot(): Promise<void> {
  editor = await createEditor({
    container: stage,
    transport,
    chrome: true,
    // debug 打开的是诊断面（diag）：它不承诺稳定，只给探针与内部验证台。
    debug: params.has('debug'),
    assets: {
      ...(glue ? { glue } : {}),
      // Worker 壳必须是经典 worker（importScripts），所以开发模式下也用构建好的那一份。
      ...(transport === 'worker' ? { worker: workerShell } : {}),
    },
    // 引擎起不来时给用户一条出路：库不重启自己，重启这一页是页面的账。
    onFatalRetry: () => location.reload(),
  }).catch((e) => {
    // 起不来时库已经在容器里画了横幅；这里只把原因留给探针（产品页该做的是上报）。
    window.__demoStartError = String((e && e.message) || e);
    throw e;
  });

  // 页面创建的实例只能这样交给页面外的脚本：evaluate 在页面里跑，拿不到模块引用。
  //   它同时是测试的唯一入口（web/tests/support/readings.ts 的前言里读一次），改名字要同步。
  window.__demo = editor;
  window.__demoState = { changes: 0, transport };
  editor.on('change', () => {
    const n = ++window.__demoState!.changes;
    const g = editor.geometry();
    chrome.say(`${editor.paragraphs().length} 段 · ${g.pages} 页 · 变更 ${n} 次`);
  });

  await editor.setText(DEMO_TEXT);
  const g = editor.geometry();
  chrome.say(`${editor.paragraphs().length} 段 · ${g.pages} 页 · 就绪`);

  // 探针的排空点：所有 fire-and-forget 的调用之后读这个。
  window.__demoReady = true;

  // 热更时先把编辑器收掉，避免同一块容器上叠两个实例。
  if (import.meta.hot) import.meta.hot.dispose(() => void editor.destroy());
}

void boot();
