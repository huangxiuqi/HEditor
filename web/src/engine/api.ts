// 引擎侧：命令表、状态、窗口计算。这一份就是引擎语义本身。
//
// 边界：本文件不碰 document、不碰 DOM、不碰 OffscreenCanvas。凡是要碰画布的地方都走
//   cfg.host 的钩子（ctxOfPage / blitPages / invalidateSlots ...），由宿主实现 ——
//   引擎于是只认"页位图 + 槽"，不认它跑在哪条线程上。
// 通道（与 transport/worker_main.ts 的约定）：
//   下行：boot / pageCanvas / viewMode / view / do（改状态）/ ask（要返回值）
//   上行：ready / state / ack / reply / error

// 类型：只封边界，不重抄导出面

/**
 * wasm 模块实例。
 *
 * 为什么不逐个写出签名：那 107 个导出的真源是 C++ 侧与 emcc --emit-tsd 生成的
 *   web/types/wasm.gen.d.ts（§10.2）。在这里手抄一遍就是第三份，而且它会静默过期。
 *   所以本文件内部一律按"引擎句柄"用，边界上的类型由生成物提供。
 */
type Mod = any;

/**
 * 一条命令的参数：键名由命令表那一列与调用方约定，不在这里建模。
 */
export type Args = Record<string, any>;

/**
 * 命令实现：返回"有没有改动"、一份结果，或装载类的 Promise。
 */
type Cmd = (a: Args) => any;

/**
 * 引擎侧要宿主提供的呈现钩子（worker 壳与主线程宿主各一份实现）。
 */
export interface EngineHostHooks {
  /**
   * 把还欠着的脏页贴到页画布上，返回贴了几页。
   */
  blitPages(): number;

  /**
   * 最近一次贴页的账：毫秒 / 页数 / 像素。
   */
  blitStats(): { ms: number; pages: number; px: number };

  /**
   * 内容整体作废（换缩放 / 换页度量 / 换文档）：所有槽都要重贴。
   */
  invalidateSlots(): void;

  /**
   * 主线程报来的一张页画布（worker 侧登记槽位）。
   * @param m 画布消息
   */
  addCanvas(m: Args): void;

  /**
   * 切分页 / 连页与页间隙。
   * @param m 视图模式消息（gap 与 continuous）
   */
  setViewMode(m: Args): void;

  /**
   * 已经建好几个槽。
   */
  slotCount(): number;

  /**
   * 还欠几页画布（0 = 窗口内每页都有画布）。
   */
  pagesMissing(): number;

  /**
   * 某一页的画布上下文（worker 侧贴图用）。
   * @param page 页号
   */
  ctxOfPage(page: number): any;

  /**
   * 已经建好几张画布。
   */
  canvasCount(): number;

  /**
   * 装载胶水层并返回工厂（worker 用 importScripts，主线程用 <script>）。
   * @param url 胶水脚本地址
   * @param exportName 工厂的全局名
   */
  loadGlue(url: string, exportName: string): Promise<unknown>;
}

export interface EngineApiConfig {
  /**
   * 上行发送。worker 侧是 postMessage，主线程侧是直接丢弃（in-thread 自己取返回值）。
   * @param msg 要发的消息
   * @param transfer 要转移所有权的对象
   */
  post(msg: Args, transfer?: Transferable[]): void;
  host: EngineHostHooks;
}

/**
 * 命令表实例。
 *
 * 两套入口用途不同、不能混：read / write 给 in-thread 宿主，只跑引擎那一步，收尾
 *   （present / 重贴 / 回推）留在宿主侧，因为同步宿主靠"调用即生效"当对照物；
 *   do / ask / view / pageCanvas / viewMode 给 worker 壳，跑完自己收尾并回推。
 * engine() 只给装载与取像素视图，不要拿它绕过命令表。
 */
export interface EngineApi {
  /**
   * 装载引擎并排出首帧。
   * @param m boot 消息（胶水地址、字体、视口、缩放）
   */
  boot(m: Args): Promise<void>;

  /**
   * 执行一条写命令（含收尾与状态回推）。
   * @param m do 消息（命令名与参数）
   */
  do(m: Args): Promise<void>;

  /**
   * 执行一条读命令，回一条 reply。
   * @param m ask 消息（查询名与参数）
   */
  ask(m: Args): Promise<void>;

  /**
   * 视口变化：落滚动位置、按需换窗、必要时呈现并重贴。
   * @param m 视口消息（scrollY、视口尺寸、提前量）
   */
  view(m: Args): { moved: boolean; scrolled: boolean; workMs: number; stats?: any };

  /**
   * 一页画布到达：登记并补贴那一页。
   * @param m 画布消息（槽号、槽数、OffscreenCanvas）
   */
  pageCanvas(m: Args): void;

  /**
   * 切换分页 / 连页（含重排与重贴）。
   * @param m 视图模式消息
   */
  viewMode(m: Args): void;

  /**
   * 成套设置页间隙与连页标志（分两路设会出现不存在的组合）。
   * @param m 视图模式消息
   */
  setViewMode(m: Args): void;

  /**
   * 只重排重画（给宿主的 resize 与 dpr 变化用）。
   */
  relayout(): void;

  /**
   * 把这一拍的状态推给宿主。
   * @param ts 回话序号（0 = 不与某个调用配对）
   * @param label 这一拍的来源（命令名）
   * @param stats 编辑那本账
   * @param changed 文档内容有没有变
   * @param workMs 这一下在引擎侧花了多少毫秒
   */
  sendState(ts: number, label: string, stats: any, changed?: boolean, workMs?: number): void;

  /**
   * 几何快照（页度量、窗口、槽数上限）。
   * @param st 这一拍的账，可空
   */
  geom(st?: any): Args;

  /**
   * 把已装好的引擎实例交给命令表。
   * @param engine wasm 模块实例
   */
  attach(engine: Mod): void;

  /**
   * in-thread 的收尾（滚动 → 窗口 → present → 贴页）。
   * @param opName 刚跑完的命令名
   * @param m 滚动位置与视口尺寸
   */
  settle(opName: string, m: Args): { moved: boolean; scrolled: boolean; stats: any };

  /**
   * 装载类命令把滚动位置归零了：读一次就清。
   */
  takeScrollReset(): boolean;

  /**
   * 直接执行一条读命令，返回原值。
   * @param name 查询名
   * @param args 参数
   */
  read(name: string, args?: Args): any;

  /**
   * 直接执行一条写命令，返回引擎那一步的结果。
   * @param name 命令名
   * @param args 参数
   */
  write(name: string, args?: Args): any;

  /**
   * 引擎句柄（装载与取帧视图用，不要拿它绕过命令表）。
   */
  engine(): Mod;

  /**
   * 当前滚动位置（设备像素）。
   */
  scrollY(): number;

  /**
   * 视口尺寸（设备像素）。
   * @returns 宽与高
   */
  viewSize(): { w: number; h: number };

  /**
   * 推一份状态（给薄壳用，与 sendState 同一条路）。
   * @param ts 回话序号
   * @param label 来源
   * @param stats 编辑那本账
   * @param changed 内容有没有变
   * @param workMs 引擎侧耗时
   */
  state(ts: number, label: string, stats: any, changed?: boolean, workMs?: number): void;
}

// 协议表

/**
 * 宿主与引擎之间的每一个动词，一条一行：[命令名, 类别, 调的 wasm 导出, 呈现归属]。
 *
 * 类别：'w' 写（跑完收尾并回推状态），'r' 读（只回值），'s' 静默写（改状态但不上屏、
 *   不回推），'q' 查询但在 askOp 里特判（rawFrame 帧太大，要转移所有权而不是克隆）。
 * 呈现归属：'self' 引擎在库里就排完画完，'host' 引擎只改状态、宿主补一次 present，
 *   'none' 根本不碰画面。
 *
 * 呈现归属是账本的一部分而不是风格选择：多补一次 present 会把这一拍的重画统计冲成
 *   "只画了一个光标"（探针拿那笔账当分母），少补一次画面就停在上一拍。它由"这条命令
 *   调的是不是 *AndRender"决定，所以只能写在表里，不能另抄一份清单。
 * 这张表与三张实现表由 collect() 对账：少一条、多一条都在造 API 时抛出。
 */
