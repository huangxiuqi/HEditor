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
import {
  createGridPicker,
  createLayer,
  createPageSetup,
  mountChrome,
  type Chrome,
  type Group,
  type Tab,
} from './ui';
import { createMenus, type Menus } from './menus';

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

// 页面与库之间只有这一个句柄，探针也用它。
declare global {
  interface Window {
    __demo?: Editor;
    __demoReady?: boolean;
    __demoState?: { changes: number; unsavedPrompt: boolean };
    __demoStartError?: string;
  }
}

const stage = document.getElementById('ed')!;

let editor: Editor;
let chrome: Chrome;
// 右键菜单：实例建好之后才有；库的回调是延迟触发的，所以先声明后装配。
let menus: Menus | undefined;

// 字号候选；"增大/减小"就在这一列上走，界面上只给固定档位。
const SIZES = [9, 10, 10.5, 12, 14, 16, 18, 22, 24, 36, 48];

/**
 * 页面缓存的那份格式，供工具条同步回显。
 *
 * 工具条的回显闭包是同步的（一次光标移动不该变成七八个来回），所以格式由这一侧一次读全，
 *   读完叫一声 refresh。缓存只用于回显 —— 读改写型的动作一律现读，见 toggle / stepSize。
 */
let mark: CharFormatPatch = {};
let para: ParaFormatPatch = {};

// 动作的键：三个开关型字符格式。
type MarkKey = 'bold' | 'italic' | 'underline';

/**
 * 有未保存的改动。
 *
 * 只有它才拦"关页面"：一律拦会让刷新变成两步，而"没改过也问一句"是纯粹的打搅。
 */
let dirty = false;

/**
 * 关页面前要不要提醒"还没保存"。默认关闭 —— 提醒是可选行为，不是编辑器的默认态度。
 */
let unsavedPrompt = false;

// 正在读格式：期间来的刷新请求先记下，读完再补一轮。
let reading = false;
// 读的期间又有人要刷新。
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
  // 取反与读写顺序都在库里（toggleFormat）：这里只负责把回显刷新一遍。
  void editor.toggleFormat(key).then(() => syncFormat());
}

/**
 * 按档位放大或缩小字号：取当前字号在候选列上的下一个（上一个）。
 * @param dir 1 = 变大，-1 = 变小
 */
function stepSize(dir: number): void {
  // 阶梯（SIZES）是产品口味，留在页面；取上一档还是下一档由库按当前字号算。
  void editor.stepFontSize(dir > 0 ? 1 : -1, SIZES).then(() => syncFormat());
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
  dirty = false;
  const g = editor.geometry();
  chrome.say(`${editor.paragraphs().length} 段 · 已序列化 ${json.length} 字节（${g.pages} 页）`);
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
    void writeMark({ color: [at(1), at(3), at(5)] });
  });
  return () => input.click();
}

/**
 * 造一个"控件颜色"按钮要用的取色器。
 *
 * 只改光标所在的那个选择类控件：单选、复选、单选组、复选组都走同一条 updateControl，
 *   记号色是控件外观的一部分，改完由引擎重画记号本身。
 * @returns 点一下就会弹出取色器的入口
 */
function makeControlColorPicker(): () => void {
  const input = document.createElement('input');
  input.type = 'color';
  input.value = '#2563eb';
  input.style.display = 'none';
  document.body.appendChild(input);
  const choiceTypes = new Set(['radioButton', 'checkBox', 'radioGroup', 'checkBoxGroup']);
  input.addEventListener('change', () => {
    const at = (i: number): number => Number.parseInt(input.value.slice(i, i + 2), 16);
    const caret = editor.caret().pos;
    // 选择类的光标停不进去（屏障三档都生效），所以不能只看 focused：插完或点完之后
    //   光标在整块之后，要按区间找"光标贴着的那个控件"。
    const target = editor.form().find((c) => {
      if (!choiceTypes.has(c.type)) return false;
      const afterStart =
        caret.para > c.start.para ||
        (caret.para === c.start.para && caret.offset >= c.start.offset);
      const beforeEnd =
        caret.para < c.end.para || (caret.para === c.end.para && caret.offset <= c.end.offset);
      return (afterStart && beforeEnd) || c.focused;
    });
    if (!target) {
      chrome.say('先把光标放到单选 / 复选 / 单选组 / 复选组里');
      return;
    }
    void editor
      .updateControl(target.index, { glyphColor: [at(1), at(3), at(5), 255] })
      .then((ok) => chrome.say(ok ? '记号颜色已改' : '记号颜色被拒（切到编辑模式再试）'));
  });
  return () => input.click();
}

