/// <reference types="vite/client" />
// 示例页：Vite 开发模式下直接 import 编辑器源码（不是 dist 产物）。
//
// 这一份是"外部使用者"的样板：只允许用 createEditor 与实例上的方法。
//   不许出现 window.__heditor / api.read / mod / HEAPU8 / FS，也不许自己接键盘指针 ——
//   输入层在库里。判据：node scripts/api_check.mjs（静态）+ web/tests 下那几份用例。
//
// 与库的边界只有三处：createEditor 的选项、实例方法、事件。
import { createEditor } from '../src/index';
import type { CharFormatPatch, Editor, ParaFormatPatch } from '../src/index';
import { createGridPicker, mountChrome, type Chrome, type Group, type Tab } from './ui';

const DEMO_TEXT = [
  'HEditor 示例文档',
  '',
  '这一页由 Vite 直接跑源码：改 web/src 下的任何文件，保存即热更，不用先构建。',
  '引擎（wasm）、命令表、输入层、画布与滚动都在库里面 —— 页面拿到的是一个编辑器对象。',
  '',
  '可以试试：点画布落光标、敲字、Ctrl+Z 撤销、拖选。这些都没有在本文件里写一行事件处理。',
  '',
  '上面那排按钮全部走公开面：格式走 format / paragraphFormat，回显走 formatAt。',
].join('\n');

const params = new URLSearchParams(location.search);
// 探针用 ?glue= 造"产物不存在"这种真实配置错误。产品页不会有这一行。
const glue = params.get('glue');
// Worker 壳是构建产物（经典 worker，要 importScripts 装胶水），开发模式下也用构建好的那一份。
// 它是从页面所在目录往上找 dist/：开发时页面在 web/（根），构建时在 web/dist-app/ —— 两种
// 摆法下"上一级 + dist"都指向 web/dist/heditor_worker.js。
const workerShell = new URL('../dist/heditor_worker.js', new URL('.', location.href)).href;

/** 页面与库之间只有这一个句柄，探针也用它。 */
declare global {
  interface Window {
    __demo?: Editor;
    __demoReady?: boolean;
    __demoState?: { changes: number };
    __demoStartError?: string;
  }
}

const stage = document.getElementById('ed')!;

let editor: Editor;
let chrome: Chrome;

/** 字号候选；"增大/减小"就在这一列上走，界面上只给固定档位。 */
const SIZES = [9, 10, 10.5, 12, 14, 16, 18, 22, 24, 36, 48];

/**
 * 页面缓存的那份格式，供工具条同步回显。
 *
 * 工具条的回显闭包是同步的（一次光标移动不该变成七八个来回），所以格式由这一侧一次读全，
 *   读完叫一声 refresh。缓存只用于回显 —— 读改写型的动作一律现读，见 toggle / stepSize。
 */
let mark: CharFormatPatch = {};
let para: ParaFormatPatch = {};

/** 动作的键：三个开关型字符格式。 */
type MarkKey = 'bold' | 'italic' | 'underline';

/** 正在读格式：期间来的刷新请求先记下，读完再补一轮。 */
let reading = false;
/** 读的期间又有人要刷新。 */
let stale = false;

/**
 * 读一次格式并刷新工具条。
 *
 * 读与写走同一条通道，上一秒发起的读可能带回上一秒的状态。所以：读在进行时不再并发，
 *   只记一笔"有新变化"，落地后再补一轮 —— 补的那一轮一定能看见刚才写进去的值。
 */
async function syncFormat(): Promise<void> {
  if (reading) {
    stale = true;
    return;
  }
  reading = true;
  try {
    do {
      stale = false;
      // 字符格式读"实际生效的那一份"：正文一个字都没改过时，显式标记是空的，
      //   而字体与字号在画面上明明有值 —— 工具栏要显示的就是那个值。
      const [m, p] = await Promise.all([
        editor.formatAt({ effective: true }),
        editor.paragraphFormatAt(),
      ]);
      mark = m;
      para = p;
      chrome.refresh();
    } while (stale);
  } finally {
    reading = false;
  }
}

/**
 * 动作队列：格式是"读—改—写"，两次点击必须一前一后。
 *
 * 不等上一条落地就发下一条，第二次读到的还是第一次写之前的值，于是两次算出来的目标值
 *   相同 —— 表现就是"连点两下只动了一档"。
 */
let queue: Promise<unknown> = Promise.resolve();