const COMMANDS: Array<[string, string, string[], string]> = [
  // 呈现与装载：整份换文档，引擎自己重排重画
  // 实现是空的：render() 认这个名字去补一次 mod.present()
  ['present',               'w', ['present'],                                             'host'],
  ['scenario',              'w', ['setScenario'],                                         'self'],
  ['reset',                 'w', ['setZoom', 'setScenario'],                              'self'],
  ['zoom',                  'w', ['setZoom'],                                             'self'],
  ['stress',                'w', ['loadStressDocument'],                                  'self'],
  ['stressDiverse',         'w', ['loadStressDocumentDiverse'],                           'self'],
  ['textDoc',               'w', ['loadTextDocument'],                                    'self'],
  ['formSample',            'w', ['loadFormSample'],                                      'self'],
  ['loadDocument',          'w', ['loadDocument'],                                        'self'],
  ['fonts',                 'w', ['loadFont', 'loadFontFromFile', 'setFontRoles'],        'self'],
  ['setFormControls',       'w', ['setFormControls'],                                     'self'],

  // 编辑：*AndRender 系在库里就排完画完了，宿主不要再画一遍
  ['insertText',            'w', ['insertTextAt'],                                           'self'],
  ['type',                  'w', ['typeText'],                                            'self'],
  ['backspace',             'w', ['backspace'],                                           'self'],
  ['deleteForward',         'w', ['deleteForward'],                                       'self'],
  ['splitParagraph',        'w', ['splitParagraph'],                                      'self'],
  ['mergePrev',             'w', ['mergeWithPreviousParagraph'],                          'self'],
  ['mergeNext',             'w', ['mergeWithNextParagraph'],                              'self'],
  ['move',                  'w', ['moveCaret'],                                           'self'],
  ['vertical',              'w', ['moveCaretVertical'],                                   'self'],
  ['docStart',              'w', ['caretToDocStart'],                                     'self'],
  ['docEnd',                'w', ['caretToDocEnd'],                                       'self'],
  ['lineStart',             'w', ['caretToLineStart'],                                    'self'],
  ['lineEnd',               'w', ['caretToLineEnd'],                                      'self'],
  ['indent',                'w', ['indent'],                                              'self'],
  ['outdent',               'w', ['outdent'],                                             'self'],
  ['tabCaret',              'w', ['tabCaret'],                                            'self'],
  ['undo',                  'w', ['undoEdit'],                                            'self'],
  ['redo',                  'w', ['redoEdit'],                                            'self'],
  ['removeControl',         'w', ['removeControlAt'],                                     'self'],
  ['setControlProp',        'w', ['setControlPropAt'],                                    'self'],
  ['focusControl',          'w', ['focusControl'],                                        'self'],

  // 选区与光标：选区的几条自己收尾；落光标、拖选、闪烁只改状态，要宿主补呈现
  ['selectAll',             'w', ['selectAll'],                                           'self'],
  ['selectCellAtPara',      'w', ['selectCellAtPara'],                                    'self'],
  ['cellSelectAt',          'w', ['cellSelectAt'],                                        'self'],
  ['selectPos',             'w', ['setSelection', 'flowIndexAt'],                                        'self'],
  ['setSelection',          'w', ['setSelection'],                                      'self'],
  ['clearSelection',        'w', ['clearSelection'],                                      'self'],
  ['setCaret',              'w', ['setCaret', 'showCaret'],                               'host'],
  ['setCaretAt',            'w', ['setCaretAt', 'showCaret'],                             'host'],
  ['setCaretAtSide',        'w', ['setCaretAtSide', 'showCaret'],                         'host'],
  ['setCaretAtSideLine',    'w', ['setCaretAtSideLine', 'showCaret'],                     'host'],
  ['setFocusAtSideLine',    'w', ['setFocusAtSideLine', 'showCaret'],                     'host'],
  ['setFocusAtSide',        'w', ['setFocusAtSide', 'showCaret'],                         'host'],
  ['blinkPhase',            'w', ['showCaret'],                                           'host'],
  ['viewport',              'w', ['setViewportAnchor'],                                   'host'],

  // 配置：改参数；改到画面的那几条（模式 / 括号 / 小点 / 光标色）要补呈现
  ['setEditMode',           'w', ['setEditMode'],                                         'host'],  // 会重编译整篇，但不重绘
  ['setShowBrackets',       'w', ['setShowBrackets'],                                     'host'],  // 会重排（括号占宽度）
  ['setShowControlDots',    'w', ['setShowControlDots'],                                  'host'],  // 不重排，只改光栅签名
  ['caretStyle',            'w', ['caretStyleJson', 'setCaretStyle'],                     'host'],
  ['selectionStyle',        'w', ['selectionStyleJson', 'setSelectionStyle'],             'host'],
  ['setUndoLimit',          'w', ['setUndoLimit'],                                        'none'],
  ['setFontCacheConfig',    'w', ['setFontCacheConfig'],                                  'none'],

  // 静默写：改引擎状态，但不上屏也不回推（门面的 showCaret / stopBlink）
  ['phase',                 's', ['showCaret'],                                           'none'],

  // 查询：文档与结构
  ['ping',                  'r', [],                                                      'none'],  // 排空屏障：什么都不算，只回一个值
  ['textAll',               'r', ['documentTextAll'],                                   'none'],
  ['text',                  'r', ['documentText'],                                        'none'],
  ['serialize',             'r', ['serializeDocument'],                                   'none'],
  // 复制是查询（不改状态、不上屏）；粘贴与删选区是命令（自带重排与呈现）。
  //   剪贴板载荷由引擎给，宿主只把它写进系统剪贴板 —— 片段的格式不在 JS 里重建。
  ['copy',                  'r', ['copySelection'],                                       'none'],
  ['paste',                 'w', ['pasteFragment'],                                       'self'],
  ['pasteText',             'w', ['pastePlainText'],                                      'self'],
  ['deleteSelection',       'w', ['deleteSelection'],                                     'self'],
  ['hit',                   'r', ['hitTest'],                                             'none'],
  ['controlAt',             'r', ['controlAtPoint'],                                      'none'],
  ['describeControls',      'r', ['describeControls'],                                    'none'],
  ['controls',              'r', ['controlsJson'],                                        'none'],
  ['bracketSlots',          'r', ['bracketSlotsJson'],                                    'none'],
  ['lines',                 'r', ['linesJson'],                                           'none'],
  ['blockedEdits',          'r', ['blockedEdits'],                                        'none'],
  ['editMode',              'r', ['editMode'],                                            'none'],
  ['showBrackets',          'r', ['showBrackets'],                                        'none'],
  ['showControlDots',       'r', ['showControlDots'],                                     'none'],
  ['undoLimit',             'r', ['undoLimit'],                                           'none'],

  // 查询：几何
  ['frameWidth',            'r', ['frameWidth'],                                          'none'],
  ['frameHeight',           'r', ['frameHeight'],                                         'none'],
  ['frameByteSize',         'r', ['frameByteSize'],                                       'none'],
  ['pagePxWidth',           'r', ['pagePxWidth'],                                         'none'],
  ['pagePxHeight',          'r', ['pagePxHeight'],                                        'none'],
  ['pageGapPx',             'r', ['pageGapPx'],                                           'none'],
  ['windowFirstPage',       'r', ['windowFirstPage'],                                     'none'],
  ['windowPageCount',       'r', ['windowPageCount'],                                     'none'],

  // 查询：光标与选区
  ['caretOffsetLive',       'r', ['caretOffsetLive'],                                     'none'],
  ['caretVerticalNow',      'r', ['caretVerticalNow'],                                    'none'],
  ['caretBlockReversedNow', 'r', ['caretBlockReversedNow'],                               'none'],
  ['flowTextPos',           'r', ['flowTextPosJson'],                                     'none'],
  ['selectionReport',       'r', ['selectionReport'],                                     'none'],
  ['caretStyleJson',        'r', ['caretStyleJson'],                                      'none'],
  ['selectionStyleJson',    'r', ['selectionStyleJson'],                                  'none'],
  ['caretReport',           'r', ['caretReport'],                                         'none'],

  // 查询：像素与账本
  ['ink',                   'r', ['frameRgba'],                                           'none'],
  ['inkRows',               'r', ['frameRgba', 'frameWidth', 'pageGapPx', 'pagePxHeight', 'windowFirstPage', 'windowPageCount'], 'none'],
  ['frameDigest',           'r', ['frameRgba'],                                           'none'],
  ['stats',                 'r', ['statsJson'],                                           'none'],
  ['editStats',             'r', ['editStatsJson'],                                       'none'],
  ['dirtyPages',            'r', ['dirtyPagesJson'],                                      'none'],
  ['pageDirtyRects',        'r', ['pageDirtyRectsJson'],                                  'none'],
  ['pageDirtyPages',        'r', ['pageDirtyPagesJson'],                                  'none'],
  ['prewarmPage',           'r', ['prewarmPageGlyphs'],                                   'none'],
  ['heap',                  'r', [],                                                      'none'],  // 读的是模块属性 HEAPU8，不是导出
  ['buildStamp',            'r', ['buildStamp'],                                          'none'],
  ['rasterBackend',         'r', ['rasterBackend'],                                       'none'],
  ['bench',                 'r', ['benchRelayoutPaint'],                                  'none'],

  // 查询：自审与诊断
  ['auditFrame',            'r', ['auditFrame'],                                          'none'],
  ['auditBase',             'r', ['auditBase'],                                           'none'],
  ['auditPage',             'r', ['auditPage'],                                           'none'],
  ['auditLayout',           'r', ['auditLayout'],                                         'none'],
  ['auditCompile',          'r', ['auditCompile'],                                        'none'],
  ['auditGlyphPages',       'r', ['auditGlyphPages'],                                     'none'],
  ['selfCheck',             'r', ['documentText', 'editStatsJson', 'frameHeight', 'frameWidth', 'pagePxHeight', 'pagePxWidth', 'pageRgba', 'rasterBackend', 'windowFirstPage', 'windowPageCount'], 'none'],
  ['markCheck',             'r', ['controlsJson', 'frameRgba', 'frameWidth', 'showBrackets'], 'none'],

  // 问答特判：帧太大，要转移所有权而不是结构化克隆（见 askOp 里的 rawFrame 分支）
  ['rawFrame',              'q', ['frameRgba'],                                           'none'],
];

/**
 * 不经过命令表的导出。每一条都要说清为什么 —— 这份清单同时是"漏了一个导出"的探测器：
 *   谁在 wasm 侧新增了一个导出却忘了登记，scripts/idl_check.mjs 会把名字打出来。
 *
 * kind 只作分类：glue = wasm 堆与字节搬运，host = 宿主自己发起的装载与呈现，
 *   state = 只喂状态对象，diag = 手工诊断。
 */
const OUTSIDE: Array<[string, string, string]> = [
  ['allocBuffer',      'glue',  '字体的字节从 JS 搬进 wasm 堆'],
  ['freeBuffer',       'glue',  '同上'],
  ['relayoutAndPaint', 'host',  '整篇重排重画：paintFull 与 relayout 用它换文档、换缩放、换页度量'],
  ['setPageMetrics',   'host',  '页度量只在装载时定一次（app.js 与 boot），没有运行期入口'],
  ['setPageGap',       'host',  '页间隙：由 api.setViewMode 与连页标志一起设'],
  ['setContinuous',    'host',  '连页标志：同上（改的是排版形状，不是一份配置）'],
  ['contentPx',        'state', 'geom() 报的内容底边（连页拿它当滚动高度）'],
  ['relayout',         'diag',  '只排版不画（手工诊断）；目前没有任何调用方'],
  ['shapeReport',      'diag',  '单段整形报告（手工诊断）；目前没有任何调用方'],
];

// 按类别取命令名。
function idlByKind(kind: string): string[] {
  return COMMANDS.filter((c) => c[1] === kind).map((c) => c[0]);
}

const IDL = {
  commands: COMMANDS,
  outside: OUTSIDE,
  byKind: idlByKind,
  // 要宿主补一次 present 的命令名。派生，不维护。
  presentAlways: () => new Set(COMMANDS.filter((c) => c[3] === 'host').map((c) => c[0])),
};

/**
 * 把实现表按协议表"套"起来：IDL 里有、实现里没有（或反过来）当场抛。
 *
 * 两个方向都要拦：只拦一个方向的话，"删了一条命令忘删实现"会留一个永远跑不到的体；
 *   而"加了一条忘登记"会静默地当未知命令 —— 宿主那边只看到一句 未知命令 x。
 */
function collect(kind: string, impl: Record<string, Cmd>): Record<string, Cmd> {
  const want = idlByKind(kind);
  const missing = want.filter((n) => !Object.prototype.hasOwnProperty.call(impl, n));
  const extra = Object.keys(impl).filter((n) => want.indexOf(n) < 0);
  if (missing.length || extra.length) {
    throw new Error('协议表与实现不一致（' + kind + '）：缺 [' + missing.join(', ') +
                    ']，多 [' + extra.join(', ') + ']');
  }
  return impl;
}

