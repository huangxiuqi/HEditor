// 浏览器宿主：标记、页画布、输入接线、光标与 IME 的位置、滚动跟随。
// 这一份是两套传输（同线程 / Worker）共有的那一半。分成两份的写法会让"点了没反应"
//   这类问题要在两处各查一遍；差别只剩三处，全部收进 EngineLink：结果何时到
//   （同步值还是 Promise）、片元在哪贴（这里 / Worker）、状态从哪读（引擎 / 镜像）。
// 一条纪律：宿主不持有文档，也不做任何字符索引。偏移的单位在引擎里是码点，
//   而 JS 字符串是 UTF-16 码元，JS 侧只要碰过 offset 算术，错就只在中文/上标/emoji
//   上出现，而英文测试全过。
import { armDprWatch, createZoom, then1, type Args, type Maybe, type ZoomControl } from '../input/keymap';
import { createDragSelect, type DragSelect } from '../input/drag';
import { createInput, type InputLayer } from '../input/input';
import {
  FRAGMENT_MIME,
  parseClipPayload,
  fragmentInHtml,
  latestPayload,
  readSystemClipboard,
  recallPayload,
  writeToDataTransfer,
  writeToSystemClipboard,
} from './clipboard';
import type { ContextTarget } from '../types';
import { type CanvasStore, type ScrollGeometry } from './pages';
import {
  imeOf,
  pagesOf,
  PART_IME,
  PART_PAGES,
  PART_PREEDIT,
  PART_STAGE,
  stageOf,
} from './parts';

/**
 * 一次读取的几何：页度量 + 窗口 + 总页数（都是设备像素，除 dpr）。
 */
export interface HostView extends ScrollGeometry {
  totalPages: number;

  /**
   * 窗口首页。
   */
  first: number;

  /**
   * 窗口页数。
   */
  count: number;
}

/**
 * 传输层交出来的引擎入口。同步宿主返回同步值，Worker 宿主返回 Promise / 镜像。
 */
export interface EngineLink {
  /**
   * 读命令。Worker 侧读的是主线程镜像，所以是同步的。
   * @param name 读命令名
   * @param args 参数
   */
  read(name: string, args?: Args): any;

  /**
   * 写命令的引擎那一步（不含收尾）；返回"有没有改动"。
   * @param name 写命令名
   * @param args 参数
   */
  write(name: string, args?: Args): Maybe<boolean>;

  /**
   * 一次写之后的收尾：对窗口 → 必要时呈现 → 贴脏页。Worker 侧是空实现。
   * @param opName 刚跑完的命令名（决定要不要补呈现）
   */
  settle(opName: string): Maybe<any>;

  /**
   * 视口位置（设备像素）。窗口跟着可见页走，滚动本身不重排。
   * @param scrollY 滚动位置（设备像素）
   */
  view(scrollY: number): Maybe<any>;

  /**
   * 几何快照。
   */
  geom(): HostView;

  /**
   * 分页 / 连页与页间隙（成套设，见 engine api 的 setViewMode）。
   * @param gap 页间隙（磅，连页时是 0）
   * @param continuous 是否连页
   */
  setViewMode(gap: number, continuous: boolean): Maybe<any>;

  /**
   * 引擎把滚动位置归零了（装载类操作）—— 宿主跟着追平一次。
   */
  takeScrollReset(): boolean;

  /**
   * 整份序列化成 JSON 文本。同线程直接读，Worker 是一次往返。
   */
  serialize(): Maybe<string>;

  /**
   * 复制选区，回一份剪贴板载荷（{f, t} 或 {e} 的 JSON 文本）。
   *
   * 同线程是同步值（copy 事件里必须当场拿到，否则浏览器已经把剪贴板写完了）；
   * Worker 是一条往返，调用方只能等 —— 那时改用 ClipboardItem 写系统剪贴板。
   */
  copy(): Maybe<string>;

  /**
   * 等队列排空。
   *
   * 同线程没有队列，直接落地；Worker 要等所有在途的 do / ask 都收到回话 ——
   *   回话是 Worker 处理完那条消息之后才发的，所以"回话到了"等于"命令已经落地"。
   */
  flush(): Promise<void>;
}

export type ChangeSource = 'user' | 'api';

export interface DomHostOptions {
  container: HTMLElement;
  link: EngineLink;

  /**
   * 是否由库接管键盘 / 指针 / IME。默认 true；false 只给"自己驱动"的自动化宿主。
   */
  input?: boolean;

  /**
   * 只读时编辑命令由引擎拦（ReadOnly 档），宿主只用来提示状态。
   */
  readOnly(): boolean;

  /**
   * 选择类控件切换之前的裁决（宿主接的前置回调）。没接就一律允许。
   * @param change 切换请求
   */
  beforeControlChange?(change: any): boolean | Promise<boolean>;

  /**
   * 点到一个"不是选择类"的控件（下拉、日期）时通知宿主 —— 弹出层归宿主。
   * @param index 控件下标
   * @param rect 控件在页面坐标下的第一块矩形（磅）；读不到时为 null
   * @param readonly 业务只读：宿主据此决定弹不弹（写回去的路引擎已经封住）
   */
  onControlTap?(
    index: number,
    rect: { page: number; x: number; y: number; w: number; h: number } | null,
    readonly: boolean,
  ): void;

  /**
   * 右键落在文档上（库已经把插入点搬过去了）。菜单归宿主画。
   * @param target 落点归属
   * @param at 指针位置（客户端坐标，CSS 像素）
   */
  onContextMenu?(target: ContextTarget, at: { x: number; y: number }): void;

  /**
   * 状态栏（产品页没有这一格，实现里就什么都不做）。
   * @param text 这句话要说什么
   * @param isErr true = 这是一次失败（要上提示条）
   */
  status?(text: string, isErr?: boolean): void;

  /**
   * 一次编辑真的改了内容。
   * @param changed 内容有没有真的变
   * @param source 改动来自哪条路
   */
  onEdit?(changed: boolean, source: ChangeSource): void;

  /**
   * 光标 / 选区变了（每拍收尾时发一次，宿主侧按"值变了才发"去重）。
   */
  onCaret?(): void;

  /**
   * 选区变了（每拍收尾时发一次，宿主侧按值去重）。
   */
  onSelection?(): void;

  /**
   * 呈现完成：这一拍重画了几页、花了多少毫秒（宿主侧计时）。
   * @param pages 这一拍重画了几页
   * @param ms 贴图花了多少毫秒
   */
  onRender?(pages: number, ms: number): void;

  /**
   * 画布存储的造法（槽复用，见 pages.ts）。
   * @param pagesEl 页容器（画布要挂进去）
   */
  canvasStore(pagesEl: HTMLElement): CanvasStore;

  /**
   * 页面级异常（脚本错误、未处理的异步失败）。
   * @param message 异常原文
   */
  onPageError?(message: string): void;

  /**
   * 贴图的账从哪里读。像素由 Worker 贴，这条线程上一次 putImageData 都没跑过 ——
   *   自己记只会得到一串恒零，而"按脏矩形贴"的判据正是靠这个计数器。
   */
  blitStats(): { ms: number; pages: number; px: number };

  /**
   * 滚轮要缩放编辑器（Ctrl + 滚轮，或触控板捏合）。
   *
   * 不传就完全放行：浏览器会去缩放整个页面，而编辑器的字号、页度量都不动 ——
   *   那是宿主自己的选择，不是库能替它决定的。
   * @param delta 缩放倍数的增量（正 = 放大）
   */
  onZoomBy?(delta: number): void;

  /**
   * 缩放回到 100%（Ctrl + 0）。
   */
  onZoomReset?(): void;
}

export interface DomHost {
  readonly stage: HTMLElement;
  readonly pagesEl: HTMLElement;
  readonly ta: HTMLTextAreaElement;
  readonly zoom: ZoomControl;

  /**
   * 只把画布集合对齐到当前窗口（Worker 侧贴图，这一侧只管建与摆）。
   */
  syncPages(): void;

  /**
   * 内容整体作废（换缩放 / 换页度量 / 换文档）：所有页下次整页贴。
   */
  invalidatePages(): void;

  /**
   * 缩放前后停在同一个文档位置（按密度折算 scrollTop）。
   *
   * 换缩放时页的 CSS 尺寸整体乘一个倍率，而滚动位置记的是 CSS 像素 —— 不折算的话
   *   读者正在看的那一段会跑掉；引擎那边把滚动位置归零过一次的话，画面直接弹回开头。
   * @param before 缩放前的密度（设备像素 / 磅）
   * @param after 缩放后的密度
   */
  keepScrollAcrossZoom(before: number, after: number): Maybe<boolean>;

  /**
   * 只是一次重贴（配置类命令改的是光栅签名，不是内容）。
   */
  repaint(): Maybe<any>;