/**
 * 排一个动作到队尾。
 * @param task 要做的事
 * @returns 这一次的完成信号
 */
function enqueue(task: () => Promise<void>): Promise<void> {
  const next = queue.then(task, task);
  queue = next.catch(() => undefined);
  return next;
}

/**
 * 写一次字符格式，写完重新回显。
 * @param patch 要改的字段
 */
async function writeMark(patch: CharFormatPatch): Promise<void> {
  await editor.format(patch);
  await syncFormat();
}

/**
 * 写一次段落格式，写完重新回显。
 * @param patch 要改的字段
 */
async function writePara(patch: ParaFormatPatch): Promise<void> {
  await editor.paragraphFormat(patch);
  await syncFormat();
}

/**
 * 切换一个开关型字符格式。
 *
 * 取反用的是"现读的值"而不是缓存：工具栏上的那个按钮亮不亮，取决于缓存什么时候刷新完，
 *   不该决定点下去会不会生效。
 * @param key 哪个开关
 */
function toggle(key: MarkKey): void {
  void enqueue(async () => {
    const now = await editor.formatAt({ effective: true });
    await writeMark({ [key]: !now[key] });
  });
}

/**
 * 按档位放大或缩小字号：取当前字号在候选列上的下一个（上一个）。
 * @param dir 1 = 变大，-1 = 变小
 */
function stepSize(dir: number): void {
  void enqueue(async () => {
    // 实际生效的那一份总有字号；这里的兜底只是防契约被改坏。
    const cur = (await editor.formatAt({ effective: true })).size ?? 12;
    const next = dir > 0
      ? SIZES.find((s) => s > cur) ?? SIZES[SIZES.length - 1]
      : [...SIZES].reverse().find((s) => s < cur) ?? SIZES[0];
    if (next !== cur) await writeMark({ size: next });
  });
}

/**
 * 改缩放倍数（夹在库的上下限里）。
 * @param z 目标倍数
 */
async function zoomTo(z: number): Promise<void> {
  await editor.setZoom(z);
  chrome.refresh();
}

/**
 * 保存：序列化当前文档，把长度写到状态栏。
 */
async function saveNow(): Promise<void> {
  const json = await editor.save();
  chrome.say(`已序列化 ${json.length} 字节`);
}

/**
 * 造一个"字体颜色"按钮要用的取色器。
 *
 * 用浏览器自己的取色器而不是画一个调色板：这一条是示例，重点在"公开面能把颜色写进去"，
 *   而自绘调色板要处理键盘、焦点与关闭时机，与这一页要示范的东西无关。
 * @returns 点一下就会弹出取色器的入口
 */
function makeColorPicker(): () => void {
  const input = document.createElement('input');
  input.type = 'color';
  input.value = '#000000';
  input.style.display = 'none';
  document.body.appendChild(input);
  input.addEventListener('change', () => {
    const at = (i: number): number => Number.parseInt(input.value.slice(i, i + 2), 16);
    void enqueue(() => writeMark({ color: [at(1), at(3), at(5)] }));
  });
  return () => input.click();
}

/**
 * 这一页的动作表：三个页签，全部落在公开面上。
 * @returns 页签表
 */