// 引擎自己不画的那些命令 —— 派生自协议表的第四列，不另维护。
const PRESENT_ALWAYS = IDL.presentAlways();

/**
 * 造一份引擎 API。
 *
 * @param {object} cfg 传输与呈现钩子
 * @param {function} cfg.post 上行发送；worker 侧是 postMessage，主线程侧是直接回调
 * @param {object} cfg.host 画布钩子，见文件头的三条分叉
 * @returns {object} boot / do / ask / view / pageCanvas / viewMode / state / relayout
 */
function createEngineApi(cfg: EngineApiConfig): EngineApi {
const post = cfg.post;
const host = cfg.host;

const err = (e: any, where?: string) =>
  post({ t: 'error', where: where || '', message: String((e && e.stack) || e) });

let mod: Mod = null;
let dpr = 1;
let viewW = 1;
let viewH = 1;
let scrollY = 0;
// 宿主报来的提前量（设备像素，连页模式下可能是负的）。它只用来选窗口 —— 真实落点一律按
//   scrollY 算，两者混用会让连页的内容跑到滚动位置前面去（停稳再弹回来）。
let scrollLead = 0;
let lastAnchor = '';
let scrollReset = false;   // 引擎自己归零过滚动位置，要通知宿主追平（随 state 送出去）
// in-thread 宿主没有 state 通道：它直接调 takeScrollReset()。两个标记分开，
//   因为 sendState 打包时会把上面那个清掉 —— 合成一个的话，宿主永远读到 false。
let scrollResetHost = false;
let bootMs = 0;
let fontMs = 0;
let fontBytes = 0;
let fontList: any[] = [];
let fontMode = 'full';
// 与单线程宿主同一口径：缩放 1 倍 = 96 dpi = 4 / 3。
const kZoomBase = 4 / 3;
let zoomLevel = kZoomBase;   // markCheck 要用：括号左缘的像素位置随缩放走
// 页度量由宿主传，与单线程宿主同一组值（常量与理由见 app.js 的同名声明）。
// A4 = 595.276 x 841.89 磅；页边距 2 厘米 = 2 x 72 / 2.54 磅。
const kPageWidthPt = 595.276;
const kPageHeightPt = 841.89;
const kPageMarginPt = 56.7;

// 字体

async function fetchFont(path: string): Promise<Uint8Array> {
  const res = await fetch(path);
  if (!res.ok) throw new Error(path + ' -> HTTP ' + res.status);
  return new Uint8Array(await res.arrayBuffer());
}

/**
 * 两种装载方式。full 把整份字节交给引擎；lazy 先写进 MEMFS 再让引擎按路径按需读。
 *   MEMFS 在 Worker 里同样可用（它就是 JS 侧的内存文件系统），所以两条路都能在这边跑。
 */
async function installFonts(mode: string): Promise<number> {
  const t0 = performance.now();
  const idx: Record<string, number> = {};
  let bytes = 0;
  for (const f of fontList) {
    const buf = await fetchFont(f.path);
    bytes += buf.length;
    let ex = -1;
    if (mode === 'lazy') {
      const name = '/' + f.path.split('/').pop();
      try { mod.FS.unlink(name); } catch (e) { /* 不存在就算了 */ }
      // canOwn=true：MEMFS 直接持有这份字节，省掉一次十几 MB 的拷贝。
      mod.FS.createDataFile('/', name, buf, true, false, true);
      ex = mod.loadFontFromFile(name);
    } else {
      const ptr = mod.allocBuffer(buf.length);
      mod.HEAPU8.set(buf, ptr);
      // 传文件名而不是 label：字体身份要跨装载方式稳定 —— 保存时引擎把它写进文件的字体表，
      //   两条路各写一个名字的话，"换一种装载方式存一次盘"就成了两份不同的文件。
      ex = mod.loadFont(f.path.split('/').pop(), ptr, buf.length);
      mod.freeBuffer(ptr);
    }
    if (ex < 0) throw new Error('字体解析失败: ' + f.path);
    idx[f.role] = ex;
  }
  mod.setFontRoles(idx.cjk, idx.mongol, idx.latin);
  fontMs = performance.now() - t0;
  fontBytes = bytes;
  fontMode = mode;
  return bytes;
}

// 几何

function geom(st: any) {
  const ph = mod.pagePxHeight();
  const step = ph + mod.pageGapPx();
  const first = mod.windowFirstPage();
  return {
    pageW: mod.pagePxWidth(),
    pageH: ph,
    gap: mod.pageGapPx(),
    first,
    count: mod.windowPageCount(),
    // 页画布在文档里的落点（设备像素）：纵向 = 页号 × 步距，横向由宿主居中。
    pageStep: step,
    frameW: mod.frameWidth(),
    frameH: mod.frameHeight(),
    dpr, viewW, viewH, scrollY,
    // 内容底边（设备像素）。连页下宿主拿它当滚动高度 —— 没有页可数，只有内容。
    contentPx: mod.contentPx ? mod.contentPx() : 0,
    // 槽数的上限（与文档位置无关，只随视口与页度量变）。宿主按它建槽、再把自己实际
    //   建了几个报回来（pageCanvas.slots）—— 一个数、一个来源，两侧不会分叉。
    maxWindowPages: maxWindowPages(),
    // 这一侧实际在用的槽数，供诊断读。它必须等于宿主报回来的那个值。
    slots: host.slotCount(),
    pagesReady: host.pagesMissing() === 0,
    // 与单线程宿主的 viewport 同形的两格：锚点原文与"该装哪几页"。
    //   探针靠它们判"窗口跟着可见页走"，缺了就是一条 .first 的 TypeError。
    anchor: lastAnchor,
    desired: st ? desiredWindow(st) : null,
    visible: st ? visibleRange(st) : null,
  };
}

/**
 * 按当前滚动位置决定窗口。返回 true 表示窗口变了（调用方要重排重画）。
 *
 * 滞回（与单线程宿主的 applyWindow 同一口径）：现有窗口只要还盖得住"看得见的那几页"，
 *   就一个字都不动。没有这一层的话，滚动每跨一条页边界就整窗重光栅（一页约 13 ms）；
 *   有了它，同一页内的滚动连一次重贴都不需要 —— 浏览器把画布搬走就行。
 */
// 看得见的那几页（设备像素下的滚动位置与视口高度）。
//   这里用的是"报了提前量的位置"（scrollY + scrollLead）：提前量只该影响"先装哪几页"，
//   让窗口朝滚动方向多留一点，不要等滚到了才开始装。它不能影响"画在哪儿" ——
//   页的落点由页号直接定（页号 x 步距），与滚动无关。
function visibleRange(st: any) {
  const total = Math.max(1, Number(st.totalPages) || 1);
  const step = mod.pagePxHeight() + mod.pageGapPx();
  const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));
  const at = scrollY + scrollLead;
  const first = clamp(Math.floor(at / step), 0, total - 1);
  const last = clamp(Math.floor((at + viewH - 1) / step), first, total - 1);
  return { first, last };
}

/**
 * 窗口最多装几页：视口装得下的页数，再加 4 页余量。与单线程宿主的 maxWindowPages
 *   同一口径（那条注释给了实测数字：换窗成本正比于窗口长度而非位移，余量 8 时一跳要
 *   94~286 ms，余量 4 时 45 ms 上下；余量大了反而把落后从 3 页推到 8~28 页）。
 *   两道封顶同理：页数 8、帧像素 24 Mpx。
 */
const kWindowMarginPages = 4;
const kWindowMaxPages = 8;
const kWindowMaxPixels = 24 * 1024 * 1024;

function maxWindowPages() {
  const step = mod.pagePxHeight() + mod.pageGapPx();
  const vis = Math.ceil(viewH / step);
  const byPixels = Math.floor(kWindowMaxPixels /
                               Math.max(1, mod.pagePxWidth() * mod.pagePxHeight()));
  return Math.max(vis, Math.min(kWindowMaxPages, byPixels, vis + kWindowMarginPages));
}

/**
 * 该装哪几页（这是要求，不是"要设成的窗口"）。与单线程宿主的 desiredWindow 同一口径
 *   （那边注释解释了为什么光标页只在"不挤掉可见页"时才并进来）。
 */
function desiredWindow(st: any) {
  const total = Math.max(1, Number(st.totalPages) || 1);
  const vis = visibleRange(st);
  let lo = vis.first;
  let hi = vis.last;
  const cp = Number(st.caretPage);
  if (maxWindowPages() - (hi - lo + 1) > 0 && cp >= 0 && cp < total) {
    if (cp === lo - 1) lo = cp;
    else if (cp === hi + 1) hi = cp;
  }
  let first = Math.max(0, lo);
  const need = hi - lo + 1;
  const count = Math.max(1, Math.min(need, total));
  if (first + count > total) first = Math.max(0, total - count);
  return { first, count };
}

/**
 * 换窗的提前量（页）与滚动方向。与单线程宿主的同名常量同一口径 ——
 *   那边解释了为什么余量要偏向滚动方向、以及"贴到窗口边缘才换窗"为什么会闪白
 *   （换窗那一拍 20~45 ms，合成器在这期间继续往前滚，每秒 80 页就是 4~5 页）。
 */
const kWindowLookaheadPages = 2;
let scrollDir = 1;

/**
 * 要挪窗口时挪到哪：把多出来的页放在滚动方向上。与单线程宿主的 targetWindow 同一口径
 *   —— 只开"必须的那几页"会让每滚一页都整窗重光栅（那边注释里有实测数字：留余量之后
 *   一页滚动只要 1 ms，不留则一页 10~30 ms）。
 */
function targetWindow(req: any, total: any, cap: any) {
  const count = Math.max(req.count, Math.min(cap || 3, total));
  const spare = count - req.count;
  const behind = scrollDir < 0 ? Math.max(0, spare - 1) : 1;
  let first = req.first - Math.min(behind, spare);
  if (first + count > total) first = total - count;
  if (first < 0) first = 0;
  return { first, count: Math.max(1, Math.min(count, total)) };
}