  /**
   * 收尾：对齐窗口、贴页、摆 IME、发事件。
   * @param st 这一拍的账；不传就现读
   */
  finish(st?: any): void;

  /**
   * 一次编辑的完整收尾（滚动 → 对窗口 → 上屏）。
   * @param opName 刚跑完的命令名
   * @param source 改动来自哪条路
   */
  afterEdit(opName: string, source: ChangeSource): Maybe<boolean>;

  /**
   * 复制选区到系统剪贴板（选区为空时返回 false，剪贴板内容不变）。
   * @returns 复制成了没有
   */
  copy(): Promise<boolean>;

  /**
   * 剪切：复制成功后删掉选区。
   * @returns 复制并删掉了没有
   */
  cut(): Promise<boolean>;

  /**
   * 粘贴：自家片段优先（保样式、控件、表格），其次纯文本。
   * @returns 粘上了没有
   */
  paste(): Promise<boolean>;

  /**
   * 把光标滚进视口（装载、程序化定位之后用）。
   * @param st 这一拍的账；不传就现读
   * @param reveal 是否真的改滚动位置（false = 只同步窗口与状态）
   */
  scrollCaretIntoView(st?: any, reveal?: boolean): Maybe<boolean>;

  /**
   * 把滚动位置归零（引擎换过文档时它自己已经把滚动位置清了，宿主跟着追平）。
   */
  resetScroll(): void;

  /**
   * 文档坐标（页内设备像素）换算。
   * @param clientX 客户端横坐标
   * @param clientY 客户端纵坐标
   */
  pointAt(clientX: number, clientY: number): { x: number; y: number };

  /**
   * 交给输入层的命中（已折成磅）。
   * @param xPt 文档横坐标（设备像素）
   * @param yPt 文档纵坐标（设备像素）
   */
  hitAt(xPt: number, yPt: number): Maybe<any>;

  /**
   * 输入层（自动化宿主可以 takeInput 之后自己驱动）。
   */
  readonly input: InputLayer;
  readonly drag: DragSelect | null;

  /**
   * 启动之后调一次：输入层与滚动从此开始工作。
   */
  start(): void;

  /**
   * 传输层在途状态（Worker 的往返期间同理）。
   * @param on 是否在途
   */
  setBusy(on: boolean): void;

  /**
   * 视觉焦点：IME 与键盘都落在承接层上。
   */
  focus(): void;

  /**
   * 上一次贴图的账（毫秒 / 页数 / 写入像素）：判据拿它证明"按脏矩形贴"。
   */
  blitStats(): { ms: number; pages: number; px: number };

  /**
   * 重启闪烁（回到可见相位）。
   */
  startBlink(): void;

  /**
   * 停表：光标常亮，且不重绘 —— 探针读"上一次全量"的账之前必须先停。
   */
  stopBlink(): void;

  /**
   * 卡片尺寸变了：重新报视口（引擎按它算窗口）。
   */
  resize(): Maybe<any>;

  /**
   * 拆掉输入层、定时器与标记（容器清空）。
   */
  destroy(): void;
}

/**
 * 造编辑器需要的 DOM 骨架。
 *
 * 三段是硬要求：滚动舞台（stage）、页容器（pages）、承接层（ime）。
 *   滚动发生在 stage 自己身上，外面那层 body 只负责撑满容器 —— 老页面的 stage 就是
 *   滚动容器，多套一层之后"容器有多高"这件事有了两个答案（外层是视口、内层是内容），
 *   而自动滚动、坐标换算、探针都要用到它。承接层必须是一个真的可聚焦 textarea：
 *   canvas 收不到键盘，也收不到合成事件。
 *
 * 三段按类名标识，不按 id：同页可以挂几个实例，而 id 只能有一个（见 host/parts.ts）。
 * @param container 挂载点（已经有舞台就直接返回）
 */
export function ensureMarkup(container: HTMLElement): void {
  if (stageOf(container)) return;
  container.innerHTML = [
    '<div class="heditor-body" style="position:relative;height:100%">',
    `  <div class="${PART_STAGE}" style="position:relative;height:100%;overflow:auto">`,
    // 页容器必须是定位祖先：画布按"页号 × 步距"绝对定位，定位基准只能是这里。
    //   少了这条，画布会相对舞台（它也是 relative）定位 —— 而舞台是滚动容器，
    //   画布于是固定在可视区不随滚动走；退化成 static 更糟（见 PageCanvases.canvasFor）。
    // 左右自动外边距 + 定宽把页摆到正中（宽度由 pages.sync 按页宽写）；上下留出边距，
    //   页与窗口边框之间才有呼吸空间。cursor:text 覆盖整个页面区域 —— 那里就是正文。
    // 只留上边距：下边距会在文档末尾多出一段可滚的空白，而"最后一页之后还有一页"
    //   会让窗口算出一个不存在的页号（判据与治疗都指向同一条）。左右交给 auto。
    // overflow: hidden 是"连页末尾不留整页空白"的必要条件：最后一条条带的画布仍是
    //   整页高（改小会把位图压扁），多出来的白纸只能由这一层裁掉；顺带把绝对定位的
    //   画布挡在滚动区之外 —— 它们会撑大滚动的 overflow 区。
    `    <div class="${PART_PAGES}"` +
      ` style="position:relative;margin:16px auto 0;overflow:hidden;cursor:text"></div>`,
    // 承接层要 pointer-events: none：它按"从光标到页右、一行高"摆放，正好压住正文
    //   那一行。opacity:0 只影响像素，不影响命中 —— 图片把手落在光标处时，按下会被
    //   这块透明区域接走，画布那侧收不到，表现为"光标那个位置拖不动"。
    //   键盘与合成事件靠焦点送达，与命中无关；焦点由按下的那一路显式给。
    `    <textarea class="${PART_IME}" rows="1" spellcheck="false"`,
    '      style="position:absolute;left:0;top:0;width:1px;height:1em;opacity:0;',
    '             border:0;padding:0;resize:none;overflow:hidden;pointer-events:none"></textarea>',
    // 预编辑层：输入法组合中的字没有别的地方显示（承接层是透明的）。
    `    <div class="${PART_PREEDIT}"` +
      ' style="position:absolute;left:0;top:0;display:none;white-space:pre;' +
      'pointer-events:none;color:#1a1a1a;background:#fff;' +
      'box-shadow:0 0 0 1px #d8e0ea;border-bottom:1px solid #1f6feb"></div>',
    '  </div>',
    '</div>',
  ].join('\n');
}

/**
 * 取一段骨架，缺了就报一句能看懂的话。
 *
 * 不用"先收集缺失清单再抛"：那样拿到的是 T | null，后面每一处都得加感叹号 ——
 *   而收窄结果进不了回调（这些元素在回调里也要用）。这里直接返回非空值。
 * @param el 找到的元素（可能为空）
 * @param part 这一段的名字（报错用）
 * @returns 同一个元素，类型上保证非空
 */
function needPart<T extends HTMLElement>(el: T | null, part: string): T {
  if (!el) throw new Error(`容器里缺少 ${part}，无法建立编辑器`);
  return el;
}

/**
 * 造宿主：标记、页画布、输入接线、滚动跟随。
 * @param o 容器、link 与各回调
 * @returns 宿主句柄（输入层、拖选、贴页与收尾都在上面）
 */