/**
 * 这一页的动作表：三个页签，全部落在公开面上。
 * @returns 页签表
 */
function makeTabs(): Tab[] {
  const pickColor = makeColorPicker();
  const pickControlColor = makeControlColorPicker();

  const doc: Group = {
    label: '文档',
    items: [
      { act: 'load', label: '载入示例', icon: 'doc', big: true, run: () => editor.setText(DEMO_TEXT) },
      { act: 'save', label: '保存', icon: 'save', big: true, run: () => saveNow() },
      {
        act: 'unsaved-prompt',
        label: '保存提醒',
        icon: 'unsaved',
        big: true,
        on: () => unsavedPrompt,
        run: () => {
          unsavedPrompt = !unsavedPrompt;
          if (window.__demoState) window.__demoState.unsavedPrompt = unsavedPrompt;
          chrome.refresh();
        },
      },
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
        // 选项按已注册清单现算：库那边注册了什么，这里就有什么。
        //   写死"下标 → 名字"的话，注册顺序一变，下拉框显示的名字就与实际字体错开。
        //   显示用 label（注册时给的中文名），没给时它等于字体自己的族名。
        //   挂工具条发生在 createEditor 之前，那一拍还没有实例，先给空表。
        options: () =>
          [{ value: '', text: '—' }].concat(
            (editor?.fonts() ?? []).map((f) => ({ value: String(f.index), text: f.label })),
          ),
        value: () => (mark.font === undefined ? '' : String(mark.font)),
        run: (v) => {
          if (!v) return;
          void writeMark({ font: Number(v) });
        },
      },
      {
        act: 'size',
        label: '字号',
        options: [{ value: '', text: '—' }].concat(SIZES.map((s) => ({ value: String(s), text: String(s) }))),
        value: () => (mark.size === undefined ? '' : String(mark.size)),
        run: (v) => {
          if (!v) return;
          void writeMark({ size: Number(v) });
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
          void writePara({ lineFactor: Number(v) });
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
          void writePara({ spaceAfter: Number(v) });
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
        run: () => {
          void editor.undo().then(() => syncFormat());
        },
      },
      {
        act: 'redo',
        label: '重做',
        icon: 'redo',
        big: true,
        run: () => {
          void editor.redo().then(() => syncFormat());
        },
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
        act: 'margin-marks',
        label: '裁剪标记',
        icon: 'margin-marks',
        on: () => editor.config().marginMarks,
        run: () => {
          // 与上面那几个下拉框同一条：配完重读一遍回显，否则按钮停在上一档。
          void editor
            .configure({ marginMarks: !editor.config().marginMarks })
            .then(() => chrome.refresh());
        },
      },
      {
        act: 'page-setup',
        label: '页面设置',
        icon: 'page-setup',
        // 纸张、方向、四边页边距：面板读一份当前的配置，按"应用"才下发（改一次要
        //   整篇重排，边输边排会让每一次击键都卡一下）。
        panel: (dismiss) =>
          createPageSetup({
            editor,
            // 配完重读回显：面板以外还有缩放读数要跟着变（见 configchange 那条）。
            applied: () => {
              void chrome.refresh();
            },
            dismiss,
          }),
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

  // 三种编辑模式是产品语义，不是调试开关：填表与预览都靠它。
  const mode: Group = {
    label: '模式',
    items: [
      {
        act: 'mode',
        label: '编辑模式',
        options: [
          { value: 'document', text: '编辑' },
          { value: 'form', text: '表单' },
          { value: 'readOnly', text: '只读' },
        ],
        value: () => editor.config().mode,
        run: (v) => {
          if (!v) return;
          // 配完重读回显：configure 不发 change，不刷新的话下拉框停在上一档。
          void editor
            .configure({ mode: v as 'document' | 'form' | 'readOnly' })
            .then(() => chrome.refresh());
        },
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
        label: '文本输入',
        icon: 'field',
        big: true,
        // 空内容 + 一句提示：提示是灰字、不进内容，填了字它就消失。
        run: () => insertControl('text', { placeholder: '（待填）' }),
      },
      {
        act: 'number',
        label: '数值输入',
        icon: 'number',
        big: true,
        // 数值控件只收数字：非数字在编辑入口就被挡住（见引擎的 ContentFilter）。
        run: () => insertControl('number', { placeholder: '0' }),
      },
      {
        act: 'radio',
        label: '单选',
        icon: 'choice',
        big: true,
        // 一次插一个，且不配互斥组：没有组名的单选是独立开关，点一下选、再点一下取消。
        //   要同组互斥时由宿主在插入选项里给 group（同组里点第二个会自动取消第一个）。
        run: () => insertControl('radioButton', { label: '选项甲', glyphColor: [220, 38, 38, 255] }),
      },
      {
        act: 'checkbox',
        label: '复选',
        icon: 'choice',
        big: true,
        // 记号色可配：这里给一个主题蓝，不配就跟随文字颜色。
        run: () => insertControl('checkBox', { label: '同意', glyphColor: [37, 99, 235, 255] }),
      },
      {
        act: 'radiogroup',
        label: '单选组',
        icon: 'choice',
        big: true,
        // 组控件的记号色同样是这一个选项：组里所有圆圈用同一种颜色。
        run: () => insertControl('radioGroup', { options: DEMO_OPTIONS, glyphColor: [22, 163, 74, 255] }),
      },
      {
        act: 'checkboxgroup',
        label: '复选组',
        icon: 'choice',
        big: true,
        run: () =>
          insertControl('checkBoxGroup', {
            options: DEMO_OPTIONS,
            glyphColor: [124, 58, 237, 255],
          }),
      },
      {
        act: 'dropdown',
        label: '下拉',
        icon: 'dropdown',
        big: true,
        run: () => insertControl('dropdownList', { options: DEMO_OPTIONS }),
      },
      {
        act: 'date',
        label: '时间选择',
        icon: 'date',
        big: true,
        // 默认格式 yyyy年MM月dd日 HH时mm分ss秒；六个单位全可选。
        run: () => insertControl('date', {}),
      },
      {
        act: 'control-color',
        label: '控件颜色',
        icon: 'color',
        big: true,
        // 改的是"光标所在那个控件"的外观，不是文档正文的字体色。
        run: pickControlColor,
      },
      {
        act: 'image',
        label: '图片',
        icon: 'image',
        big: true,
        // 解码交给浏览器（createImageBitmap 在原生代码里跑），引擎只收 RGBA ——
        //   文档里存的是资源 id，像素在资源表里。
        run: () => pickImage(),
      },
    ],
  };

  return [
    { label: '开始', groups: [doc, clipboard, font, paragraph, edit] },
    { label: '插入', groups: [insert] },
    { label: '视图', groups: [mode, show, zoom] },
  ];
}

/**
 * 演示用的选项表：值与标签分开，值进数据、标签进纸面。
 */
const DEMO_OPTIONS = [
  { value: 'a', label: '选项甲' },
  { value: 'b', label: '选项乙' },
  { value: 'c', label: '选项丙' },
];

/**
 * 插一个控件。每次给一个不同的业务编码 —— 同编码表示"同一个字段的多处副本"。
 * @param type 业务类型
 * @param opts 其余选项（标签、选项表、互斥组、格式）
 */
function insertControl(type: string, opts: Record<string, unknown>): void {
  const key = type + '-' + Math.random().toString(36).slice(2, 6);
  void editor.insertControl({ type, key, ...opts } as never).then((ok) => {
    chrome.say(ok ? '插入：' + type : '插入被拒：' + type);
  });
}

/**
 * 控件弹出层：下拉的选项列表与日期的时间选择器都归宿主画。
 *
 * 位置与关闭时机走 layer（与右键菜单、属性面板同一份实现）：这一层只管"面板里放什么"。
 * 引擎只画控件本身；这一层是编辑期辅助，不进文档、不进导出。
 * @param index 控件下标
 * @param rect 控件在页面坐标下的第一块矩形（磅）
 * @param readonly 业务只读：只提示一句，不弹
 */
function openControlPanel(
  index: number,
  rect: { page: number; x: number; y: number; w: number; h: number } | null,
  readonly: boolean,
): void {
  const c = editor.form()[index];
  const pages = document.querySelector('.heditor-pages');
  if (!c || !rect || !(pages instanceof HTMLElement)) return;
  if (readonly) {
    // 引擎对只读控件的写入已经拒了，弹出来也只能看；不如直接说清楚它为什么不动。
    chrome.say('这个字段是只读的');
    return;
  }

  const g = editor.geometry();
  const box = pages.getBoundingClientRect();
  // 磅 -> CSS 像素：缩放倍数乘 4/3（96 dpi / 72 磅），与画布摆放同一套换算。
  const scale = editor.config().zoom * (4 / 3);
  const top = box.top + (rect.page * (g.page.h + g.gap) + rect.y + rect.h) * scale;
  const left = box.left + rect.x * scale;

  const layer = createLayer('heditor-popover heditor-popover--control', () => editor.focus());
  layer.el.style.minWidth = Math.round(rect.w * scale) + 'px';

  if (c.type === 'dropdownList') {
    for (const opt of c.options) {
      const item = document.createElement('button');
      item.type = 'button';
      item.className = 'heditor-popover-item';
      item.textContent = opt.label;
      item.addEventListener('click', () => {
        void editor.setControlValue(index, { text: opt.value });
        layer.close();
      });
      layer.el.appendChild(item);
    }
  } else if (c.type === 'date') {
    // 输入控件的类型、步长、以及"该塞多长的串"都在字段表里（按 dateUnits 算好）：
    //   这一层只建元素，值怎么裁、怎么补回 ISO 都不归它管。
    const field = editor.describeControl(c).fields.find((f) => f.kind === 'datetime');
    if (!field) return;
    const input = document.createElement('input');
    input.type = field.inputType ?? 'date';
    input.value = String(field.value ?? '');
    if (field.step !== undefined) input.step = String(field.step);
    const apply = document.createElement('button');
    apply.type = 'button';
    apply.className = 'heditor-popover-item';
    apply.textContent = '确定';
    apply.addEventListener('click', () => {
      // 片段补齐成完整 ISO、与现状比差值都在 applyControlForm 里；没改动它返回 false。
      void editor.applyControlForm(index, { value: input.value }).then(() => layer.close());
    });
    layer.el.appendChild(input);
    layer.el.appendChild(apply);
  } else {
    return;
  }

  layer.openAtPoint(Math.round(left), Math.round(top + 2));
}

/**
 * 选一张本地图片装进编辑器：浏览器解码，像素交给引擎，文档里只留资源 id。
 *
 * 太宽的图按版心宽度等比缩一下 —— 一张 4000 像素宽的照片插进 A4 会横着跑出页面，
 *   而画面上看起来只是"图片不见了"。
 * @param file 用户挑的文件
 */
async function insertImageFile(file: File): Promise<void> {
  const bitmap = await createImageBitmap(file);
  const canvas = document.createElement('canvas');
  canvas.width = bitmap.width;
  canvas.height = bitmap.height;
  const ctx = canvas.getContext('2d');
  if (!ctx) {
    chrome.say('这台浏览器给不出 2D 画布，图片没法解码');
    return;
  }
  ctx.drawImage(bitmap, 0, 0);
  const px = ctx.getImageData(0, 0, canvas.width, canvas.height);
  const id = 'img-' + Date.now().toString(36);
  await editor.registerImage({
    id,
    width: canvas.width,
    height: canvas.height,
    data: new Uint8Array(px.data.buffer),
  });

  // 版心宽度（磅）= 页宽 - 左右页边距；100 DPI 的图按 96 折算，留一点余量。
  const g = editor.geometry();
  const page = editor.config().page;
  const textWidthPt = g.page.w * 0.75 - page.margins.left - page.margins.right;
  const naturalPt = (canvas.width * 72) / 96;
  const width = Math.min(naturalPt, textWidthPt > 0 ? textWidthPt : naturalPt);
  await editor.insertImage({
    resourceId: id,
    pxWidth: canvas.width,
    pxHeight: canvas.height,
    width,
  });
  chrome.say('图片已插入：拖它的把手可以改大小');
}

/**
 * 弹文件选择器。选完才解码，取消则什么都不做。
 */
function pickImage(): void {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = 'image/*';
  input.addEventListener('change', () => {
    const file = input.files && input.files[0];
    if (!file) return;
    void insertImageFile(file).catch((e) => chrome.say('插入图片失败：' + String(e)));
  });
  input.click();
}

/**
 * 重算页数与字数，并把状态行换成这一句。
 * @param tail 状态行要说的那句话
 */
function refreshCounts(tail: string): void {
  const g = editor.geometry();
  // 字数由引擎数（counts）：同一份文档换个宿主也是同一个数，页面不必搬全文回来扫。
  chrome.setCounts(g.pages, editor.counts().words);
  chrome.say(`${editor.paragraphs().length} 段 · ${tail}`);
}

async function boot(): Promise<void> {
  editor = await createEditor({
    container: stage,
    chrome: true,
    // 下拉的选项列表与日期的输入框归宿主画：引擎只画控件本身。
    onControlTap: (index, rect, readonly) => openControlPanel(index, rect, readonly),
    // 右键菜单也归宿主：库只负责拦掉浏览器那份、把插入点搬过去、把压着什么交回来。
    onContextMenu: (target, at) => {
      void menus?.open(target, at);
    },
    // 前置回调的接缝：判据把它换成"先拒后放"，演示页默认一律允许。
    onBeforeControlChange: (change) => {
      const decide = (window as unknown as { __demoBeforeChange?: (c: unknown) => unknown })
        .__demoBeforeChange;
      if (typeof decide !== 'function') return true;
      const r = decide(change);
      return typeof r === 'boolean' ? r : Promise.resolve(r as Promise<boolean>);
    },
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
  window.__demoState = { changes: 0, unsavedPrompt };
  menus = createMenus({
    editor,
    say: (text) => chrome.say(text),
    focusEditor: () => editor.focus(),
    insertControl,
    pickImage,
  });

  await editor.setText(DEMO_TEXT);
  dirty = false;
  chrome.bind(editor);

  const touch = (): void => {
    void syncFormat();
  };
  editor.on('caret', touch);
  editor.on('selection', touch);
  // 配置变了也要刷一遍回显：Ctrl+滚轮与 Ctrl+±/0 是库内直接改缩放的，不经过这个页面的
  //   动作表 —— 不接这一条，右下角的百分比与滑块会停在旧值上。
  editor.on('configchange', () => chrome.refresh());
  editor.on('change', () => {
    const n = ++window.__demoState!.changes;
    dirty = true;
    refreshCounts(`变更 ${n} 次`);
    // 内容变了，光标处能继承的格式也可能变了（撤销、粘贴、格式命令都走这条路）。
    void syncFormat();
  });

  await syncFormat();
  refreshCounts('就绪');

  // 探针的排空点：所有 fire-and-forget 的调用之后读这个。
  window.__demoReady = true;

  // 热更时先把编辑器收掉，避免同一块容器上叠两个实例。
  if (import.meta.hot) import.meta.hot.dispose(() => void editor.destroy());
}

// 工具条必须在实例之前挂好：动作闭包引用 editor 与 chrome，真正按下去时才读到它们。
const tabs = makeTabs();
chrome = mountChrome(document.getElementById('bar')!, 'HEditor', tabs);

void boot();

// 有未保存的改动、且用户打开了提醒，才拦一下"关页面 / 刷新"。
//
// 文案由浏览器给（自定义的那一句不显示），所以这里只做"要不要问"的判断：
//   默认不问 —— 提醒是可选行为；用户可以在工具条上把它打开。
window.addEventListener('beforeunload', (ev) => {
  if (!unsavedPrompt || !dirty) return;
  ev.preventDefault();
  // Chrome 只认 returnValue 非空，其余浏览器读 returnValue 的字符串。
  ev.returnValue = '文档还没保存';
});

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