function retarget(st: any) {
  const total = Math.max(1, Number(st.totalPages) || 1);
  const req = desiredWindow(st);
  const look = kWindowLookaheadPages;
  // 要求先夹到文档范围内：第 0 页往上、最后一页往下都没有"那一页"可留，
  //   不夹的话 needFirst 会是 -1 —— 那是个永远满足不了的判据（窗口不可能从 -1 页起），
  //   于是滞回恒判"盖不住"，每一拍都白换一次窗。
  const needFirst = Math.max(0, req.first - (scrollDir < 0 ? look : 0));
  const needLast = Math.min(total - 1,
                            req.first + req.count - 1 + (scrollDir > 0 ? look : 0));
  // 判据必须取"引擎这一刻真的装着哪个窗口"，不能用上一次请求过的锚点：引擎呈现时会把
  //   越界锚点夹回文档范围内（repaint 里的 clamp），而请求值不会跟着变。两者一分叉，
  //   滞回就一直拿着那个夹之前的数判"盖得住"，窗口于是永久停住 —— 症状是连续粘贴把
  //   文档撑长之后，容器高度一路涨到几十页、末尾那一段永远没有画布（TRAPS 186）。
  //   windowFirstPage / windowPageCount 是上一次 present 的落点，正是"现有窗口"本身。
  // 仍然不许再加"现有窗口不能超过总页数"那一条：装载时先把窗口钉成 (0, 上限) 再排版，
  //   而单页文档的总页数是 1 ⇒ 多那一条会把"钉好的窗口"判成换窗，于是全量重排之外
  //   再 present 一次（那次只重画一个光标），首次全量那笔账被冲掉。
  // 判据取"要求"（加提前量）而不是"目标窗口"：目标带余量还随可见页挪，
  //   拿它当判据等于要求窗口永远精确居中，一滚就动、滞回形同虚设。
  const curFirst = mod.windowFirstPage();
  const curCount = mod.windowPageCount();
  if (curCount > 0 && curFirst <= needFirst && needLast <= curFirst + curCount - 1) {
    return false;   // 现有窗口还盖得住
  }
  const w = targetWindow(req, total, maxWindowPages());
  const key = w.first + ':' + w.count;
  if (key === lastAnchor) return false;
  mod.setViewportAnchor(w.first, w.count);
  lastAnchor = key;
  return true;
}

// 呈现收尾

/**
 * 收尾：必要时呈现一次，然后把脏页重贴上去，返回这一拍的账。
 *
 * 呈现的条件是"引擎没画（见 PRESENT_ALWAYS）或窗口换了" —— 与 app.js 的 settle 同一
 *   口径（窗口没变就不 present）。setFontCacheConfig / setUndoLimit 这类根本不碰帧的
 *   运算，两次条件都不成立，于是连 present 都不做。
 */
// 自己钉锚点的运算：不要再让 retarget 按可见范围改回来（见 WRITE.viewport 的说明）。
const KEEP_ANCHOR = new Set(['viewport']);

function render(opName: any) {
  let st = JSON.parse(mod.editStatsJson());
  const moved = KEEP_ANCHOR.has(opName) ? false : retarget(st);
  if (moved || PRESENT_ALWAYS.has(opName)) {
    mod.present();
    st = JSON.parse(mod.editStatsJson());
  }
  // 窗口换了要整窗重贴；没换就只贴脏页 —— 这正是"一页一张画布"省下来的那部分。
  // 窗口换了：blitPages 自己会看槽过期（新进窗口的页）与还欠着的脏矩形，
  //   这里不再替它挑页 —— 那两件事的判据在引擎与槽表里，宿主重算一遍只会不同步。
  //   内容整体作废（换缩放/换页度量/换文档）不在这里判 —— paintFull 已经把槽表全置空，
  //   于是这一趟无论传什么都会整窗重贴。
  host.blitPages();
  return st;
}

/**
 * 回话的打包成本（构造那份状态对象 + 结构化克隆），随每一份状态带回宿主。
 *
 * 它是"往返里没算进 work 的那一段"里最容易被忽略的一块：work 只从消息进来到
 *   present/blit 结束为止，而一份状态里有整段文本、控件、选区、光标报告、排版账，
 *   构造它们与克隆它们都在这之后。收与发都在 Worker 这边，所以量在这里最准。
 */
let packSum = 0, packMax = 0, packN = 0;
const packAvg = () => (packN ? packSum / packN : 0);

/**
 * 全文缓存。
 *
 * 为什么不每拍都算一遍 documentTextAll：它是 O(文档)，而状态每拍都推（光标移动也算）。
 *   变了的那些拍（编辑、装载）由 changed 与 paintFull 刷新，其余拍直接带缓存。
 */
let textAllCache = '';

function sendState(ts: number, label: string, stats: any, changed?: boolean, workMs?: number) {
  const tPack = performance.now();
  if (changed) textAllCache = mod.documentTextAll();
  const blit = host.blitStats();
  post({
    t: 'state', ts: ts || 0, label: label || '',
    stats, geom: geom(stats),
    scrollReset,
    // 这一下在 Worker 里花了多少毫秒。只有视口那条路会填；0 = 与视口无关的一份状态
    //   （编辑、装载、画布到达）。宿主拿它和往返比（见 host.viewDiag）。
    workMs: workMs || 0,
    // 打包那一段（构造 + 克隆）的近期均值与峰值。宿主只读不判 —— 它只为把
    //   "往返减 work"那段缺口拆成两半，好知道时间花在打包上还是花在路上。
    packAvgMs: packAvg(), packMaxMs: packMax,
    // 这一拍重贴了几页画布、传了多少像素、花了多少毫秒。挪窗时贴的是全窗，而真正需要
    //   新像素的只有新露出来那一页 —— 其余页的画布在文档流里位置没动、像素本来就是对的。
    //   px 是"实际 putImageData 的面积"：按脏矩形贴之后它应该比 pages × 页面积小两三个
    //   数量级（光标移动一次几十像素），这是"上屏没有整页搬"的唯一直接证据。
    blitMs: blit.ms, blitPages: blit.pages, blitPx: blit.px,
    // 这一下到底有没有真的改动。宿主门面的契约是"没改 => 返回 null"（对照单线程宿主
    //   app.js 里 if (!mod.X()) return null; return this.present();），
    //   引擎那一步的返回值就是它，必须一路带到主线程。
    changed: !!changed,
    heapMB: mod.HEAPU8.length / 1048576,
    fontMode,
    // 顺带把主线程要"同步读"的那几样一起推过去 —— 它们都很小，但每拍去 ask 一次
    //   就是一次往返（200 µs）。推一次让宿主侧的门面能保持同步读的形态。
    text0: mod.documentText(0),
    // 整篇文本：主线程的 text() / paragraphs() 是同步读，不能在读的那一刻再问一次。
    textAll: textAllCache,
    controls: JSON.parse(mod.controlsJson()),
    blockedEdits: mod.blockedEdits(),
    dirtyPages: JSON.parse(mod.dirtyPagesJson()),
    caretReport: mod.caretReport(),
    // 选区取证（两端全局流位置 + 页面坐标矩形）。与 caretReport 同档：小字符串，
    //   随 state 推过去，主线程的 host.selection 于是仍是同步读。
    //   两端同时推一份"段内位置"：对外 API 的 Pos 是 {段号, 段内偏移}，而这个换算
    //   只有引擎会（段间分隔符算不算一个位置是引擎的口径），主线程不该拿全文去猜。
    selection: (() => {
      const sel = JSON.parse(mod.selectionReport());
      sel.anchorPos = READ.flowTextPos({ flow: sel.anchor });
      sel.focusPos = READ.flowTextPos({ flow: sel.focus });
      return sel;
    })(),
    // 引擎实时光标偏移（直读，不经过 editStats 那本账）。宿主拿它和 editStats 里的
    //   光标位置比，就能判出"有没有哪个改了状态的入口忘了给自己收尾"。
    caretLive: mod.caretOffsetLive(),
    // 键位映射要同步读这两个：Worker 少一次往返，方向键就不会按错分支。
    caretVertical: !!mod.caretVerticalNow(),
    caretBlockReversed: !!mod.caretBlockReversedNow(),
    // 排版那本账（statsJson）与编辑那本账（editStatsJson）在单线程宿主上是两个入口，
    //   上面那个 stats 走的是编辑那本（宿主的内部逻辑要它：光标矩形、脏页、重画像素）。
    //   这里必须把排版那本也推过去 —— 否则主线程的 stats 会与 editStats 指向同一个对象，
    //   页数 / 字形 run 数 / 分阶段形状耗时这些字段读出来全是 undefined。
    //   undefined 不会让任何断言变红，只会让报告里少几列：那是查错方向都定不下来的那种错。
    layout: JSON.parse(mod.statsJson()),
    // 样式现状。与 selection 同档：小字符串，随 state 推过去，主线程读它是同步的 ——
    //   面板初始化控件要用它，而默认值只在 EngineConfig 里有一份。
    caretStyle: JSON.parse(mod.caretStyleJson()),
    selectionStyle: JSON.parse(mod.selectionStyleJson()),
  });
  const pack = performance.now() - tPack;
  packSum += pack;
  ++packN;
  if (pack > packMax) packMax = pack;
  scrollReset = false;   // 一次性信号：随这一份状态送出去就清掉
}

function push(ts: any, opName: any, changed: any) { sendState(ts, opName, render(opName), changed); }

// 装载

/**
 * 全量重排重绘的公共收尾。
 *
 * present 只画不排 —— 装载之后排版一次都没跑过时，present 出来是一片空白（首帧 ink 为 0、
 * totalPages 恒为 1；编辑走 *AndRender 版本会自己收尾，所以只有首帧露）。
 *
 * 锚点必须先钉住、再排 —— 与单线程宿主的 presetWindowForLoad 同一口径。锚点也不能事后留空：
 * 留空会让随后的 retarget 把"窗口从无到有"判成换窗，于是在全量之外再 present 一次 ——
 * 后一次只重画一个光标，首次全量那笔账（探针的分母）就被冲掉了。
 */
function paintFull() {
  // 换文档、换缩放、换页度量、重设窗口锚点：页像素尺寸或页面内容整体变了，
  //   每个槽里装的都可能是上一份的像素，所以槽表整体作废、这一趟整窗重贴。
  // 这一条必须显式：paintFull 自己先把窗口锚点设好了，随后 render() 里的 retarget
  //   会判"窗口没动"。按页号建销的那一版里，这一趟总有一批新画布陆续到达、各自把
  //   那一页贴一次，于是"整窗都换过页"被顺带补齐了；改成按槽复用之后画布不再新建，
  //   那条隐式触发就没了 —— 表现为切字体之后画布还留着上一窗的像素，而帧是对的
  //   （自审逐像素比对一比就露）。所以这件事必须由上面说出来，不能指望下游有谁代劳。
  host.invalidateSlots();
  scrollY = 0;
  // 告诉宿主"引擎把滚动位置归零了"。必须是显式信号，不能让宿主那边
  //   用"两边不一致就追平"来猜：追平要写 DOM 的 scrollTop，而写它会触发 scroll
  //   事件、再发一条视口消息回来 —— 与正在滚动的用户（或外部脚本）来回打架，
  //   窗口跟着反复变、画布反复建与丢。实测症状：跨页滚动后某一页始终没有画布。
  scrollReset = true;
  scrollResetHost = true;
  const n = maxWindowPages();
  mod.setViewportAnchor(0, n);
  lastAnchor = '0:' + n;
  mod.relayoutAndPaint();
  // 换过文档了：全文缓存必须跟着换，否则主线程读到的是上一份文档的正文。
  textAllCache = mod.documentTextAll();
}