function makeTabs(): Tab[] {
  const pickColor = makeColorPicker();

  const doc: Group = {
    label: '文档',
    items: [
      { act: 'load', label: '载入示例', icon: 'doc', big: true, run: () => editor.setText(DEMO_TEXT) },
      { act: 'save', label: '保存', icon: 'save', big: true, run: () => saveNow() },
    ],
  };

  const clipboard: Group = {
    label: '剪贴板',
    items: [
      { act: 'paste', label: '粘贴', icon: 'paste', big: true, run: () => editor.paste() },
      {
        items: [
          { act: 'cut', label: '剪切', icon: 'cut', run: () => editor.cut() },
          { act: 'copy', label: '复制', icon: 'copy', run: () => editor.copy() },
        ],
      },
    ],
  };

  const font: Group = {
    label: '字体',
    items: [
      {
        act: 'font',
        label: '字体',
        wide: true,
        options: [
          { value: '', text: '—' },
          { value: '0', text: '思源黑体' },
          { value: '1', text: '蒙古文' },
          { value: '2', text: '拉丁' },
        ],
        value: () => (mark.font === undefined ? '' : String(mark.font)),
        run: (v) => {
          if (!v) return;
          void enqueue(() => writeMark({ font: Number(v) }));
        },
      },
      {
        act: 'size',
        label: '字号',
        options: [{ value: '', text: '—' }].concat(SIZES.map((s) => ({ value: String(s), text: String(s) }))),
        value: () => (mark.size === undefined ? '' : String(mark.size)),
        run: (v) => {
          if (!v) return;
          void enqueue(() => writeMark({ size: Number(v) }));
        },
      },
      { act: 'grow', label: '增大字号', icon: 'grow-font', run: () => stepSize(1) },
      { act: 'shrink', label: '减小字号', icon: 'shrink-font', run: () => stepSize(-1) },
      { act: 'bold', label: '加粗', icon: 'bold', on: () => !!mark.bold, run: () => toggle('bold') },
      {
        act: 'italic',
        label: '斜体',
        icon: 'italic',
        on: () => !!mark.italic,
        run: () => toggle('italic'),
      },
      {
        act: 'underline',
        label: '下划线',
        icon: 'underline',
        on: () => !!mark.underline,
        run: () => toggle('underline'),
      },
      { act: 'color', label: '字体颜色', icon: 'color', run: pickColor },
    ],
  };

  const paragraph: Group = {
    label: '段落',
    items: [
      {
        act: 'line',
        label: '行距',
        icon: 'line-spacing',
        options: [
          { value: '', text: '—' },
          { value: '1', text: '1.0' },
          { value: '1.25', text: '1.25' },
          { value: '1.5', text: '1.5' },
          { value: '1.75', text: '1.75' },
          { value: '2', text: '2.0' },
          { value: '3', text: '3.0' },
        ],
        value: () => (para.lineFactor === undefined ? '' : String(para.lineFactor)),
        run: (v) => {
          if (!v) return;
          void enqueue(() => writePara({ lineFactor: Number(v) }));
        },
      },
      {
        act: 'after',
        label: '段后',
        icon: 'paragraph',
        options: [
          { value: '', text: '—' },
          { value: '0', text: '0' },
          { value: '6', text: '6 磅' },
          { value: '12', text: '12 磅' },
          { value: '18', text: '18 磅' },
        ],
        value: () => (para.spaceAfter === undefined ? '' : String(para.spaceAfter)),
        run: (v) => {
          if (!v) return;
          void enqueue(() => writePara({ spaceAfter: Number(v) }));
        },
      },
      { act: 'outdent', label: '减少缩进', icon: 'outdent', run: () => editor.setIndent(-1) },
      { act: 'indent', label: '增加缩进', icon: 'indent', run: () => editor.setIndent(1) },
    ],
  };

  // 撤销与重做是一对，样式必须一样：一个带文字一个大图标，读起来像两个不同级别的动作。
  const edit: Group = {
    label: '编辑',
    items: [
      {
        act: 'undo',
        label: '撤销',
        icon: 'undo',
        big: true,
        run: () => void enqueue(async () => {
          await editor.undo();
          await syncFormat();
        }),
      },
      {
        act: 'redo',
        label: '重做',
        icon: 'redo',
        big: true,
        run: () => void enqueue(async () => {
          await editor.redo();
          await syncFormat();
        }),
      },
      { act: 'all', label: '全选', icon: 'select-all', run: () => editor.selectAll() },
    ],
  };

  const show: Group = {
    label: '显示',
    items: [
      {
        act: 'marks',
        label: '编辑标记',
        options: [
          { value: 'none', text: '不显示' },
          { value: 'paragraph', text: '段落标记' },
          { value: 'all', text: '全部标记' },
        ],
        value: () => editor.config().showMarks,
        run: (v) => {
          if (!v) return;
          // 配完重读一遍回显：configure 不触发 caret / change，不主动刷新的话工具条会
          //   停在上一档（下拉框显示的与实际生效的不一致）。
          void editor
            .configure({ showMarks: v as 'none' | 'paragraph' | 'all' })
            .then(() => chrome.refresh());
        },
      },
      {
        act: 'paged',
        label: '分页',
        icon: 'paged',
        on: () => editor.config().pageMode === 'paged',
        run: () => editor.configure({ pageMode: 'paged' }),
      },
      {
        act: 'continuous',
        label: '连页',
        icon: 'continuous',
        on: () => editor.config().pageMode === 'continuous',
        run: () => editor.configure({ pageMode: 'continuous' }),
      },
    ],
  };

  const zoom: Group = {
    label: '缩放',
    items: [
      { act: 'zoom-out', label: '缩小', icon: 'zoom-out', run: () => zoomTo(editor.config().zoom - 0.125) },
      { act: 'zoom-in', label: '放大', icon: 'zoom-in', run: () => zoomTo(editor.config().zoom + 0.125) },
      { act: 'zoom-reset', label: '100%', icon: 'zoom-reset', run: () => zoomTo(1) },
    ],
  };

  const insert: Group = {
    label: '插入',
    items: [
      {
        act: 'type',
        label: '插入一句',
        icon: 'type',
        big: true,
        run: () => editor.type('（从这里开始编辑）'),
      },
      {
        act: 'table',
        label: '表格',
        icon: 'table',
        big: true,
        // 行列数在网格上挑，不写死一张 3x3。网格本身是复用组件（ui/grid-picker），
        //   这一层只把"选了多少"接到公开面上。
        panel: (dismiss) =>
          createGridPicker({
            rows: 3,
            cols: 3,
            maxRows: 8,
            maxCols: 8,
            header: true,
            onPick: (rows, cols, header) => {
              void editor.insertTable({ rows, cols, header });
            },
            dismiss,
          }),
      },
      {
        act: 'field',
        label: '表单字段',
        icon: 'field',
        big: true,
        run: () => editor.insertControl({ type: 'text', key: 'field', label: '（待填）' }),
      },
    ],
  };

  return [
    { label: '开始', groups: [doc, clipboard, font, paragraph, edit] },
    { label: '插入', groups: [insert] },
    { label: '视图', groups: [show, zoom] },
  ];
}