export function createDomHost(o: DomHostOptions): DomHost {
  const root = o.container;
  ensureMarkup(root);
  const stageEl = needPart(stageOf(root), PART_STAGE);
  const pagesEl = needPart(pagesOf(root), PART_PAGES);
  const ta = needPart(imeOf(root), PART_IME);
  // 上面那次检查是为了给出一句能看懂的话；这里的断言是给类型看的。
  const body = root.querySelector<HTMLElement>('.heditor-body')!;
  const preeditEl = root.querySelector<HTMLElement>('.' + PART_PREEDIT);

  const link = o.link;
  const scroller = stageEl;
  // 页容器必须是定位祖先（见 ensureMarkup）。调用方自己递进来的那份骨架不一定写了这条
  //   （容器可以带着现成标记进来），补一次 —— 幂等，只在真的是 static 时写。
  if (getComputedStyle(pagesEl).position === 'static') pagesEl.style.position = 'relative';
  const zoom = createZoom({ base: 4 / 3 });
  const pages: CanvasStore = o.canvasStore(pagesEl);

  const read = (name: string, args?: Args): any => link.read(name, args);
  const dpr = (): number => window.devicePixelRatio || 1;

  /**
   * 这个实例的身份。剪贴板缓存靠它判断"是不是我自己刚写出去的那一份" ——
   *   文本相等不等于同源（别处复制来同样的字，格式可能完全不同）。
   */
  const instanceId =
    typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function'
      ? crypto.randomUUID()
      : 'h' + Math.random().toString(36).slice(2) + Date.now().toString(36);

  /**
   * scrollTop 折算成"文档第 0 页顶边之上的偏移"（CSS 像素）。
   *
   * 页容器在滚动内容里带着外边距（骨架里的上下边距），scrollTop = 0 时页顶还没到视口顶。
   *   直接拿 scrollTop 当文档纵坐标会整体少算这一段：窗口算出来落后一截，边缘那一页
   *   于是判成"不在窗口里"，表现是滚到底之后最后可见的那一页没有画布。
   * @returns 文档坐标下的滚动偏移（可能为负，表示顶部边距还没滚完）
   */
  function docScrollTop(): number {
    return scroller.scrollTop - pagesEl.offsetTop;
  }

  let disposed = false;
  let ready = false;
  let busy = false;
  let caretOn = true;
  // 拖动搬运期间把闪的那颗光标收起来：它与落点竖条同时出现时，页面上就是"两个光标"。
  let caretHidden = false;
  let blinkTimer = 0;
  let lastCaretKey = '';
  let lastSelKey = '';
  let pendingOp = '';
  // 窗口自愈的在途标志与"补过哪一处滚动位置"（见 healWindow）。
  let healing = false;
  let healedFor = -1;

  /**
   * 这一拍的上屏账。像素在 Worker 侧贴，这条线程只转发对面推来的数。
   */
  const blitStats = (): { ms: number; pages: number; px: number } => o.blitStats();

  // 几何

  const view = (): HostView => link.geom();

  // IME 与滚动

  /**
   * 把承接键盘的 textarea 摆到光标处。
   *
   * 这不是 UI 细节，而是架构依赖：IME 的候选框位置由输入元素的屏幕位置决定。
   *   光标不在窗口里时把输入框收成 1 px —— 这一帧光标本来就没画，拿窗口外的 caretY
   *   去摆会把输入框（以及它带出来的候选框）摆到屏幕某个莫名其妙的角落。
   */
  function positionIme(st: any): void {
    const inWindow =
      st && st.caretPage >= st.windowFirstPage && st.caretPage < st.windowFirstPage + st.windowPages;
    if (!st || !st.caretW || !inWindow) {
      ta.style.width = '1px';
      ta.style.height = '1px';
      return;
    }
    const g = view();
    const sr = stageEl.getBoundingClientRect();
    const pr = pagesEl.getBoundingClientRect();
    const step = g.ph + g.gap;
    const left = pr.left - sr.left + scroller.scrollLeft + st.caretX / dpr();
    // 页内 y + 页号 x 步距 = 文档坐标（与光标滚入视野同一口径）。
    const top =
      pr.top - sr.top + scroller.scrollTop + (st.caretPage * step + st.caretY) / dpr();
    const lineH = Math.max(12, Math.round(st.caretH / dpr()));
    ta.style.left = Math.round(left) + 'px';
    ta.style.top = Math.round(top) + 'px';
    ta.style.width = Math.max(24, Math.round(pagesEl.clientWidth - st.caretX / dpr())) + 'px';
    ta.style.height = lineH + 'px';
    ta.style.lineHeight = lineH + 'px';
    ta.style.fontSize = Math.max(10, Math.round(lineH * 0.62)) + 'px';
  }

  /**
   * 把滚动位置归零，并让滚动监听不再把这一下当成用户滚动（否则会白发一条视口消息，
   *   引擎与用户来回抢同一个 scrollTop）。
   */
  function applyScrollReset(): void {
    scroller.scrollTop = 0;
    lastScrollTop = 0;
  }

  /**
   * 把光标滚进视口。只在真的看不见时才动 scrollTop —— 每次编辑都居中会把用户正在
   *   看的地方拽走，那比"光标偶尔跑到屏幕外"更难受。
   *
   * 顺序有两条硬约束（连续粘贴把文档撑长时全靠它们）：
   *   一、先让滚动高度跟上这一拍的总页数。Worker 传输下 pages.sync 只在 finish 里跑，
   *     而滚动发生在它之前 —— 容器还按上一拍的页数算高度，赋值 scrollTop 会被浏览器
   *     夹在旧底边上。夹过之后值根本没变、不会再派发 scroll 事件，窗口于是永久停在
   *     旧位置：症状是"pages 很高、末尾一段没有画布"（TRAPS 186）。sync 是幂等的。
   *   二、改完当场把位置交给引擎，不等 scroll 事件 —— 理由同上：被夹住时值没变，
   *     浏览器不会派发事件，而"最后一下粘贴"之后也不会再有下一拍来补。
   * @param st 这一拍的状态读数（省略则现读）
   * @returns 有没有产生视口变化
   */
  function scrollCaretIntoView(st?: any, reveal = true): Maybe<boolean> {
    const s = st ?? link.read('editStats');
    lastState = s;

    if (!s || s.windowPages === undefined) return false;
    const g = view();
    pages.sync(g.first, g.count, g.totalPages, g);
    // 光标没动就不滚。视口是用户自己滚到的地方，而"搬一张图""改一次格式""提交一个
    //   表单"都不挪光标 —— 按旧光标回拽一下，看起来就是"松手之后页面自己跳了"。
    //   窗口与状态上面已经同步过，这里只省掉改滚动那一步。
    if (!reveal) return false;
    const step = g.ph + g.gap;
    const inWindow =
      s.caretPage >= s.windowFirstPage && s.caretPage < s.windowFirstPage + s.windowPages;
    const top = inWindow
      ? (s.caretPage * step + s.caretY) / dpr()
      : (s.caretPage * step) / dpr();
    const bot = top + Math.max(12, s.caretH / dpr());
    const pad = 18;
    const before = scroller.scrollTop;
    const viewTop = before;
    const viewBot = viewTop + scroller.clientHeight - 2 * pad;
    if (top < viewTop + pad) scroller.scrollTop = Math.max(0, top - pad);
    else if (bot > viewBot) scroller.scrollTop = bot - scroller.clientHeight + pad * 2;
    return nudgeViewport(before);
  }

  /**
   * 程序化改过 scrollTop 之后，当场把新位置告诉引擎。
   *
   * 与 onScroll 那条路互为补充，不是重复：onScroll 靠浏览器派发事件，而"值没变"
   *   （被旧高度夹住）或"上一拍 rAF 还没跑"时它什么都不会发。这里先把 lastScrollTop
   *   记成新值，onScroll 之后读到同一个值就不会再发第二遍。
   * @param before 改动之前的 scrollTop
   * @returns 发出了视口变化没有
   */
  function nudgeViewport(before: number): Maybe<boolean> {
    const top = scroller.scrollTop;
    if (top === before) return false;
    lastScrollTop = top;
    return then1(link.view(docScrollTop() * dpr()), () => true);
  }

  // 收尾

  /**
   * 自愈：可见页不在窗口里，就补一条视口消息。
   *
   * 为什么必须有这一层：窗口换不换是引擎按"它这一刻的总页数"算的，而总页数、滚动位置、
   *   容器高度分属三拍 —— 连续粘贴把文档一次撑长几页时，换窗请求可能带着上一拍的总页数
   *   出发，算出来的窗口被夹在旧范围里；而 scrollTop 之后没再变，onScroll 也就不会发
   *   第二条。结果正是"容器一直涨、末尾那一段永远没有画布"（TRAPS 186）。
   *   同一个滚动位置只补一次，避免"引擎夹住 → 再补 → 再夹"的循环；真正滚动之后
   *   onScroll 会把 healedFor 放开。
   * @param g 这一拍的几何（窗口、页步距与屏幕像素比）
   */
  function healWindow(g: HostView): void {
    if (healing || disposed || g.count <= 0) return;
    const step = g.ph + g.gap;
    if (step <= 0) return;
    const top = Math.round(docScrollTop());
    if (top === healedFor) return;
    const d = dpr();
    const visFirst = Math.floor((top * d) / step);
    const visLast = Math.floor((top * d + scroller.clientHeight * d - 1) / step);
    if (g.first <= visFirst && visLast <= g.first + g.count - 1) return;
    healedFor = top;
    healing = true;
    fireAndForget(
      then1(link.view(top * d), () => {
        healing = false;
        finish();
      }),
    );
  }

  /**
   * 收尾：对齐窗口、贴页、摆 IME、发事件。同一次状态可以重复来（Worker 会多拍）。
   */
  function finish(st?: any): void {
    if (disposed) return;
    // 引擎自己归零过滚动位置（换文档、诊断装载）：这一拍把页面追平。
    //   必须在这里消费，不能只在 afterEdit 里 —— 装载与诊断装载走的是 settle + finish，
    //   标记于是挂到下一次编辑才生效：多页文档上点一下，视口被那次"迟到的归零"拽回
    //   第 0 页（症状是"点第二页，页面跳回顶上、光标不见了"）。
    if (link.takeScrollReset()) applyScrollReset();
    const s = st ?? link.read('editStats');
    const g = view();
    // 画布集合跟着窗口走。这是唯一的建画布时机 —— 建好一张就 transfer 一张。
    pages.sync(g.first, g.count, g.totalPages, g);
    healWindow(g);
    positionIme(s);
    // 变化键取完整的落点身份：段号与偏移在相邻控件边界上会重合（][ 两侧同段同偏移），
    //   少了侧别，光标在两枚括号之间挪一格就判成"没动"，输入法候选框不跟着挪；
    //   少了行别，折行处上下换行也判成没动。这条键只判"变没变"，不参与定位。
    const caretKey = s
      ? `${s.caretPara}:${s.caretOffset}:${s.caretSide}:${s.caretLineAff}:${s.caretPage}:${s.caretVisible}`
      : '';
    if (caretKey !== lastCaretKey) {
      lastCaretKey = caretKey;
      o.onCaret?.();
    }
    // 选区两端是流位置：只挪一个槽、另一侧不变时它也得算变。
    const selKey = s ? `${s.selectionAnchor}:${s.selectionFocus}` : '';
    if (selKey !== lastSelKey) {
      lastSelKey = selKey;
      o.onSelection?.();
    }
    o.onRender?.(blitStats().pages, blitStats().ms);
  }

  /**
   * 一次编辑的完整收尾。
   *
   * 顺序要紧，两步都不能换：
   *   先滚再对窗口 —— 反过来的话窗口按"旧的滚动位置"算，光标在第 0 页而屏幕还停在
   *     第 15 页时会把窗口拉成整篇。
   *   先对窗口再上屏 —— 反过来的话第一次上屏会把帧贴到错的页上。
   */
  // 事件处理器里发起的异步链：没人 await 它，拒绝必须在这里收掉。典型场景是销毁那一拍
  //   还在途的视口消息 —— 它注定被拒，而浏览器只会把它记成一条 uncaught（用户看不见，
  //   探针的"无 console 错误"却会红）。非销毁态的失败照样报到错误面，那是用户该看见的。
  function fireAndForget(p: Maybe<any>): void {
    if (!p || typeof (p as Promise<any>).then !== 'function') return;
    (p as Promise<any>).catch((e) => {
      if (disposed) return;
      o.onPageError?.('异步收尾失败：' + String((e as Error)?.message ?? e));
    });
  }

  function afterEdit(opName: string, source: ChangeSource): Maybe<boolean> {
    if (disposed) return false;
    // 编辑之后光标必须当场可见：相位要在 settle 之前写 —— settle 里的 present 才会把
    //   它画出来（present 之后再写只能等下一拍）。
    caretOn = true;
    writeOp('phase', { on: true });
    return then1(link.settle(opName), (r: any) => {
      // settle 回的是 { moved, scrolled, stats }（两种传输同形，见 EngineLink.settle）。
      //   stats 里才是这一拍的光标读数 —— 少了这层解包，IME 与"滚进视野"读到的都是
      //   undefined：输入法框塌成一像素，而"滚进视野"按上一次的位置算。
      const s = r?.stats ?? link.read('editStats');
      // 滚动必须在 settle 之后：settle 之前读到的 editStats 还是上一拍的（它只在
      //   present 时刷新）。拿旧读数决定滚动方向，多页文档上点第二页会把视口拽回
      //   上一次编辑所在的那一页 —— 看起来就是"点一下页面乱跳、光标不见了"。
      // 光标没动就别滚（见 scrollCaretIntoView 的 reveal）：搬图、改格式、提交表单
      //   都不挪光标，而这时视口停在用户自己滚到的位置。
      const prev = lastState;
      const caretMoved =
          !prev || !s || prev.caretPara !== s.caretPara || prev.caretOffset !== s.caretOffset ||
          prev.caretPage !== s.caretPage;
      // 滚动与收尾之间还要等一次视口消息：滚动之后引擎可能得换窗，而 finish 是按
      //   "引擎这一刻的窗口"建画布、贴页的。不等的话这一拍贴的是旧窗口那几页，
      //   新窗口要等下一拍 —— 连续粘贴时就是每一拍都落后一点，越粘差距越大。
      return then1(scrollCaretIntoView(s, caretMoved), () => {
        finish();
        // 计时器在这里重新起：相位从"可见"开始，530 ms 后翻到隐藏。放在 finish 里会
        //   被每一份状态重置（Worker 每拍都推一份），于是永远停在可见那一拍。
        resetBlink();
        o.onEdit?.(true, source);
        return true;
      });
    });
  }

  // 剪贴板

  /**
   * 跑一条可能抛的写命令：失败转成状态栏提示，不把异常留给事件处理器。
   *
   * paste 那条命令在"片段读不下去"时抛出（那是用户输入坏了，不是 bug），
   *   同步传输会当场抛，Worker 传输会拒绝 —— 两条路都得收成同一句提示。
   * @param name 命令名
   * @param args 参数
   * @returns 有没有改动
   */
  function tryWrite(name: string, args?: Args): Maybe<boolean> {
    const failed = (e: unknown): false => {
      o.status?.(`${name} 失败：${String((e as Error)?.message || e)}`, true);
      return false;
    };
    try {
      const r = writeOp(name, args);
      return r && typeof (r as Promise<boolean>).then === 'function'
        ? (r as Promise<boolean>).catch(failed)
        : r;
    } catch (e) {
      return failed(e);
    }
  }

  /**
   * 把引擎给的载荷写进系统剪贴板。
   *
   * 两条路走的是同一个载荷，差别只在"写进哪里"：事件里的 DataTransfer 只能同步写，
   *   所以同线程传输（copy 查询当场返回值）优先走它；Worker 传输要等一趟往返，
   *   那时事件已经结束，只能写 navigator.clipboard。
   * @param dt copy / cut 事件里的 clipboardData（程序化调用时为 null）
   * @returns 写成了没有
   */
  function copyToClipboard(dt: DataTransfer | null): Maybe<boolean> {
    const raw = link.copy();
    // 载荷是同步回来的（同线程传输）才能写进事件的 DataTransfer —— 那个对象只在事件
    //   处理器的这一次调用里可写，异步回来之后再 setData 不会报错，但剪贴板是空的。
    const sync = !(raw && typeof (raw as Promise<string>).then === 'function');
    return then1(raw, (v: string) => {
      const parsed = parseClipPayload(v, instanceId);
      if (!parsed.payload) {
        o.status?.(parsed.error || '没有选中内容', true);
        return false;
      }
      const p = parsed.payload;
      const write = dt && sync ? writeToDataTransfer(dt, p) : writeToSystemClipboard(p);
      return then1(write as Maybe<boolean>, (ok: boolean) => {
        if (!ok) o.status?.('写不进系统剪贴板', true);
        return ok;
      });
    });
  }

  /**
   * 剪切：复制成功之后才删 —— 反过来会让"复制失败但内容已经没了"变成一次数据丢失。
   * @param dt cut 事件里的 clipboardData
   * @returns 复制并删掉了没有
   */
  function cutToClipboard(dt: DataTransfer | null): Maybe<boolean> {
    return then1(copyToClipboard(dt), (ok: boolean) => {
      if (!ok) return false;
      return then1(tryWrite('deleteSelection'), (changed: boolean) =>
        changed ? afterEdit('deleteSelection', 'user') : false,
      );
    });
  }

  /**
   * 粘贴一份自家格式的片段。
   * @param fragment 片段 JSON 文本
   * @param source 改动来自哪条路
   * @returns 粘上了没有
   */
  function pasteFragmentText(fragment: string, source: ChangeSource): Maybe<boolean> {
    return then1(tryWrite('paste', { s: fragment }), (done: boolean) =>
      done ? afterEdit('paste', source) : false,
    );
  }

  /**
   * 粘贴纯文本（别的应用来的文字，或自家格式丢失时的退路）。
   * @param text 纯文本
   * @param source 改动来自哪条路
   * @returns 粘上了没有
   */
  function pastePlainText(text: string, source: ChangeSource): Maybe<boolean> {
    return then1(tryWrite('pasteText', { s: text }), (done: boolean) =>
      done ? afterEdit('pasteText', source) : false,
    );
  }

  // 输入

  let drag: DragSelect | null = null;

  /**
   * 文档坐标（设备像素）：基准取页容器 —— 它的顶边就是文档第 0 页的顶边。
   * @param clientX 客户端横坐标
   * @param clientY 客户端纵坐标
   * @returns 文档坐标（设备像素）
   */
  function pointAt(clientX: number, clientY: number): { x: number; y: number } {
    const r = pagesEl.getBoundingClientRect();
    const d = dpr();
    return { x: (clientX - r.left) * d, y: (clientY - r.top) * d };
  }

  /**
   * 设备像素 -> 引擎的文档坐标（磅）。
   *
   * 只在这里折算一次。同一条规则写两遍就会分叉：输入层与拖选各自除一次密度，
   *   漏掉的那一处差 1.33 倍（96 dpi 的 density 就带 4/3），症状是"单击落得准、
   *   拖选落到下一行"，而且只在非 1.0 密度下显形。
   */
  const docPt = (xPx: number, yPx: number): { x: number; y: number } => ({
    x: xPx / zoom.density,
    y: yPx / zoom.density,
  });

  /**
   * 命中：收设备像素（与 drag 的 pointAt 同一口径），内部折算成文档坐标。
   */
  const hitAt = (xPx: number, yPx: number, clamp?: boolean): Maybe<any> => {
    const p = docPt(xPx, yPx);
    return read('hit', clamp ? { x: p.x, y: p.y, clamp: 1 } : { x: p.x, y: p.y });
  };

  // 拖选的移动端：搬焦点端，不搬 anchor。走"不滚视口"那一档：拖动过程中每拍都把
  //   光标滚进视口，会与边缘自动滚动抢同一个 scrollTop，看起来是画面在抖。
  // 位置直接搬命中给的 position（流位置 + 行别）：这不是"从命中里挑两个数"，而是
  //   把引擎算出来的身份原样送回去 —— 任何时候都不该由宿主把地址拼成位置。
  function dragExtendTo(hit: any): void {
    const pos = hit && hit.position;
    if (!pos || typeof pos.flow !== 'number' || pos.flow < 0) return;
    writeOp('extendTo', { flow: pos.flow, affinity: pos.affinity | 0 });
    caretOn = true;
    writeOp('phase', { on: true });
    then1(link.settle('extendTo'), () => finish());
  }

  function writeOp(name: string, args?: Args): Maybe<boolean> {
    pendingOp = name;
    return link.write(name, args);
  }

  // 最近一次状态：预编辑层要按光标位置摆（组合事件不带坐标）。
  let lastState: any = null;

  /**
   * 输入法组合串的显示。
   *
   * 承接层是透明的 textarea，浏览器画在它里面的组合串用户看不见 —— 中文输入法在选字
   *   阶段屏幕上就什么都没有。这一层把组合串画在光标处，与字处理软件的做法一致；
   *   组合结束就收起（正文由引擎按最终文本一次落地）。
   * @param text 组合中的串；空串表示收起来
   */
  function showPreedit(text: string): void {
    if (!preeditEl) return;
    if (!text) {
      if (preeditEl.style.display !== 'none') preeditEl.style.display = 'none';
      return;
    }
    const st = lastState;
    const inWindow =
      st && st.caretPage >= st.windowFirstPage && st.caretPage < st.windowFirstPage + st.windowPages;
    if (!st || !inWindow) return;
    const g = view();
    const sr = stageEl.getBoundingClientRect();
    const pr = pagesEl.getBoundingClientRect();
    const step = g.ph + g.gap;
    const left = pr.left - sr.left + scroller.scrollLeft + st.caretX / dpr();
    const top = pr.top - sr.top + scroller.scrollTop + (st.caretPage * step + st.caretY) / dpr();
    const lineH = Math.max(12, Math.round(st.caretH / dpr()));
    preeditEl.textContent = text;
    preeditEl.style.left = Math.round(left) + 'px';
    preeditEl.style.top = Math.round(top) + 'px';
    preeditEl.style.height = lineH + 'px';
    preeditEl.style.lineHeight = lineH + 'px';
    preeditEl.style.fontSize = Math.max(10, Math.round(lineH * 0.62)) + 'px';
    if (preeditEl.style.display !== 'block') preeditEl.style.display = 'block';
  }

  // 表格边界拖拽：列宽与行高。
  //
  // 判定与落地都在这一侧：引擎给几何（tableGrid），宿主把指针增量换成磅、画一条指示线、
  //   松手才提交一次 —— 拖动过程中每帧提交会重排重画整张表，撤销栈也会留下几十步。

  let guideEl: HTMLElement | null = null;
  let dropEl: HTMLElement | null = null;
  let dropRaf = 0;
  let hoverRaf = 0;

  /**
   * 指示线元素（懒建）。绝对定位在页容器上，不接指针事件。
   * @returns 那一个 div
   */
  function gridGuide(): HTMLElement {
    if (guideEl && guideEl.isConnected) return guideEl;
    const el = document.createElement('div');
    el.style.position = 'absolute';
    el.style.background = 'rgba(43, 127, 255, 0.9)';
    el.style.pointerEvents = 'none';
    el.style.display = 'none';
    el.style.zIndex = '3';
    pagesEl.appendChild(el);
    guideEl = el;
    return el;
  }

  /**
   * 拖动搬运的落点竖条。与格线指示线同一层（页容器里的绝对定位 div）。
   * @returns 那个元素（首次调用时创建）
   */
  function dropBar(): HTMLElement {
    if (dropEl && dropEl.isConnected) return dropEl;
    const el = document.createElement('div');
    el.style.position = 'absolute';
    // 与正文光标同一个样子：拖动期间闪的那颗光标是隐起来的，这一条就是"松手插在哪"，
    //   两者形状不同的话会读成"页面上有两个光标"。
    el.style.background = '#1a1a1a';
    el.style.pointerEvents = 'none';
    el.style.display = 'none';
    el.style.zIndex = '4';
    pagesEl.appendChild(el);
    dropEl = el;
    return el;
  }

  /**
   * 磅 -> 页容器里的 CSS 像素。x 是页内磅，y 是文档磅（含页偏移）。
   * @param ptX 页内横坐标（磅）
   * @param ptYDoc 文档纵坐标（磅）
   * @returns CSS 像素坐标
   */
  function cssOf(ptX: number, ptYDoc: number): { x: number; y: number } {
    const d = dpr();
    return { x: (ptX * zoom.density) / d, y: (ptYDoc * zoom.density) / d };
  }

  /**
   * 表格格线拖拽：压在内部格线上就接管整条手势，返回真表示"这一下不落光标"。
   *
   * 收设备像素（与输入层其他入口同一口径），内部折成磅。哪一条线来自命中（hit 的
   *   gridTable / gridCol / gridRow），栅格几何则等真的动起来才取：那是一次往返，
   *   压在按下这一拍上会把起手推到第一次移动之后（拖选与图片把手都因此断过）。
   *
   * 格线上点一下不算拖：抬手时一次都没越界，就把这一下还给落点 —— 否则内部格线
   *   会变成一小片"按下去什么也不发生"的死区。
   * @param ptPx 文档坐标（设备像素）
   * @param h 这一下的命中（带格线三格）
   * @returns 接管了没有
   */
  function tableResize(ptPx: { x: number; y: number }, h: any): Maybe<boolean> {
    const col = typeof h.gridCol === 'number' ? h.gridCol : 0;
    const row = typeof h.gridRow === 'number' ? h.gridRow : -1;
    if (col <= 0 && row < 0) return false;
    const table = Number(h.gridTable);
    const vertical = col > 0;
    const start = docPt(ptPx.x, ptPx.y);
    const guide = gridGuide();
    // 起手门槛（磅）：格线上点一下是"落点"，不是"改表"。
    const slop = 2;
    let last = start;
    let armed = false;
    let fetching = false;
    let outWidths: number[] | null = null;
    let outHeight = 0;
    // 栅格几何：首次越界之后才取回来。
    let cols: number[] = [];
    let widths0: number[] = [];
    let tableLeft = 0;
    let tableRight = 0;
    let pageTopPt = 0;
    let bandTop = 0;
    let bandBottom = 0;
    let rowEdge = 0;
    let h0 = 0;

    /**
     * 画指示线：竖线给页内 x 与文档 y 区间，横线给页内 x 区间与文档 y。
     * @param axis 竖线还是横线
     * @param pos 线的位置（磅）
     * @param lo 区间起点（磅）
     * @param hi 区间终点（磅）
     */
    const showGuide = (axis: 'col' | 'row', pos: number, lo: number, hi: number): void => {
      if (axis === 'col') {
        const a = cssOf(pos, lo);
        const b = cssOf(pos, hi);
        guide.style.left = Math.round(a.x) + 'px';
        guide.style.top = Math.round(a.y) + 'px';
        guide.style.width = '1px';
        guide.style.height = Math.max(1, Math.round(b.y - a.y)) + 'px';
      } else {
        const a = cssOf(lo, pos);
        guide.style.left = Math.round(a.x) + 'px';
        guide.style.top = Math.round(a.y) + 'px';
        guide.style.width = Math.max(1, Math.round(((hi - lo) * zoom.density) / dpr())) + 'px';
        guide.style.height = '1px';
      }
      guide.style.display = 'block';
    };

    const detach = (): void => {
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
      guide.style.display = 'none';
    };

    /**
     * 按最新的指针位置算新尺寸并画线。
     * @param q 指针位置（文档磅）
     */
    const apply = (q: { x: number; y: number }): void => {
      if (vertical) {
        const dx = q.x - start.x;
        const i = col;
        if (i >= widths0.length) {
          // 表的右边框：表宽跟着走，只有最后一列变 —— 拖它是"把表加宽"，
          //   按相邻两列分摊就把表钉死在原宽上了（Word 同一条）。
          const next = widths0.slice();
          next[i - 1] = Math.max(12, widths0[i - 1] + dx);
          outWidths = next;
          showGuide('col', tableLeft + cols[i] + dx, bandTop, bandBottom);
          return;
        }
        const total = widths0[i - 1] + widths0[i];
        // 相邻两列的总宽不变：拖一条边界不该把整张表撑宽或缩窄（Word 同一条）。
        const left = Math.max(12, Math.min(total - 12, widths0[i - 1] + dx));
        const next = widths0.slice();
        next[i - 1] = left;
        next[i] = total - left;
        outWidths = next;
        showGuide('col', tableLeft + cols[i] + dx, bandTop, bandBottom);
      } else {
        const dy = q.y - start.y;
        outHeight = Math.max(8, h0 + dy);
        showGuide('row', pageTopPt + rowEdge + dy, tableLeft, tableRight);
      }
    };

    /**
     * 取一次栅格几何（列边界、宽度、这一页的行片段与上下范围）。
     */
    const fetchGeom = (): void => {
      fetching = true;
      void then1(read('tableGrid', { x: start.x, y: start.y }), (g: any) => {
        fetching = false;
        if (!g || g.ok !== true || !Array.isArray(g.cols) || g.cols.length < 2) {
          detach();
          return;
        }
        const gv = view();
        // 页在文档里按"页高 + 页间隙"的像素步距摆，折回磅要除密度 —— 与命中测试同一个口径。
        const stepPt = (gv.ph + gv.gap) / zoom.density;
        pageTopPt = (g.page | 0) * stepPt;
        cols = (g.cols as any[]).map((v) => Number(v));
        widths0 = [];
        for (let i = 0; i + 1 < cols.length; i++) widths0.push(cols[i + 1] - cols[i]);
        tableLeft = Number(g.x);
        tableRight = tableLeft + Number(g.w);
        const rows: any[] = Array.isArray(g.rows) ? g.rows : [];
        bandTop = pageTopPt + (rows.length ? Number(rows[0].y) : 0);
        const tail = rows.length ? rows[rows.length - 1] : null;
        bandBottom = tail ? pageTopPt + Number(tail.y) + Number(tail.h) : bandTop;
        const at = rows.find((r) => Number(r.row) === row);
        rowEdge = at ? Number(at.y) + Number(at.h) : bandBottom - pageTopPt;
        h0 = at ? Number(at.h) : 0;
        armed = true;
        apply(last);
      });
    };

    const onMove = (e: MouseEvent): void => {
      if (typeof e.buttons === 'number' && (e.buttons & 1) === 0) {
        // 在窗口外松手时 mouseup 收不到：移动事件自带按键状态，靠它自愈。
        onUp();
        return;
      }
      const qp = pointAt(e.clientX, e.clientY);
      last = docPt(qp.x, qp.y);
      if (armed) {
        apply(last);
        return;
      }
      if (fetching) return;
      const dx = last.x - start.x;
      const dy = last.y - start.y;
      if (dx * dx + dy * dy < slop * slop) return;
      fetchGeom();
    };
    const onUp = (): void => {
      detach();
      if (!armed) {
        // 没动过：这一下是落点（格线上点一下不该改表）。
        void input.placeAt(ptPx);
        return;
      }
      if (vertical && outWidths) {
        const w = outWidths;
        void then1(writeOp('setTableColWidths', { handle: table, widths: w }),
          () => afterEdit('setTableColWidths', 'user'));
      } else if (!vertical && outHeight > 0) {
        const hh = outHeight;
        void then1(writeOp('setTableRowHeight', { handle: table, row, h: hh }),
          () => afterEdit('setTableRowHeight', 'user'));
      }
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
    return true;
  }

  const input = createInput({
    isReady: () => ready && !disposed,    isBusy: () => busy,
    ta,
    hot: () => ({
      vertical: !!read('caretVerticalNow'),
      blockReversed: !!read('caretBlockReversedNow'),
    }),
    write: (name, args) => writeOp(name, args),
    hit: (xPx, yPx) => hitAt(xPx, yPx),
    docPt: (pt) => docPt(pt.x, pt.y),
    pointOf: (ev) => pointAt(ev.clientX, ev.clientY),
    setCursor: (cursor) => {
      pagesEl.style.cursor = cursor || 'text';
    },
    /**
     * 把正文光标连相位一起收/放（拖动搬运时用）。
     *
     * 只改相位不呈现是看不见的：光标画在页位图之上，得重画一次那一小块它才消失。
     * 拖动一趟只调两次（起手收起、松手放回），这点代价换来"页面上只有一个光标"。
     * @param on true = 显示（恢复闪烁），false = 收起
     */
    setCaretPhase: (on: boolean) => {
      caretHidden = !on;
      caretOn = on;
      writeOp('phase', { on });
      if (on) resetBlink();
      return repaint();
    },
    /**
     * 拖动搬运的落点指示：一条竖线，说明"松手会插在这儿"。
     *
     * 拖动过程中指针可能停在任意位置（包括页边距与页间空白），判定归引擎（它算得
     *   出插入位置），这里只负责画。按帧节流：每次移动都问一次会让拖动发滞。
     * @param pt 指针位置（文档设备像素）；null 表示收起
     */
    dropMark: (pt: { x: number; y: number } | null) => {
      const el = dropBar();
      if (pt === null) {
        if (dropRaf) {
          cancelAnimationFrame(dropRaf);
          dropRaf = 0;
        }
        el.style.display = 'none';
        return null;
      }
      if (dropRaf) return null;
      dropRaf = requestAnimationFrame(() => {
        dropRaf = 0;
        const p = docPt(pt.x, pt.y);
        void then1(read('dropSpot', { x: p.x, y: p.y }), (r: any) => {
          if (!r || typeof r.page !== 'number') {
            el.style.display = 'none';
            return;
          }
          const g = view();
          const step = g.ph + g.gap;
          // 宽度取光标自己的（磅 x 密度 = 设备像素，与 caretRectPx 同一个式子）：
          //   写死一个 2px 比正文光标粗一档，看起来像另一个东西。
          const wPt = typeof r.w === 'number' && r.w > 0 ? r.w : 1.5;
          el.style.width = Math.max(1, Math.ceil(wPt * zoom.density) / dpr()) + 'px';
          el.style.left = Math.round(cssOf(r.x, 0).x) + 'px';
          el.style.top = Math.round((r.page * step + r.y * zoom.density) / dpr()) + 'px';
          el.style.height = Math.max(8, Math.round(r.h * zoom.density / dpr())) + 'px';
          el.style.display = 'block';
        });
      });
      return null;
    },
    capturePointer: () => {
      if (lastPointerId === null) return;
      try {
        pagesEl.setPointerCapture(lastPointerId);
        capturedId = lastPointerId;
      } catch {
        // 合成事件没有指针
      }
      lastPointerId = null;
    },
    beginDrag: (ev) => drag?.begin(ev, false),
    get drag() {
      return drag ?? undefined;
    },
        // preventScroll 不是可选的：承接层是一个绝对定位在文档里的 textarea，焦点一给它，
    //   浏览器就会把滚动容器滚到它当前所在的位置 —— 而它此刻还停在上一次光标那里。
    //   Worker 传输下"落光标"是异步的，focus 先发生，于是点第二页时视口先被拽回上一次
    //   光标所在的页（多页文档上就是"点一下乱跳"）。焦点不需要顺带滚视口：滚视口由
    //   scrollCaretIntoView 按落点算。
    focus: () => ta.focus({ preventScroll: true }),
    commit: (changed) => {
      if (!changed) {
        resetBlink();
        return true;
      }
      return afterEdit(pendingOp, 'user');
    },
    place: (act) => {
      // 落点身份就是命中的 flow：侧别已经含在里面，不必再由段号与偏移现拼一次。
      writeOp('setCaretPosition', { flow: act.flow, affinity: act.affinity });
      caretOn = true;
      writeOp('phase', { on: true });
      return afterEdit('setCaretPosition', 'user');
    },
    selectCellPara: (para) => {
      writeOp('selectCellAtPara', { para });
      caretOn = true;
      writeOp('phase', { on: true });
      return afterEdit('selectCellAtPara', 'user');
    },
    cellSelected: () => {
      caretOn = true;
      void afterEdit('cellSelectAt', 'user');
    },
    images: () => then1(read('imageList'), (r: any) => (r && Array.isArray(r.images) ? r.images : [])),
    controlToggle: (h: any) => {
      // 只看"点了哪个控件"：值、类型、选项都在主线程这份状态镜像里，不必再问引擎一趟。
      const st = (read('controls') ?? {}) as any;
      const items = Array.isArray(st.items) ? st.items : [];
      const it = items[h.field];
      if (!it) return null;
      const type = String(it.type ?? '');
      if (type !== 'radioButton' && type !== 'checkBox' && type !== 'radioGroup' &&
          type !== 'checkBoxGroup') {
        return null;
      }
      return {
        index: h.field,
        option: typeof h.opt === 'number' ? h.opt : -1,
        type,
        key: String(it.key ?? ''),
        oldValue: {
          flag: !!it.flag,
          text: String(it.sel ?? ''),
          items: Array.isArray(it.selItems) ? it.selItems.map(String) : [],
        },
      };
    },
    beforeControlChange: (change: any) =>
      typeof o.beforeControlChange === 'function' ? o.beforeControlChange(change) : true,
    toggleControl: (index: number, option: number) =>
      writeOp('toggleControl', { i: index, option }),
    onControlTap: (index: number) => {
      // 弹出层要落在控件上，所以把它的第一块矩形一起带出去（页面坐标、磅）。
      //   只读位一并带出：宿主不必再回头查一次镜像，也免得两处读到不同时刻的那一份。
      const st = (read('controls') ?? {}) as any;
      const items = Array.isArray(st.items) ? st.items : [];
      const rs = Array.isArray(items[index]?.rects) ? items[index].rects : [];
      o.onControlTap?.(index, rs.length > 0 ? rs[0] : null, items[index]?.ro === true);
    },
    selectImage: (i) => writeOp('selectImage', { i }),
    resizeImage: (i, w, h, merge) => writeOp('resizeImage', { i, w, h, merge }),
    previewImageSize: (i, w, h, handle) =>
      writeOp('previewImageResize', { i, w, h, handle }) as Maybe<any>,
    previewImageMove: (i, dx, dy) =>
      writeOp('previewImageMove', { i, dx, dy }) as Maybe<any>,
    clearImagePreview: () => writeOp('clearImagePreview') as Maybe<any>,
    tableResize,
    status: (text, isErr) => o.status?.(text, isErr),
    preedit: (text) => showPreedit(text),
    zoomBy: (delta) => o.onZoomBy?.(delta),
    zoomReset: () => o.onZoomReset?.(),
  });

  /**
   * 光标闪烁。
   *
   * 这个函数只负责"把定时器重置到可见相位"。呈现与记账在 tick 里做 ——
   *   在这里也 present 一次的结果是编辑后多跑一趟，还把账覆盖成闪烁那一笔。
   */
  function resetBlink(): void {
    caretOn = true;
    if (blinkTimer) clearInterval(blinkTimer);
    blinkTimer = window.setInterval(() => {
      if (!ready || busy || disposed || input.composing || caretHidden) return;
      caretOn = !caretOn;
      writeOp('phase', { on: caretOn });
      void repaint();
    }, 530);
  }

  /**
   * 只呈现一次（不重排）：闪烁、样式变更、显式 present 都走它。
   */
  function repaint(): Maybe<any> {
    writeOp('present', {});
    return then1(link.settle('present'), (r: any) => {
      finish(r?.stats ?? link.read('editStats'));
    });
  }

  let lastPointerId: number | null = null;
  let capturedId: number | null = null;

  const inputOn = o.input !== false;
  if (inputOn) {
  ta.addEventListener('keydown', (ev) => void input.onKeyDown(ev));
  ta.addEventListener('compositionstart', () => input.onCompositionStart());
  ta.addEventListener('compositionupdate', (ev) => input.onCompositionUpdate(ev));
  ta.addEventListener('compositionend', (ev) => void input.onCompositionEnd(ev));
  // 剪贴板：C / V / X 不经过 keydown 判定，交给浏览器自己的 copy / cut / paste 事件 ——
  //   粘贴的数据只在事件同步执行期间读得到，那一刻不读就永远读不到了。
  //   三个事件一律拦掉默认行为：放行的话承接层会收到那行假文本（复制走的是它），
  //   或把别处来的文字塞进 textarea 与输入法组合搅在一起。
  ta.addEventListener('copy', (ev) => {
    if (!ready || disposed) return;
    ev.preventDefault();
    void copyToClipboard(ev.clipboardData);
  });
  ta.addEventListener('cut', (ev) => {
    if (!ready || disposed) return;
    ev.preventDefault();
    void cutToClipboard(ev.clipboardData);
  });
  ta.addEventListener('paste', (ev) => {
    if (!ready || disposed || !ev.clipboardData) return;
    const dt = ev.clipboardData;
    const fragment = dt.getData(FRAGMENT_MIME);
    const text = dt.getData('text/plain');
    // 片段也藏在 text/html 里：自定义 MIME 过不了剪贴板时，标准类型还能把它带过来
    //   （跨实例、跨浏览器都走这一条）。
    const inHtml = fragment ? null : fragmentInHtml(dt.getData('text/html'));
    ev.preventDefault();
    if (fragment || inHtml) {
      void pasteFragmentText(fragment || inHtml || '', 'user');
      return;
    }
    // 剪贴板里只剩纯文本时，先查这段文本有没有对应的自家片段（浏览器不保证自定义
    //   类型穿过剪贴板）。文本相等就按片段粘，样式与控件都不丢。
    const memo = text ? recallPayload(text, instanceId) : null;
    if (memo) void pasteFragmentText(memo.fragment, 'user');
    else if (text) void pastePlainText(text, 'user');
  });
  // 鼠标挂在滚动容器上而不是某一张画布：画布随窗口建与丢，逐张挂监听会在换窗时漏挂；
  //   委托给容器就没有那个缺口。合成 MouseEvent（探针）没有配套的 pointerdown，
  //   所以"按下"这一条必须挂在 mousedown 上。
  scroller.addEventListener('mousedown', (ev) => void input.onPointerDown(ev));
  /**
   * 右键：先把插入点搬到指的地方，再把"这一下压着什么"交给宿主。
   *
   * 顺序有意义 —— 宿主拿到归属时光标已经在那一格 / 那个控件旁边了，它随后的动作（插行、
   *   改属性）才落在用户指的那一处。点图片时落点走 placeAt 那一路：图片是原子，光标只能
   *   停在它两侧，选中它才是"我要对这张图做事"。
   *
   * 没接 onContextMenu 就完全不拦：拦掉一个宿主没打算接的手势，等于把一个能用的功能
   *   变成不能用的。
   */
  const onContextMenu = (ev: MouseEvent): void => {
    if (!ready || disposed) return;
    const notify = o.onContextMenu;
    if (!notify) return;
    const px = pointAt(ev.clientX, ev.clientY);
    const p = docPt(px.x, px.y);
    ev.preventDefault();
    fireAndForget(
      then1(read('contextTarget', { x: p.x, y: p.y }), (target: ContextTarget) => {
        // 读不回来（引擎还没装载）就什么都不发：宿主拿到一个形状不对的落点只会崩在它自己手里。
        if (!target || typeof target.kind !== 'string') return undefined;
        if (target.kind === 'none') {
          notify(target, { x: ev.clientX, y: ev.clientY });
          return undefined;
        }
        return then1(input.placeAt(px, undefined, { tap: false }), () => {
          notify(target, { x: ev.clientX, y: ev.clientY });
        });
      }),
    );
  };
  scroller.addEventListener('contextmenu', onContextMenu);
  // 悬停只改光标样式，不动文档：落在图片把手上时换成对应方向的缩放光标。
  scroller.addEventListener('mousemove', (ev) => input.onPointerMove(ev));
  scroller.addEventListener('mouseleave', () => input.onPointerLeave());
  // 指针 id 只能从 pointerdown 上拿（mousedown 没有），而按下时要拿它去 setPointerCapture。
  scroller.addEventListener('pointerdown', (ev) => {
    // 记下指针 id：mousedown 上没有 pointerId，而按下时要拿它去 setPointerCapture。
    //   顺便把上一次的捕获放掉 —— 捕获只在 pointerup 时由浏览器自动释放，
    //   而"抬起事件丢了"正是要处理的情形。
    if (capturedId !== null) {
      try {
        pagesEl.releasePointerCapture(capturedId);
      } catch {
        // 已经没了
      }
      capturedId = null;
    }
    lastPointerId = ev.pointerId;
  });

  drag = createDragSelect({
    container: scroller,
    pointAt,
    // 拖选用夹取那一档：指针拖出画布（左右出页、上下进页间空隙）之后仍要能继续选。
    //   夹的是"设备的文档坐标"，与 pointAt 同一把尺子；单击那一路不夹 —— 点在页外
    //   本来就没有落点（那一条另有判据）。
    hitAt: (x, y) => {
      const w = Math.max(0, pagesEl.clientWidth * dpr() - 1);
      const h = Math.max(0, pagesEl.clientHeight * dpr() - 1);
      return hitAt(Math.min(Math.max(x, 0), w), Math.min(Math.max(y, 0), h), true);
    },
    extendTo: dragExtendTo,
  });
  }

  // 滚动与视口

  let scrollRaf = 0;
  let lastScrollTop = 0;

  function onScroll(): void {
    if (!ready || disposed || scrollRaf) return;
    scrollRaf = requestAnimationFrame(() => {
      scrollRaf = 0;
      if (!ready || disposed) return;
      const top = scroller.scrollTop;
      if (top === lastScrollTop) return;
      lastScrollTop = top;
      // 位置真的变了：允许自愈在这一处再补一次（见 healWindow）。
      healedFor = -1;
      // 滚动不重排：窗口没变就什么都不做，画布摆在文档流里，浏览器已经把像素搬走了。
      fireAndForget(
        then1(link.view(docScrollTop() * dpr()), (r: any) => {
          if (r && r.moved) finish();
        }),
      );
    });
  }
  // Ctrl + 滚轮（以及触控板捏合，浏览器同样带 ctrlKey）在页面上的默认行为是缩放整个页面。
  //   拦下来改成缩放编辑器：整页缩放会把工具栏与画布一起放大，而画布是位图，放大之后
  //   只剩插值糊，正文该有的清晰度反而没了。
  //   必须是非被动监听：被动监听里 preventDefault 无效，浏览器那一下缩放拦不住。
  scroller.addEventListener(
    'wheel',
    (ev) => {
      if (input.onWheel(ev)) ev.preventDefault();
    },
    { passive: false },
  );

  scroller.addEventListener('scroll', onScroll, { passive: true });

  /**
   * 指针停在哪就换什么形状：格线上是拖列宽 / 行高，图片把手是缩放，图身上是搬走。
   *
   * 没有这一步，"这里能拖"完全看不出来 —— 格线只有细细一条，与正文的文字区
   *   长得一样。判定复用命中（它一次就把格线三格与图片把手带回来），按帧节流；
   *   按键状态下不判（那时输入层已经在拖，光标由它管）。
   * @param ev 鼠标移动
   */
  function onHoverCursor(ev: MouseEvent): void {
    if (!ready || disposed) return;
    if (typeof ev.buttons === 'number' && ev.buttons !== 0) return;
    if (hoverRaf) return;
    const px = pointAt(ev.clientX, ev.clientY);
    if (!px) return;
    hoverRaf = requestAnimationFrame(() => {
      hoverRaf = 0;
      void then1(hitAt(px.x, px.y), (h) => {
        if (!h) {
          pagesEl.style.cursor = 'text';
          return;
        }
        if (typeof h.img === 'number' && h.img >= 0) {
          // 八个把手按方位给形状：角上是斜双箭头，边上是单轴双箭头。
          const cursors = ['nwse-resize', 'ns-resize', 'nesw-resize', 'ew-resize',
                           'nwse-resize', 'ns-resize', 'nesw-resize', 'ew-resize'];
          const k = typeof h.imgHandle === 'number' ? h.imgHandle : -1;
          pagesEl.style.cursor = k >= 0 ? cursors[k] ?? 'default' : 'move';
          return;
        }
        if (typeof h.gridCol === 'number' && h.gridCol > 0) {
          pagesEl.style.cursor = 'col-resize';
          return;
        }
        if (typeof h.gridRow === 'number' && h.gridRow >= 0) {
          pagesEl.style.cursor = 'row-resize';
          return;
        }
        pagesEl.style.cursor = 'text';
      });
    });
  }
  pagesEl.addEventListener('mousemove', onHoverCursor, { passive: true });

  const stopDprWatch = armDprWatch((next) => {
    if (!ready || disposed) return;
    if (next === zoom.dpr) return;
    // 倍数不变、密度按新屏幕重算：屏幕上仍是同一个百分比。
    //   反推倍数必须用 storedDpr（旧屏幕），不能用新 DPR —— 那会读到一个平白缩掉
    //   一半的假倍数，照它写回去等于空操作。
    const factor = zoom.factorOf(zoom.density);
    const density = zoom.ofFactor(factor);
    zoom.set(density);
    writeOp('zoom', { zoom: density, dpr: next });
    pages.invalidate();
    then1(link.settle('zoom'), () => finish());
  });

  const onWindowError = (e: ErrorEvent): void => {
    if (disposed) return;
    o.onPageError?.('error: ' + String(e.message));
  };
  const onRejection = (e: PromiseRejectionEvent): void => {
    if (disposed) return;
    o.onPageError?.('unhandledrejection: ' + String(e.reason));
  };
  window.addEventListener('error', onWindowError);
  window.addEventListener('unhandledrejection', onRejection);

  // 对外

  const host: DomHost = {
    stage: stageEl,
    pagesEl,
    ta,
    zoom,
    input,
    get drag() {
      return drag;
    },
    syncPages: () => {
      const g = view();
      pages.sync(g.first, g.count, g.totalPages, g);
    },
    invalidatePages: () => pages.invalidate(),
    keepScrollAcrossZoom: (before: number, after: number) => {
      if (!(before > 0) || !(after > 0) || before === after) return false;
      // 折算的是"页顶之上那一段边距之下"的部分：上边距是骨架给的固定 CSS 值，
      //   不随缩放变，跟着乘倍率会把页整体推下来一截。
      const docTop = docScrollTop();
      const prevTop = scroller.scrollTop;
      scroller.scrollTop = Math.max(0, docTop * (after / before)) + pagesEl.offsetTop;
      return nudgeViewport(prevTop);
    },
    repaint,
    finish,
    afterEdit,
    copy: async () => !!(await Promise.resolve(copyToClipboard(null))),
    cut: async () => !!(await Promise.resolve(cutToClipboard(null))),
    /**
     * 程序化粘贴：先读系统剪贴板，再按"自家片段优先、否则纯文本"落进文档。
     *
     * 与键盘那次粘贴的差别只有一处：这里的数据要经 navigator.clipboard.read()，需要浏览器
     *   授权；键盘那条由 paste 事件把数据直接送进来，不需要。
     * @returns 粘上了没有
     */
    paste: async () => {
      const clip = await readSystemClipboard();
      if (!clip) {
        // 读整条被拒（没有 clipboard-read 授权）：用最近一次写出去的那一份 ——
        //   编辑器内的"复制 → 粘贴"本来就不该依赖权限。
        const memo = latestPayload(instanceId);
        if (memo) return !!(await Promise.resolve(pasteFragmentText(memo.fragment, 'api')));
        o.status?.('读不到系统剪贴板（浏览器未授权）', true);
        return false;
      }
      if (clip.fragment) return !!(await Promise.resolve(pasteFragmentText(clip.fragment, 'api')));
      const inHtml = clip.html ? fragmentInHtml(clip.html) : null;
      if (inHtml) return !!(await Promise.resolve(pasteFragmentText(inHtml, 'api')));
      if (clip.text) {
        const memo = recallPayload(clip.text, instanceId);
        if (memo) return !!(await Promise.resolve(pasteFragmentText(memo.fragment, 'api')));
        return !!(await Promise.resolve(pastePlainText(clip.text, 'api')));
      }
      o.status?.('剪贴板里没有可粘贴的文字', true);
      return false;
    },
    scrollCaretIntoView,
    resetScroll: applyScrollReset,
    blitStats,
    startBlink: () => resetBlink(),
    stopBlink() {
      if (blinkTimer) {
        clearInterval(blinkTimer);
        blinkTimer = 0;
      }
      caretOn = true;
      // 只写相位、不 present：present 会把 edit_stats 覆盖成"只重画了个光标"，
      //   而调用方紧接着要读的往往是上一次全量呈现的账。
      writeOp('phase', { on: true });
    },
    pointAt,
    hitAt,
        // preventScroll 不是可选的：承接层是一个绝对定位在文档里的 textarea，焦点一给它，
    //   浏览器就会把滚动容器滚到它当前所在的位置 —— 而它此刻还停在上一次光标那里。
    //   Worker 传输下"落光标"是异步的，focus 先发生，于是点第二页时视口先被拽回上一次
    //   光标所在的页（多页文档上就是"点一下乱跳"）。焦点不需要顺带滚视口：滚视口由
    //   scrollCaretIntoView 按落点算。
    focus: () => ta.focus({ preventScroll: true }),
    start() {
      ready = true;
      lastScrollTop = scroller.scrollTop;
      resetBlink();
    },
    setBusy(on: boolean) {
      busy = on;
    },
    resize() {
      return then1(link.view(docScrollTop() * dpr()), () => finish());
    },
    destroy() {
      if (disposed) return;
      disposed = true;
      ready = false;
      if (blinkTimer) clearInterval(blinkTimer);
      if (scrollRaf) cancelAnimationFrame(scrollRaf);
      stopDprWatch();
      window.removeEventListener('error', onWindowError);
      window.removeEventListener('unhandledrejection', onRejection);
      drag?.end();
      drag = null;
      pages.destroy();
      root.innerHTML = '';
    },
  };

  return host;
}