function loadScenario(id: any, zoom: any) {
  if (zoom) { mod.setZoom(zoom); zoomLevel = zoom; }
  mod.setScenario(id);
  paintFull();
}

/**
 * 换块流之后的统一收尾（与 loadScenario 同理，只是入口在别处）。
 *
 * 锚点必须先钉住、再排 —— 与单线程宿主的 presetWindowForLoad 同一口径。
 *   这里若图省事写 lastAnchor = ''，retarget 会把"窗口从无到有"判成换窗，
 *   于是在 relayoutAndPaint 的全量之外再 present 一次 —— 而那次只重画一个光标，
 *   首次全量那笔账（探针的分母）就被冲掉了。
 */
function loadBlocks(fn: any) {
  fn();
  paintFull();
  // 必须回一个"有改动"：装载命令换的是整份文档，而调用方（库的 edit()）按这个布尔
  //   决定要不要走收尾 —— 回 undefined 的后果是"画面换了、收尾没跑"（滚动位置不追平、
  //   光标事件不发），屏幕上看着正常，读数全停在上一份文档上。
  return true;
}

/**
 * 胶水与 wasm 这两个资源的传输字节数（口径与单线程宿主的 wasmBytes() 逐条相同）。
 *
 * 必须在 Worker 这一侧量：胶水（importScripts）与 wasm（胶水自己去取）都是这一侧发出的，
 *   主线程的 performance 资源表里没有它们 —— 在那边量恒为 0，而 0 会安安静静地进报告，
 *   没人会怀疑一个数字。
 */
function glueBytesInWorker() {
  let n = 0;
  for (const e of performance.getEntriesByType('resource') as any[]) {
    if (e.name.includes('heditor')) n += e.transferSize || e.decodedBodySize || 0;
  }
  return n;
}

async function boot(m: any) {
  const t0 = performance.now();
  // 胶水怎么加载由宿主决定：Worker 侧是 importScripts，主线程侧是 <script>。
  //   这条差异属于"传输"，不属于引擎 —— 放在这里会让同一份代码没法跑在主线程。
  const factory = await host.loadGlue(m.glue, m.exportName || 'CreateHEditor');
  if (typeof factory !== 'function') {
    throw new Error('胶水层没导出 ' + (m.exportName || 'CreateHEditor'));
  }
  const wasmPath = String(m.wasm || '');
  const base = m.glue.replace(/[^/]*$/, '');
  // bust 一路带到 wasm：只给胶水加戳、不给 wasm 加，就会出现"新胶水配旧 wasm"。
  // wasm 可以显式给（assets.wasm）：开发服务器的根不是仓库根时，胶水按自己的目录推会落空。
  mod = await factory({
    locateFile: (p: string) =>
      wasmPath && /\.wasm$/.test(p) ? wasmPath : base + p + (m.bust || ''),
  });
  dpr = m.dpr || 1;
  viewW = m.viewW || 1;
  viewH = m.viewH || 1;
  fontList = m.fonts || [];
  await installFonts(m.fontMode || 'full');
  // 先把 ready 报出去，后面的装载失败也看得见 —— 顺序反了的话，装载里一抛异常
  //   主线程就永远等不到 ready，表现成"整个页面静默挂住"，是最难查的那种形态。
  bootMs = performance.now() - t0;
  post({
    t: 'ready', bootMs, fontMs, fontBytes, fontMode,
    glueBytes: glueBytesInWorker(),
    heapMB: mod.HEAPU8.length / 1048576,
  });
  try {
    // 页度量要先设再装载：它会让整篇重排，排在装载之前只会排这一次。
    mod.setPageMetrics(kPageWidthPt, kPageHeightPt, kPageMarginPt);
    // 缩放也要先设：loadScenario 收 zoom 参数，而 empty 那条路不走它 —— 漏设就会
    //   按 EngineConfig 的默认密度排（1.5 倍），页面比请求的大一整档。
    if (m.zoom) {
      mod.setZoom(m.zoom);
      zoomLevel = m.zoom;
    }
    // empty = 库要的初始状态：一份只有一个空段落的文档，不带演示内容。
    if (m.empty) loadBlocks(() => mod.loadTextDocument('', 1));
    else loadScenario(m.scenario === undefined ? 6 : m.scenario, m.zoom);
    // 首帧的几何先报出去，宿主才能按窗口把每页画布建出来并交给这一侧。
    //   这时候还没有画布，所以只推几何；画布到一张贴一张（见 pageCanvas 分支）。
    sendState(0, 'boot', JSON.parse(mod.editStatsJson()));
  } catch (e) {
    err(e, 'boot:layout');
  }
}

// 运算表

// 改状态的运算。跑完一律收尾（present + 重贴 + 回推 state）。
const WRITE = collect('w', {
  // 什么都不改，只收尾。给宿主的 present() 用 —— 它就是"重画并回推"。
  present: () => {},
  scenario: (a) => loadScenario(a.id, a.zoom),
  reset: (a) => loadScenario(a.id === undefined ? 6 : a.id, a.zoom === undefined ? kZoomBase : a.zoom),
  zoom: (a) => {
    mod.setZoom(a.zoom);
    zoomLevel = a.zoom;
    // 屏幕像素比会随窗口换显示器而变，宿主每次改缩放都捎一份新的过来。回推的状态里带着
    //   它，宿主拿它折算页画布的 CSS 尺寸 —— 不回传就会按旧值折算，显示尺寸与背衬差一档。
    if (a.dpr) dpr = a.dpr;
    paintFull();
  },
  stress: (a) => loadBlocks(() => mod.loadStressDocument(a.paras | 0, a.chars | 0)),
  stressDiverse: (a) => loadBlocks(() => mod.loadStressDocumentDiverse(a.paras | 0, a.chars | 0)),
  textDoc: (a) => loadBlocks(() => mod.loadTextDocument(a.text, (a.repeat | 0) || 1)),
  formSample: (a) => loadBlocks(() => mod.loadFormSample(a.variant | 0)),
  /**
   * 打开一份保存过的文档（JSON 文本）。
   *
   * 走 loadBlocks 而不是普通 present：换的是整份文档 —— 页数、页度量、窗口锚点全变了，
   *   与 textDoc / stress 同一档。失败时引擎一个字节都没动，所以这里直接抛给宿主显示，
   *   不 paintFull（画面上仍是失败前那份，什么都不用做）。
   */
  loadDocument: (a) => loadBlocks(() => {
    const err = mod.loadDocument(a.text || '');
    if (err) throw new Error(err);
  }),
  fonts: async (a) => { await installFonts(a.mode || 'full'); paintFull(); },

  insertText: (a) => mod.insertTextAt(a.para | 0, a.offset | 0, a.s),
  type: (a) => mod.typeText(a.s),
  /**
   * 粘贴：片段（自家格式）与纯文本各一条，两条都自带重排与呈现。
   *
   * 失败时引擎回一句给人看的原因（不是异常）：片段可能来自旧版本、也可能被别的应用
   *   改坏，这属于用户输入，不是本层的 bug。原因原样回给调用方。
   */
  paste: (a) => {
    const why = mod.pasteFragment(a.s || '');
    if (why) throw new Error(why);
    return true;
  },
  pasteText: (a) => {
    const why = mod.pastePlainText(a.s || '');
    if (why) throw new Error(why);
    return true;
  },
  deleteSelection: () => mod.deleteSelection(),
  backspace: () => mod.backspace(),
  deleteForward: () => mod.deleteForward(),
  splitParagraph: () => mod.splitParagraph(),
  mergePrev: () => mod.mergeWithPreviousParagraph(),
  mergeNext: () => mod.mergeWithNextParagraph(),
  move: (a) => mod.moveCaret(a.dcp | 0, !!a.extend),
  /**
   * 上下键（dir < 0 上 / > 0 下）与文档首尾。extend = Shift 扩展（锚点不动）。
   *   与 move 同一档：引擎的 *AndRender 版自己收尾，这里不额外 present。
   */
  vertical: (a) => mod.moveCaretVertical(a.dir | 0, !!a.extend),
  docStart: (a) => mod.caretToDocStart(!!a.extend),
  docEnd: (a) => mod.caretToDocEnd(!!a.extend),
  /**
   * 选区三个入口。两端是全局流位置（与事务里那份快照同一把尺子），不是段内偏移。
   */
  selectAll: () => mod.selectAll(),
  // 整格选区：段号来自 hit 的 cellPara 字段（引擎的几何结论），宿主原样搬回来。
  selectCellAtPara: (a) => mod.selectCellAtPara(a.para | 0),
  // 三击：按点选整格。点不在表格里返回 false，宿主退回落光标。
  //   必须留在这里（命令表）：它会重画帧，而查询那条路不重贴脏页 —— 画面要等下一次
  //   闪烁相位才更新，那就是"点了要等一秒才选中"。
  cellSelectAt: (a) => mod.cellSelectAt(a.x, a.y),
  // 外部 API 收 {段号, 段内偏移}；换算成全局流位置只在引擎里做（见 flowIndexAt）。
  selectPos: (a) =>
    mod.setSelection(
      mod.flowIndexAt(a.anchorPara | 0, a.anchorOffset | 0),
      mod.flowIndexAt(a.focusPara | 0, a.focusOffset | 0),
    ),
  setSelection: (a) => mod.setSelection(a.anchor | 0, a.focus | 0),
  clearSelection: () => mod.clearSelection(),
  // 落光标的三个入口都要无条件把相位拉回可见，与单线程宿主的 setCaretAndPresent
  //   逐字对齐（那边就是 mod.setCaret(...); caretOn = true; mod.showCaret(true);
  //   mod.present();）。漏掉的症状：相位停在隐藏那半拍时落一次光标，光标不出现 ——
  //   而这正是"点画布换字段后光标看不见"那个用户 bug 的一半。
  setCaret: (a) => { mod.setCaret(a.para | 0, a.offset | 0); mod.showCaret(true); },
  setCaretAt: (a) => {
    mod.setCaretAt(a.para | 0, a.offset | 0, a.field | 0);
    mod.showCaret(true);
  },
  setCaretAtSide: (a) => {
    mod.setCaretAtSide(a.para | 0, a.offset | 0, a.side | 0);
    mod.showCaret(true);
  },
  // 带行别的版本：折行处"上一行行尾"与"下一行行首"是同一个偏移与侧别，
  //   只有行别分得开（点击落点走它，见 EditorEngine::caretLineAffinity）。
  setCaretAtSideLine: (a) => {
    mod.setCaretAtSideLine(a.para | 0, a.offset | 0, a.side | 0, a.line | 0);
    mod.showCaret(true);
  },
  setFocusAtSideLine: (a) => {
    mod.setFocusAtSideLine(a.para | 0, a.offset | 0, a.side | 0, a.line | 0);
    mod.showCaret(true);
  },
  // 拖选的移动端：只搬焦点端，anchor 不动。与 setCaretAtSide 同档 ——
  //   引擎不自己重绘，所以它也在 PRESENT_ALWAYS 里（否则拖动时选区不更新）。
  setFocusAtSide: (a) => {
    mod.setFocusAtSide(a.para | 0, a.offset | 0, a.side | 0);
    mod.showCaret(true);
  },
  indent: () => mod.indent(),
  outdent: () => mod.outdent(),
  tabCaret: (a) => mod.tabCaret(!!a.backward),
  lineStart: (a) => mod.caretToLineStart(!!a.extend),
  lineEnd: (a) => mod.caretToLineEnd(!!a.extend),
  undo: () => mod.undoEdit(),
  redo: () => mod.redoEdit(),
  setUndoLimit: (a) => mod.setUndoLimit(a.n | 0),

  setEditMode: (a) => mod.setEditMode(a.mode | 0),
  setShowBrackets: (a) => mod.setShowBrackets(!!a.on),
  setShowControlDots: (a) => mod.setShowControlDots(!!a.on),
  /**
   * 光标与选区的样式。缺的字段按现状补 —— 与单线程宿主门面同一套补全规则，
   *   两处不能各写一套（面板只改颜色时，宽度必须原样保留）。
   */
  caretStyle: (a) => {
    const s = a.style || {};
    const cur = JSON.parse(mod.caretStyleJson());
    const c = s.color || cur.color;
    mod.setCaretStyle(c[0] | 0, c[1] | 0, c[2] | 0, (c[3] === undefined ? 255 : c[3]) | 0,
                      s.widthPt === undefined ? cur.widthPt : Number(s.widthPt));
  },
  selectionStyle: (a) => {
    const s = a.style || {};
    const cur = JSON.parse(mod.selectionStyleJson());
    const c = s.color || cur.color;
    mod.setSelectionStyle(c[0] | 0, c[1] | 0, c[2] | 0, (c[3] === undefined ? 255 : c[3]) | 0);
  },
  /**
   * 换字段名单。必须重排，不能只 present —— 括号标记（含只读的灰括号）是排进
   *   布局结果里的，只 present 的话标记还是上一轮的：症状是"控件清单里 ro=true，
   *   可槽清单里 ro 全是 false"（一条自相矛盾的读数，很难往"没重排"上想）。
   *   对照单线程宿主的 setFormControls：它就是 return relayoutAndBlit();。
   */
  setFormControls: (a) => {
    mod.setFormControls((a.list || []).map((it: any) => ({
      start: it.start, end: it.end, kind: it.kind || 0, id: it.id || '', ro: !!it.ro,
    })));
    paintFull();
  },
  /**
   * 闪烁那一拍：摆相位并呈现（对照单线程宿主 tick 里的 showCaret + present）。
   *   它必须是"会收尾"的那一类 —— 闪烁要在屏幕上看得见，就得重画。
   *   而门面的 showCaret / stopBlink 走的是 SILENT 那张表（只改状态不呈现），
   *   两者不能混：混了就会把探针的"全量基线"（分母）覆盖掉。
   */
  blinkPhase: (a) => mod.showCaret(!!a.on),
  focusControl: (a) => mod.focusControl(a.forward === undefined ? true : !!a.forward),
  removeControl: (a) => mod.removeControlAt(a.i | 0, !!a.keepContent),
  setControlProp: (a) => mod.setControlPropAt(a.i | 0, a.prop | 0, !!a.value),
  setFontCacheConfig: (a) => mod.setFontCacheConfig(a.blockSize | 0, a.blocks | 0),

  /**
   * 视口锚点：与单线程宿主的门面 setViewportAnchor 逐字对齐 —— 直接钉锚点（不碰滚动
   *   位置），并且必须呈现。
   * 也不能让它走 retarget：retarget 会按"滚动位置算出来的可见页"把锚点又挪回可见范围。
   *   探针靠这个入口把窗口挪到前/中/后去量按需读的覆盖面 —— 被挪回去就会量到别的页，
   *   而且是静默的（数字看着正常，只是不是那一页）。
   */
  viewport: (a) => {
    mod.setViewportAnchor(a.first | 0, a.count | 0);
    lastAnchor = (a.first | 0) + ':' + (a.count | 0);
  },
});