async function boot(): Promise<void> {
  editor = await createEditor({
    container: stage,
    chrome: true,
    // debug 打开的是诊断面（diag）：它不承诺稳定，只给探针与内部验证台。
    debug: params.has('debug'),
    assets: {
      ...(glue ? { glue } : {}),
      // Worker 壳必须是经典 worker（importScripts），所以开发模式下也用构建好的那一份。
      worker: workerShell,
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
  window.__demoState = { changes: 0 };

  await editor.setText(DEMO_TEXT);
  chrome.bind(editor);

  const touch = (): void => {
    void syncFormat();
  };
  editor.on('caret', touch);
  editor.on('selection', touch);
  editor.on('change', () => {
    const n = ++window.__demoState!.changes;
    const g = editor.geometry();
    chrome.say(`${editor.paragraphs().length} 段 · ${g.pages} 页 · 变更 ${n} 次`);
    // 内容变了，光标处能继承的格式也可能变了（撤销、粘贴、格式命令都走这条路）。
    void syncFormat();
  });

  await syncFormat();
  const g = editor.geometry();
  chrome.say(`${editor.paragraphs().length} 段 · ${g.pages} 页 · 就绪`);

  // 探针的排空点：所有 fire-and-forget 的调用之后读这个。
  window.__demoReady = true;

  // 热更时先把编辑器收掉，避免同一块容器上叠两个实例。
  if (import.meta.hot) import.meta.hot.dispose(() => void editor.destroy());
}

// 工具条必须在实例之前挂好：动作闭包引用 editor 与 chrome，真正按下去时才读到它们。
const tabs = makeTabs();
chrome = mountChrome(document.getElementById('bar')!, 'HEditor', tabs);

void boot();

// 契约与公开成员清单变了就整页重载。
//
// 这两份东西被门面捕获进闭包（白名单在 guard 里、成员清单在 facade 里），热更替不掉
//   已经建好的那个实例 —— 页面会继续用旧白名单，症状是"调了个新加的方法，报它不是
//   公开 API"，看着像代码错了。示例页是给人看的，重载一次比留着旧状态省事。
if (import.meta.hot) {
  // 库与外壳的每一处都在实例化那一刻把闭包捕获下来：宿主的事件处理器、剪贴板模块里的
  //   状态、白名单、协议表 —— 热更只换模块，换不掉已经建好的那个实例。于是页面会
  //   "看着是新的代码、跑的却是旧的"（改了粘贴逻辑却毫无变化，正是这一条）。
  //   代价是一次重载，收益是"改了就能验"；示例页是给人看的，重载不心疼。
  import.meta.hot.on('vite:beforeUpdate', (payload) => {
    const files = (payload?.updates ?? []).map((u) => String(u?.path ?? ''));
    if (files.some((f) => /(^|\/)src\/|(^|\/)app\//.test(f))) {
      location.reload();
    }
  });
}