/**
 * 只改引擎状态、不做收尾的运算（不回推 state、不重画）。
 *
 * 为什么需要这一档：单线程宿主的 stopBlink() 与门面 showCaret() 都只调 mod.showCaret，
 *   不 present —— 那边注释写得很清楚："present 会把 edit_stats_ 覆盖成'只重画了个
 *   光标'，而探针紧接着要读的往往是'上一次全量呈现'的数字（分母）。停表就只停表。"
 *   搬进 Worker 之后若把这两个入口也走统一收尾，基线就被无声地换掉了：一堆增量断言会以
 *   错误的分母照绿（这是最难发现的一类失真）。
 * ⇒ 门面 showCaret / stopBlink 走这张表；闪烁那一拍走 WRITE.blinkPhase（那个要呈现）。
 */
const SILENT = collect('s', {
  phase: (a) => mod.showCaret(!!a.on),
});

// 要返回值的查询。
const READ = collect('r', {
  /**
   * 排空屏障：什么都不算，立刻回一个值。
   *
   * 用途是给宿主一个"顺序保证"—— postMessage 保序，所以这条 reply 回来时，它之前
   *   发出的消息都已经处理完了。这是探针的同步点（交互之后要读到那一拍的结果）。
   * 它刻意不碰 mod、也不走 push：哨兵不能有副作用（一次 present 就会改变帧，
   *   把"某个入口忘了收尾"这类断言直接盖掉）。
   */
  ping: () => 1,
  /**
   * 一页的脏矩形（页内像素坐标），读后清零。
   *
   * 5B 的按页接口：宿主按它决定 putImageData 哪几块。光标移动只应该给出两三个小矩形，
   *   而不是整页 —— 那一条的判据在 edit_probe / edit_test [58]。
   */
  pageDirtyRects: (a) => JSON.parse(mod.pageDirtyRectsJson(a.page | 0)),
  /**
   * 还欠着脏矩形的页号。宿主按它决定贴哪几页 —— 不能用 dirtyPages：
   *   那一份是"本拍写过哪几页"，而矩形是累积的，两者不同步就会永久漏贴几块。
   */
  pageDirtyPages: () => JSON.parse(mod.pageDirtyPagesJson()),
  text: (a) => mod.documentText(a.i | 0),
  // 整篇一次拼好：宿主每拍都要一份全文（change 事件带它），逐段取是 N 次跨界。
  textAll: () => mod.documentTextAll(),
  /**
   * 保存：当前文档 → JSON 文本。空串 = 没装载文档（引擎只有这一种失败）。
   *
   * 走查询而不是命令：它不改任何状态，也不该触发一次呈现 —— 保存不该有副作用。
   */
  serialize: () => mod.serializeDocument(),
  /**
   * 复制选区：回一份剪贴板载荷（{"f": 片段, "t": 纯文本} 或 {"e": 原因}）。
   *
   * 走查询而不是命令：复制不改文档、也不该触发一次呈现 —— 与保存同一条道理。
   *   片段的格式只有引擎知道，这一层原样搬字节，不解析。
   */
  copy: () => mod.copySelection(),
  /**
   * 整帧里"非白"的像素数。主线程读不到画布（所有权在 Worker 这边），所以"帧上到底
   *   有没有字"这个判据只能在这里数 —— 与单线程版探针的口径一致（任一通道 < 200 算一个）。
   */
  ink: () => {
    const v = mod.frameRgba();
    let n = 0;
    for (let i = 0; i + 3 < v.length; i += 4) {
      if (v[i] < 200 || v[i + 1] < 200 || v[i + 2] < 200) ++n;
    }
    return n;
  },
  /**
   * 逐行的墨迹与底色，按文档顺序给出窗口内每一条带的那一段。
   *
   * 判据用它回答两个问题，都是"连页到底连不连"的直接读数：
   *   一、条带里有没有"不是白底"的行 —— 有就是还有页间隙或页边框（分页的痕迹）。
   *   二、相邻条带的文字行距是不是与条带内部一样 —— 不一样就是内容在边界上断开了。
   * 为什么在 Worker 侧数：帧的所有权在这一侧（主线程拿不到画布）。这是测量，
   *   走的是与绘制不同的代码路径（只读帧、不做任何布局判断），不会自我循环验证。
   */
  inkRows: () => {
    const f = mod.frameRgba();
    const fw = mod.frameWidth();
    const ph = mod.pagePxHeight();
    const step = ph + mod.pageGapPx();
    const first = mod.windowFirstPage();
    const count = mod.windowPageCount();
    const out = [];
    for (let p = first; p < first + count; ++p) {
      // 导出图里第 k 页（相对窗口首页）落在 k x 步距 —— 槽那套编号已经删掉。
      const base = (p - first) * step;
      const ink = [];
      const sel = [];
      const selDeep = [];
      let gray = 0;
      for (let y = 0; y < step; ++y) {
        const off = (base + y) * fw * 4;
        if (off + fw * 4 > f.length) { ink.push(-1); sel.push(-1); selDeep.push(-1); continue; }
        // 左边那一列（x = 1）是不是白：不是就是页间隙或页边框。
        const l = off + 4;
        if (f[l] < 250 || f[l + 1] < 250 || f[l + 2] < 250) ++gray;
        // 暗像素 = 字；偏蓝的浅色 = 选区（默认选区色混白之后约 228/241/255，
        //   而背景灰 238/240/243 的蓝红差只有 5，页边框 206/212/218 是 12 —— 取 15 分得开）。
        let n = 0;
        let s = 0;
        let deep = 0;
        for (let x = 0; x < fw; x += 2) {
          const i = off + x * 4;
          if (f[i] < 128 && f[i + 1] < 128 && f[i + 2] < 128) ++n;
          else if (f[i + 2] - f[i] > 15 && f[i] > 150) {
            ++s;
            // "深"的选区像素：混一次应当是 228/241/255（红 228），叠加多次会往 178 走。
            //   这一格是"选区被反复混色"的直接读数（见 engine_paint 里 sel_paint 的说明）。
            if (f[i] < 205) ++deep;
          }
        }
        ink.push(n);
        sel.push(s);
        selDeep.push(deep);
      }
      out.push({ page: p, step, ink, sel, selDeep, gray });
    }
    return { first, count, step, ph, slabs: out };
  },
  /**
   * 某几段的行盒与字形（诊断"字画到哪儿去了"）。见 wasm 侧 linesJson 的说明。
   */
  lines: (a) => JSON.parse(mod.linesJson(a.from | 0, a.to | 0)),
  /**
   * 帧的指纹（FNV-1a，只算 RGB 三通道）。
   *
   * 判据用它回答"同一份逻辑状态重画一次，像素是不是一个字都没变" —— 增量绘制最容易犯的
   *   错是"重画时把叠加式的效果又叠了一遍"（选区混色就是这么变深的）。整帧搬回去比太贵
   *   （一份就是窗口像素 x 4，dpr 2 下可达 80 MB），指纹在这一侧算完只回一个数。
   */
  frameDigest: () => {
    const v = mod.frameRgba();
    let h = 2166136261;
    for (let i = 0; i + 3 < v.length; i += 4) {
      h ^= v[i]; h = Math.imul(h, 16777619);
      h ^= v[i + 1]; h = Math.imul(h, 16777619);
      h ^= v[i + 2]; h = Math.imul(h, 16777619);
    }
    return h >>> 0;
  },
  // 帧与页的几何读数：宿主排版、滚动条与自检都要用。都是标量，跨传输安全。
  frameWidth: () => mod.frameWidth(),
  frameHeight: () => mod.frameHeight(),
  frameByteSize: () => mod.frameByteSize(),
  pagePxWidth: () => mod.pagePxWidth(),
  pagePxHeight: () => mod.pagePxHeight(),
  pageGapPx: () => mod.pageGapPx(),
  windowFirstPage: () => mod.windowFirstPage(),
  windowPageCount: () => mod.windowPageCount(),

  // 光标与选区的即时读数。它们不经过 edit_stats 那本账，拿它当分母会读错。
  caretOffsetLive: () => mod.caretOffsetLive(),
  caretVerticalNow: () => mod.caretVerticalNow(),
  caretBlockReversedNow: () => mod.caretBlockReversedNow(),
  selectionReport: () => JSON.parse(mod.selectionReport()),
  // 流位置 -> 段内位置（选区两端的对外形状）。
  flowTextPos: (a) => JSON.parse(mod.flowTextPosJson(a.flow | 0)),
  // 样式现状：写入口是 caretStyle / selectionStyle（缺字段按现状补），读是这两条。
  caretStyleJson: () => JSON.parse(mod.caretStyleJson()),
  selectionStyleJson: () => JSON.parse(mod.selectionStyleJson()),

  // 构建与后端标识：诊断面板与探针报告要用。
  buildStamp: () => (typeof mod.buildStamp === 'function' ? mod.buildStamp() : ''),
  rasterBackend: () => (mod.rasterBackend ? mod.rasterBackend() : '?'),

  stats: () => JSON.parse(mod.statsJson()),
  editStats: () => JSON.parse(mod.editStatsJson()),
  caretReport: () => mod.caretReport(),
  hit: (a) => JSON.parse(mod.hitTest(a.x, a.y)),
  controlAt: (a) => mod.controlAtPoint(a.x, a.y),
  describeControls: () => mod.describeControls(),
  controls: () => JSON.parse(mod.controlsJson()),
  bracketSlots: () => JSON.parse(mod.bracketSlotsJson()).slots,
  blockedEdits: () => mod.blockedEdits(),
  prewarmPage: (a) => mod.prewarmPageGlyphs(a.page | 0),
  dirtyPages: () => JSON.parse(mod.dirtyPagesJson()),
  editMode: () => mod.editMode(),
  showBrackets: () => mod.showBrackets(),
  showControlDots: () => mod.showControlDots(),
  undoLimit: () => mod.undoLimit(),
  auditFrame: () => JSON.parse(mod.auditFrame()),
  auditBase: () => JSON.parse(mod.auditBase()),
  // 页自审：第 p 页的缓存 vs 从零光栅。帧与底图两条自审读的都是缓存，
  //   缓存本身写错时它们照样全绿 —— 这一条是那一层的唯一参照物。
  auditPage: (a) => JSON.parse(mod.auditPage(a.page | 0)),
  auditLayout: () => (mod.auditLayout ? JSON.parse(mod.auditLayout()) : null),
  auditCompile: () => (mod.auditCompile ? JSON.parse(mod.auditCompile()) : null),
  auditGlyphPages: () => (mod.auditGlyphPages ? JSON.parse(mod.auditGlyphPages()) : null),
  bench: (a) => JSON.parse(mod.benchRelayoutPaint(a.runs || 12)),
  heap: () => mod.HEAPU8.length / 1048576,

  /**
   * 自检：把每页画布上真正的像素与帧里对应的一段逐点比。
   *
   * 返回形状必须与单线程宿主的 selfCheck 一致 —— 它是给用户的取证入口
   *   （F12 里 __heditor.selfCheck() 把对象发出来），也是探针的主判据：
   *   screenVsEngineDiffPx > 0 说明"上屏"贴错了，恒为 0 说明问题在引擎的排版与光栅化。
   *   形状不一致的代价不是"少几个字段"：探针读 sc.framePx[0] 会直接抛，
   *   整段断言连跑都跑不到（表现为一个 TypeError，看不出是这个原因）。
   * 多出来的 compared / ratio / missing / sizeBad 是本侧扩展：缺页与尺寸不符单独报数 ——
   *   "有页还没拿到画布"和"画错了"是两件事，混在一起会让人查错方向。
   */
  selfCheck: () => {
    // 先把欠着的上屏贴完，再比。否则比的是"帧已经前进了一拍、画布还停在上一拍"，
    //   而那不是贴图逻辑的错 —— 是两次操作之间的时序（闪烁拍 present 之后、blit 之前）。
    //   诊断接口要回答的是"贴得对不对"，不是"凑不凑巧"。
    host.blitPages();
    const fw = mod.frameWidth();
    const fh = mod.frameHeight();
    const pw = mod.pagePxWidth();
    const ph = mod.pagePxHeight();
    const first = mod.windowFirstPage();
    const count = mod.windowPageCount();
    let diff = 0;
    let compared = 0;
    let missing = 0;
    let sizeBad = 0;
    let comparedPages = 0;
    let diffPage = -1;
    let minx = 1e9;
    let miny = 1e9;
    let maxx = -1;
    let maxy = -1;
    // 一页一张画布，源就是这一页自己的导出像素（内容 + 瞬态）。
    //   页摆在哪儿是宿主的账（页号 x 步距），与画布里的像素无关 —— 所以这里没有槽、
    //   没有偏移，比的是同一页的同一块：任何差额都只能是"贴错了"。
    for (let p = first; p < first + count; ++p) {
      const c = host.ctxOfPage(p);
      if (!c) { ++missing; continue; }
      if (c.canvas.width !== pw || c.canvas.height !== ph) { ++sizeBad; continue; }
      const src = mod.pageRgba(p);
      if (src.length !== pw * ph * 4) { ++sizeBad; continue; }
      ++comparedPages;
      const D = c.getImageData(0, 0, pw, ph).data;
      compared += pw * ph;
      for (let i = 0; i + 3 < D.length; i += 4) {
        if (D[i] === src[i] && D[i + 1] === src[i + 1] && D[i + 2] === src[i + 2]) continue;
        ++diff;
        if (diffPage < 0) diffPage = p;
        const q = i >> 2;
        const x = q % pw;
        const y = (q - x) / pw;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
      }
    }
    const st = JSON.parse(mod.editStatsJson());
    return {
      text: mod.documentText(0),
      docCodepoints: st.docCodepoints,
      caret: { para: st.caretPara, offset: st.caretOffset, x: st.caretX, y: st.caretY,
               w: st.caretW, h: st.caretH, visible: st.caretVisible, page: st.caretPage },
      framePx: [fw, fh],
      window: { first, count, comparedPages },
      pageCanvases: host.canvasCount(),
      // cssSize / rectPx 要看主线程上的 DOM，由宿主那边补（见 worker_host.js 的 selfCheck 包装）。
      pages: { backing: [mod.pagePxWidth(), ph], cssSize: null, rectPx: null },
      dpr,
      zoom: zoomLevel,
      backend: mod.rasterBackend ? mod.rasterBackend() : '?',
      screenVsEngineDiffPx: diff,
      screenVsEngineDiffBox: diff ? [minx, miny, maxx, maxy] : null,
      repainted: { pages: st.repaintedPages, pixels: st.repaintedPixels,
                   w: st.repaintedW, h: st.repaintedH },
      diff, compared, missing, sizeBad, ratio: compared ? diff / compared : 1,
    };
  },

  /**
   * 每个可编辑字段的左括号在帧上到底画了没有（扫左缘那一列里的"括号蓝"）。
   *   判据与探针一致：偏蓝够远 + 够亮 + 红通道 > 45（把光标色排除掉）。
   *   括号常显之后每个可编辑字段都必须有；"只画聚焦字段"那一档已经删了。
   */
  markCheck: () => {
    const cc = JSON.parse(mod.controlsJson());
    const f = mod.frameRgba();
    const fw = mod.frameWidth();
    const near = (r: number, g: number, b: number) => r > 45 && b - r > 60 && b > 140;
    const rows = [];
    for (let ci = 0; ci < cc.items.length; ++ci) {
      const it = cc.items[ci];
      if (it.ro || !it.rects.length) continue;
      const cx = it.rects[0].x;
      const lo = Math.round((cx - 10) * zoomLevel);
      const hi = Math.round((cx - 1) * zoomLevel);
      let n = 0;
      for (let i = 0; i + 3 < f.length; i += 4) {
        const x = (i >> 2) % fw;
        if (x < lo || x > hi) continue;
        if (near(f[i], f[i + 1], f[i + 2])) ++n;
      }
      rows.push({ control: ci, focused: ci === cc.caret, leftEdgePx: n, ok: n > 0 });
    }
    return {
      mode: cc.mode, showBrackets: mod.showBrackets(), count: cc.count,
      caretControl: cc.caret, zoom: zoomLevel, rows,
      allOk: rows.every((r) => r.ok),
    };
  },
});

/**
 * 每轮换窗之后往前预热一页的字形，且要等 90 ms 没有新的视口消息才动手。
 *
 * 为什么要预热：换窗那一拍要重新光栅新露出来的那几页，而冷字形占那一拍的一大半 ——
 *   同一条路上同一页量到：冷的 14.6 µs·字形、整拍 44.5 ms；预热带过之后 7.0 µs、25.7 ms。
 * 为什么要限量成"一页 + 静下来 90 ms 才跑"：预热是 Worker 这条唯一线程上的活，
 *   成串地跑会把视口消息、编辑命令与宿主的查询全排在后面 —— 实测探针的求值直接超时。
 *   连续快滚时它几乎不跑（那时每一拍都该留给滚动），滚动一停就把下一页先烘上。
 */
const PREFETCH_PAGES = 1;
const PREFETCH_DELAY_MS = 90;
let pfTimer = 0;
let pfNext = -1;
let pfLeft = 0;
let pfDir = 1;

function schedulePrewarm(dir: any) {
  if (!mod) return;
  pfDir = dir < 0 ? -1 : 1;
  pfNext = pfDir > 0 ? mod.windowFirstPage() + mod.windowPageCount() : mod.windowFirstPage() - 1;
  pfLeft = PREFETCH_PAGES;
  if (pfTimer) clearTimeout(pfTimer);
  pfTimer = setTimeout(pumpPrewarm, PREFETCH_DELAY_MS);
}

function pumpPrewarm() {
  pfTimer = 0;
  if (!mod || pfLeft <= 0) return;
  const total = Math.max(1, JSON.parse(mod.editStatsJson()).totalPages);
  if (pfNext < 0 || pfNext >= total) return;
  mod.prewarmPageGlyphs(pfNext);
  pfNext += pfDir;
  --pfLeft;
}

// 消息入口

async function doOp(m: any) {
  // 静默档先查：它只改引擎状态，什么都不回推（对照单线程宿主的 stopBlink/showCaret）。
  const silent = SILENT[m.name];
  if (silent) {
    await silent(m.args || {});
    return void post({ t: 'ack', ts: m.ts });
  }
  const fn = WRITE[m.name];
  if (!fn) throw new Error('未知命令 ' + m.name);
  const ret = await fn(m.args || {});
  return void push(m.ts, m.name, ret);
}

async function askOp(m: any) {
  // rawFrame 要单独走：帧是窗口像素 x 4（dpr 2 下可达 80 MB），按值结构化克隆会白拷
  //   两份；拷到新 buffer 再转移所有权过去是 O(1)。探针要拿它做逐字节比对，所以
  //   必须是一份快照（wasm 堆上那个视图任何一次 present 都会被原地覆盖）。
  if (m.name === 'rawFrame') {
    const v = mod.frameRgba();
    const copy = new Uint8ClampedArray(v.length);
    copy.set(v);
    return void post({ t: 'reply', id: m.id, value: copy }, [copy.buffer]);
  }
  const fn = READ[m.name];
  if (!fn) throw new Error('未知查询 ' + m.name);
  return void post({ t: 'reply', id: m.id, value: await fn(m.args || {}) });
}

/**
 * 视口变化：更新位置、按需换窗、必要时呈现并重贴。
 *
 * 返回 { moved, scrolled, workMs }：moved 表示窗口换了（已经 present + 重贴过），
 *   scrolled 表示滚动位置变了（连页模式下调用方要重贴）。
 */
function view(m: any) {
  const tWork = performance.now();
  const prevScrollY = scrollY;
  scrollY = Math.max(0, m.scrollY || 0);
  scrollLead = Number(m.leadPx) || 0;
  viewW = m.viewW || viewW;
  viewH = m.viewH || viewH;
  // 方向只用来决定余量放前面还是后面（见 targetWindow），得在 retarget 之前定下来。
  if (scrollY !== prevScrollY) scrollDir = scrollY > prevScrollY ? 1 : -1;
  const scrolled = scrollY !== prevScrollY;
  // 窗口没变就什么都不做：画布摆在文档流里，浏览器已经把像素搬走了。
  //   这一条是"滚动不花引擎的钱"的落点 —— 只回一个 ack，不重贴、不推统计。
  //   连页是例外：屏上那一张视口画布不动，页在它上面的落点是随滚动量算出来的，
  //   所以滚动量一变就必须重贴 —— 哪怕窗口一动没动（否则画面停在上一拍的位置，
  //   而那正是"滚动后有的页看着还是旧内容"的来源）。
  if (!retarget(JSON.parse(mod.editStatsJson()))) {
    return { moved: false, scrolled, workMs: performance.now() - tWork };
  }
  mod.present();
  host.blitPages();
  schedulePrewarm(scrollY - prevScrollY);
  // 把这一拍的状态一起带回去：薄壳要拿它回推，而"读一次 editStats 再拼一份 state"
  //   是两件应该留在一侧的事 —— 让薄壳伸手读引擎，那条边界就又有第二个入口了。
  return { moved: true, scrolled, workMs: performance.now() - tWork,
           stats: JSON.parse(mod.editStatsJson()) };
}

/**
 * 视图模式的两个配置：页间隙与连页标志。
 *
 * 它们是"页的落点"这一件事的两半（间隙决定步距，连页标志决定排版在不在页底断行），
 *   所以必须成套设下去 —— 分两路传，中间那一拍就会出现"连页的间隙 + 分页的落点"
 *   这种不存在的组合。薄壳（engine_worker）只调这一条，不碰引擎的导出。
 *
 * @param {object} m { gap?: number, continuous?: boolean }
 */
function setViewMode(m: any) {
  if (typeof m.gap === 'number') mod.setPageGap(m.gap);
  mod.setContinuous(!!m.continuous);
}

/**
 * 页面画布到达：交给宿主登记，然后把已到齐的那几页补一遍，再回推几何。
 */
function pageCanvas(m: any) {
  host.addCanvas(m);
  sendState(m.ts, 'pageCanvas', JSON.parse(mod.editStatsJson()));
}

/**
 * 视图模式切换（分页 / 连页）：改引擎的排版形状（页间隙 + 连页标志）并重排重画。
 * 两种模式共用同一套"一页一张画布"，所以这条消息不再带画布 —— 它只是配置变更。
 */
function viewMode(m: any) {
  host.setViewMode(m);
  sendState(m.ts, 'viewMode', JSON.parse(mod.editStatsJson()));
}

/**
 * 换页度量与重排（给宿主的 resize / dpr 变化用）。
 */
function relayout() {
  mod.relayoutAndPaint();
  host.invalidateSlots();
  host.blitPages();
}

/**
 * in-thread 的一次收尾：滚动位置落地 → 决定窗口 → 必要时 present → 贴脏页。
 *
 * 与 worker 侧 view + push 同一口径，区别只有两点：同步，且把"先滚再对窗口"
 *   这条顺序做进一次调用里（顺序反了窗口就按旧的滚动位置算 —— 光标在第 0 页而屏幕
 *   停在第 15 页时，窗口会被拉成整篇）。
 *
 * @param opName 刚跑完的那条命令名（决定要不要补一次 present，见 PRESENT_ALWAYS）
 * @param m { scrollY: 设备像素 }
 */
function settle(opName: string, m: Args): { moved: boolean; scrolled: boolean; stats: any } {
  const prev = scrollY;
  scrollY = Math.max(0, Number(m.scrollY) || 0);
  if (scrollY !== prev) scrollDir = scrollY > prev ? 1 : -1;
  let st = JSON.parse(mod.editStatsJson());
  const moved = retarget(st);
  if (moved || PRESENT_ALWAYS.has(opName)) {
    mod.present();
    st = JSON.parse(mod.editStatsJson());
  }
  host.blitPages();
  return { moved, scrolled: scrollY !== prev, stats: st };
}

/**
 * 直接执行一条读命令，返回原值。给 in-thread 宿主用：它自己管呈现与窗口，
 *   不需要这边的收尾（present + blit + 回推 state），只要引擎那一步的结果。
 *
 * @param {string} name 读命令名
 * @param {object} args 参数
 * @returns {*} 命令结果；未知命令抛错
 */
function read(name: any, args: any) {
  // rawFrame 不在 READ 表里：它要拷一份（wasm 堆上的视图下一次 present 就被覆盖），
  //   与 askOp 里那条特判是同一件事。少了它，diag.rawFrame 直接抛。
  if (name === 'rawFrame') {
    const v = mod.frameRgba();
    const copy = new Uint8ClampedArray(v.length);
    copy.set(v);
    return copy;
  }
  const fn = READ[name];
  if (!fn) throw new Error('未知读取 ' + name);
  return fn(args || {});
}

/**
 * 直接执行一条写命令，返回引擎那一步的结果（通常是"有没有改动"）。
 *   in-thread 宿主拿到它之后自己做 settle（present + 重贴），收尾策略留在宿主侧。
 *
 * @param {string} name 写命令名
 * @param {object} args 参数
 * @returns {*} 引擎返回值；未知命令抛错
 */
function write(name: any, args: any) {
  const fn = WRITE[name] || SILENT[name];
  if (!fn) throw new Error('未知命令 ' + name);
  return fn(args || {});
}

/**
 * 把已经装好的引擎交给 API。
 *
 * in-thread 宿主的装载路径与 Worker 不同（它要读 HEAD、比构建戳、量字体字节），
 *   所以由它自己装载，再把实例交进来；Worker 侧仍走 boot。
 *
 * @param {object} engine wasm 模块实例
 */
function attach(engine: any) {
  mod = engine;
}

return {
  boot, do: doOp, ask: askOp, view, pageCanvas, viewMode, setViewMode, relayout, sendState,
  geom, settle,
  attach, read, write,
  /**
   * 装载类命令把滚动位置归零了：读一次就清（宿主按它追平 scrollTop）。
   */
  takeScrollReset: () => {
    const v = scrollResetHost;
    scrollResetHost = false;
    return v;
  },
  // 引擎句柄：只给宿主取帧视图与装载用，不要拿它绕过命令表。
  engine: () => mod,
  // 当前滚动位置（设备像素）：贴连页时算目标 y 要用。
  scrollY: () => scrollY,
  // 视口尺寸（设备像素）。
  viewSize: () => ({ w: viewW, h: viewH }),
  state: (ts, label, stats, changed, workMs) => sendState(ts, label, stats, changed, workMs),
};
}

export { createEngineApi, IDL };
