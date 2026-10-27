/**
 * HEditor 外部 API 契约（真源）。
 *
 * 实现就在同目录的 editor.ts 里，与这份契约共用同一套类型 —— 不再有一份手写的
 *   .d.ts 与实现并行维护：web/dist/index.d.ts 由 tsc --declaration 从这份文件生成。
 * 设计说明见 docs/外部API设计.md。
 *
 * 四条约定：坐标一律 CSS 像素、页内相对坐标（页号单独给），offset 是段内码点而不是
 *   UTF-16 单元；键盘、快捷键、IME、拖选、滚动都在库内部，外部不需要写任何 DOM 事件；
 *   返回 Promise 的方法在"引擎处理完 + 状态回推完"之后 resolve，查询类同步返回本地
 *   镜像；diag 的成员不承诺跨版本稳定，参数与返回暂为 unknown（载荷形状由 schema
 *   生成后替换）。
 */

/**
 * 文档内位置。offset 是段内码点序号（0 起）。
 */
export interface Pos {
  /**
   * 段号（0 起）。
   */
  para: number;

  /**
   * 段内码点序号（0 起；不是 UTF-16 码元，星平面字符算一个）。
   */
  offset: number;
}

/**
 * 一块页面矩形（CSS 像素，页内相对坐标）。
 */
export interface Rect {
  /**
   * 页号（0 起，全篇连续编号）。
   */
  page: number;

  /**
   * 左边缘（页内横坐标）。
   */
  x: number;

  /**
   * 上边缘（页内纵坐标）。
   */
  y: number;

  /**
   * 宽。
   */
  w: number;

  /**
   * 高。
   */
  h: number;
}

export type RGBA = [number, number, number, number];

/**
 * 光标：位置 + 落在哪一页 + 页内矩形。
 */
export interface Caret {
  /**
   * 位置（段号 + 段内偏移）。
   */
  pos: Pos;

  /**
   * 光标落在哪一页。
   */
  page: number;

  /**
   * 页内矩形：x/y 是插入点，w 是粗，h 是行高。
   */
  rect: { x: number; y: number; w: number; h: number };

  /**
   * 这一拍画没画出来（闪烁相位决定）。
   */
  visible?: boolean;
}

/**
 * 选区：两端位置 + 每页上的矩形（跨页会是多条）。
 */
export interface Selection {
  /**
   * 起点：按下鼠标那一端，拖选过程中不动。
   */
  anchor: Pos;

  /**
   * 焦点端：跟着指针或 Shift 移动的那一端。
   */
  focus: Pos;

  /**
   * 两端相同（没有真的选中东西）。
   */
  empty: boolean;

  /**
   * 每页上的矩形，跨页会是多条；空选区是空数组。
   */
  rects: Rect[];

  /**
   * 单元格选区时给出格区间；文字选区为 null。
   */
  cells: { rows: [number, number]; cols: [number, number] } | null;
}

/**
 * 视口与页度量（CSS 像素）。
 */
export interface Geometry {
  /**
   * 页宽 / 页高。
   */
  page: { w: number; h: number };

  /**
   * 页间隙（连页档是 0）。
   */
  gap: number;

  /**
   * 总页数（至少 1）。
   */
  pages: number;

  /**
   * 滚动内容高度（CSS 像素）。
   *
   * 分页：总页数 x 步距减掉末尾那道间隙。连页：跟着内容底边走，但至少一页 ——
   * 最后一条条带只露出自己那一段内容，所以它不是整数页。
   */
  scrollHeight: number;

  /**
   * 引擎当前装着的窗口：首页号与页数。它与可见页不是一回事，带着滚动方向的余量。
   */
  window: { first: number; count: number };

  /**
   * 屏幕像素比。
   */
  dpr: number;
}

export interface CaretStyle {
  /**
   * 颜色（RGBA，0~255；越界由引擎夹取）。
   */
  color: RGBA;

  /**
   * 线宽（磅）。下限 0.4，低于它按 0.4 算。
   */
  widthPt: number;
}

export interface SelectionStyle {
  /**
   * 选区色；半透明混色，alpha 决定深浅。
   */
  color: RGBA;
}

/**
 * 纸张规格。固定规格只列这几档，另一档是 'Custom'（宽高由调用方给，见 PageSetup.custom）——
 * 引擎只认页宽页高，规格名与这张表都只是宿主的书写便利。
 */
export type PaperSize = 'A4' | 'A3' | 'A5' | 'B5' | 'Letter' | 'Legal' | 'Custom';

/**
 * 固定纸张的宽高（磅）。表里是纵向（长边朝上），横排由 paperSizePt 对调。
 * 'Custom' 不在这张表里：它的尺寸来自调用方。
 */
export const PAPER_PT: Record<Exclude<PaperSize, 'Custom'>, { w: number; h: number }> = {
  A4: { w: 595.276, h: 841.89 },
  A3: { w: 841.89, h: 1190.551 },
  A5: { w: 419.528, h: 595.276 },
  B5: { w: 498.898, h: 708.661 },
  Letter: { w: 612, h: 792 },
  Legal: { w: 612, h: 1008 },
};

/**
 * 自定义纸张的宽高（磅）。它是"纵向"的口径：横排仍然由 paperSizePt 对调。
 */
export interface CustomPaper {
  /**
   * 页宽。
   */
  w: number;

  /**
   * 页高。
   */
  h: number;
}

/**
 * 按规格与方向算页宽页高。'Custom' 用 custom 给的宽高（缺了按 A4 算）。
 * @param paper 纸张规格
 * @param landscape true = 横排（宽高对调）
 * @param custom 'Custom' 的宽高（磅，纵向口径）
 * @returns 页宽与页高（磅）
 */
export function paperSizePt(
  paper: PaperSize,
  landscape: boolean,
  custom?: CustomPaper,
): { w: number; h: number } {
  const p: { w: number; h: number } =
    paper === 'Custom' ? (custom ?? PAPER_PT.A4) : (PAPER_PT[paper] ?? PAPER_PT.A4);
  return landscape ? { w: p.h, h: p.w } : { w: p.w, h: p.h };
}

/**
 * 四边页边距（磅）。
 */
export interface PageMargins {
  /**
   * 上边距。
   */
  top: number;

  /**
   * 右边距。
   */
  right: number;

  /**
   * 下边距。
   */
  bottom: number;

  /**
   * 左边距。
   */
  left: number;
}

/**
 * 页面设置：纸张、方向、四边页边距。改它要整篇重排（断行宽度变了）。
 */
export interface PageSetup {
  /**
   * 纸张规格。'Custom' 时宽高由 custom 给。
   */
  paper: PaperSize;

  /**
   * 横排（宽高对调）。
   */
  landscape: boolean;

  /**
   * 自定义纸张的宽高（磅，纵向口径）。paper 为 'Custom' 时必填：缺了或不是正数就抛 TypeError。
   * 换成固定规格之后它仍留着 —— 再切回 'Custom' 不用重新输一遍。
   */
  custom?: CustomPaper;

  /**
   * 四边页边距（磅）。1 厘米 = 28.35 磅。
   */
  margins: PageMargins;
}

/**
 * 运行期可改的配置。每项带一个代价标签（none / paint / relayout / reload，见设计文档 §7.3）；
 * 一次 configure 只做其中最贵的那一档 —— configure({zoom, showControlDots}) 是一次重排，
 * 不是"一次重排又一次重绘"。
 */
export interface EditorConfig {
  /**
   * 倍数，1 = 100%（96 dpi）。引擎内部收的是密度（4/3 基准），换算只在接缝上做。
   */
  zoom: number;

  /**
   * 分页 / 连页。连页时页间隙为 0，排版不在页底断行。
   */
  pageMode: 'paged' | 'continuous';

  /**
   * 页面设置：纸张、方向、四边页边距。改它整篇重排 —— 断行的可用宽度就是它算出来的。
   */
  page: PageSetup;

  /**
   * 编辑模式。三档都是产品语义，不是探针口径：
   *   document —— 随便改（默认）；
   *   form     —— 光标自由，但只有可编辑控件里的字改得动（填表）；
   *   readOnly —— 光标自由，一个字都不许改（预览）。
   * 只读时库的编辑方法直接返回 false；真正的拒绝仍由引擎判定（blockedEdits 记账）。
   */
  mode: 'document' | 'form' | 'readOnly';

  /**
   * 是否由库内部接管键盘/指针/IME。默认 true；false 只给"自己驱动"的自动化宿主。
   */
  input: boolean;

  /**
   * 段落标记括号。会改变行宽，要重排。
   */
  showBrackets: boolean;

  /**
   * 控件小点。只改光栅签名，不重排。
   */
  showControlDots: boolean;

  /**
   * 不可见字符标记：'none' 全不显示，'paragraph' 只显示段落标记，'all' 连空格与制表符
   * 一起显示（空格里画中点、制表符画箭头）。默认 'paragraph'。
   *
   * 标记是版面之外的一层注释：不占宽度、不进流位置、不影响断行与命中，所以只改光栅签名、
   * 不重排。只读模式下不画 —— 那是呈现态，标记会被印出来。
   */
  showMarks: 'none' | 'paragraph' | 'all';

  /**
   * 页边距裁剪标记：正文四角的短角线（与文字处理软件里的"显示裁剪标记"同一件事）。
   * 默认打开：它是判断"页边距设对了没有"的直接读数。臂长固定 18 磅（1/4 英寸，印刷裁切线
   * 的惯例），边距比它窄时按边距的一半收。它是页位图上的一层辅助线，不占宽度、不进流位置
   * —— 只重光栅，不重排；连页模式下没有页，四角也就没有意义，引擎不画。
   */
  marginMarks: boolean;

  /**
   * 撤销栈深度，0 = 不限。
   */
  undoLimit: number;

  /**
   * 光标样式。改它只重绘，不重排。
   */
  caretStyle: CaretStyle;

  /**
   * 选区样式。改它只重绘，不重排。
   */
  selectionStyle: SelectionStyle;
}

/**
 * 资源定位。不传时按当前脚本地址推（ESM 下可用；UMD 下必须显式给）。
 */
export interface Assets {
  /**
   * 胶水脚本（heditor.js）的绝对地址。
   */
  glue?: string;

  /**
   * wasm 的地址；不传按胶水同目录 + 同名推。
   */
  wasm?: string;

  /**
   * Worker 壳的地址；不传按库产物同目录推（web/dist/heditor_worker.js）。
   * 页面与库不在同一层时（开发服务器、示例页的构建产物）必须显式给。
   */
  worker?: string;

  /**
   * 迁移期：旧宿主脚本（app.js 等）所在目录。默认按本库脚本的地址推（../）。
   * C 步把它们搬进 src/ 之后这一项会删掉。
   */
  base?: string;
}

/**
 * 字体在三处样例内容里的角色：正文 / 蒙文竖排 / 拉丁回退。
 *
 * 只在"默认正文是谁、样例里那几段用哪份字体"这件事上有意义。它不是装载的前提 ——
 * 一份字体也能排版，缺的档按"正文回落到第一张、蒙文跟随正文、拉丁关掉回退"补。
 */
export type FontRole = 'cjk' | 'mongol' | 'latin';

/**
 * 一份字体：给地址，或者直接给字节。名字不用声明 —— 显示名由字体文件自己报
 * （name 表的族名），声明过的那一份迟早与文件对不上。
 */
export interface FontSource {
  /**
   * 取字体的地址（相对 createEditor 的产物目录，或绝对地址）。与 data 二选一。
   */
  url?: string;

  /**
   * 字体字节。File / Blob 会当场读出来；ArrayBuffer 与 Uint8Array 直接用。
   */
  data?: File | Blob | ArrayBuffer | Uint8Array;

  /**
   * 字节的文件名。只用于字体身份的最后一级（存档里写的是 PostScript 名，退到文件名时用它）；
   * 传 File 时自动取 file.name。
   */
  name?: string;

  /**
   * 显示名（给人看的那一行）。不传就用字体文件自己的族名。
   *
   * 只影响界面：身份仍是 PostScript 名 / 族名 / 文件名。注册表里同名多份时，按显示名
   *   选字体会命中先注册的那一份。
   */
  label?: string;

  /**
   * 角色。不传时该字体只是"可选的一份"，不改变默认正文。
   */
  role?: FontRole;

  /**
   * 按需装载：整份不进常驻，缺哪块取哪块。默认整份装进来。
   */
  lazy?: boolean;
}

/**
 * 一份字体的给法：一条地址串、一份声明，或者直接把字节递过来。
 */
export type FontInput = string | FontSource | Blob | ArrayBuffer | Uint8Array;

/**
 * 已注册的一份字体。名字是引擎从字体文件的 name 表读出来的，不是宿主声明的。
 */
export interface FontInfo {
  /**
   * 字体下标。文档里的字体引用存的就是它。
   */
  index: number;

  /**
   * 族名（字体文件里那个，给 format({ font }) 认）。
   */
  family: string;

  /**
   * 显示名：注册时给的 label，没给就是族名。界面用这一项。
   */
  label: string;

  /**
   * PostScript 名（存档里写的就是它）；读不到时是空串。
   */
  postScript: string;

  /**
   * 文件名（字体身份的最后一级）。
   */
  file: string;

  /**
   * 装载时的地址（绝对）；按字节注册的那一份是空串 —— 它没有来源地址。
   */
  url: string;

  /**
   * 是否按需装载。
   */
  lazy: boolean;

  /**
   * 三个角色里它占了哪一个；都不是时缺席。
   */
  role?: FontRole;
}

/**
 * 构造期参数：只管"挂在哪、怎么配"，内容走 open() / setText()。
 */
export interface EditorOptions {
  /**
   * 挂载点。必须是空元素，或带着本库骨架（heditor-stage / heditor-pages / heditor-ime）
   * 的那一份；库不认 id —— 同一页可以挂几个实例。
   */
  container: HTMLElement;

  /**
   * 素材地址覆盖（胶水 / wasm / 字体），不传按库的地址推。
   */
  assets?: Assets;

  /**
   * 字体清单。可给一项或一整串；不传用内置的三份 Noto（正文 / 蒙文 / 拉丁）。
   *
   * 只管"装载"，不管"用哪一份" —— 选字体走 format({ font }) 或 paragraphFormat({ font })。
   */
  fonts?: FontInput | FontInput[];

  /**
   * 初始配置（等价于创建之后调一次 configure）。
   */
  config?: Partial<EditorConfig>;

  /**
   * 是否画工具条。默认 false：产品页只给舞台。
   */
  chrome?: boolean;

  /**
   * 诊断面开关。默认 false：实例上没有 diag，访问会抛。
   *
   * 探针与审计带 true；产品页不带 —— "能不能依赖 diag"在代码里是显式的，
   * 而不是靠一句"不承诺稳定"的注释。
   */
  debug?: boolean;

  /**
   * 引擎彻底不可用时给用户的出口（通常就是 location.reload）。
   *
   * 库自己不能重启引擎：重启要重建实例、重装素材，而那是调用方的账。给了这一个，
   *   致命横幅上就有一个重试按钮；不给就只剩说明 —— 一条没有出口的失败提示是死胡同。
   */
  onFatalRetry?: () => void;

  /**
   * 选择类控件切换前问一次宿主：返回 false（或 resolve 出 false）就不切换。
   *
   * 允许异步，语义是"先持住不落"：等待期间文档不变、画面不变，也没有"先勾上再弹回去"
   * 的那一帧。钩子里不要调编辑 API（会把当前事务重入）。
   * @param change 切换的是哪个控件的哪个选项、切换之前的值
   * @returns 允许不允许；Promise 也行
   */
  onBeforeControlChange?: (change: ControlChange) => boolean | Promise<boolean>;

  /**
   * 点到一个"不是选择类"的控件（下拉、日期）时通知宿主：选项列表与日历都归宿主画。
   *
   * 引擎只负责画控件本身与它的值；弹出层是编辑期辅助，不进文档、不进导出。
   *
   * 只读的控件也会照常通知（标了只读不等于点不动），由宿主决定弹不弹 —— 但写回去
   *   的路已经封住了：引擎对只读控件拒写，弹出来也只能看。
   * @param index 控件下标
   * @param rect 控件在页面坐标下的第一块矩形（磅）；读不到时为 null
   * @param readonly 业务只读（填表的人只能看）
   */
  onControlTap?: (
    index: number,
    rect: { page: number; x: number; y: number; w: number; h: number } | null,
    readonly: boolean,
  ) => void;

  /**
   * 右键落在文档上时通知宿主：菜单归宿主画（与 onControlTap 同一分工）。
   *
   * 库在这一下里做两件事：把浏览器自己的菜单拦掉、把插入点搬到指的地方（点图片则选中它）。
   * 给到的落点归属是"搬完光标之后"的那一份，所以宿主可以直接用 tableState() / form() 读
   * 现状、直接改组选用的动作 —— 不必再自己判一遍点到哪。
   *
   * 不给这个回调时库不动右键（浏览器菜单照常弹）：拦掉一个宿主没打算接的手势，等于把一个
   * 能用的功能变成不能用的。
   * @param target 落点归属（种类、位置、格坐标、控件与图片下标）
   * @param at 指针位置（客户端坐标，CSS 像素）
   */
  onContextMenu?: (target: ContextTarget, at: { x: number; y: number }) => void;
}

/**
 * 光标移动的档位：行的首尾 vs 整篇的首尾。
 */
export type CaretStep =
  | 'left'
  | 'right'
  | 'up'
  | 'down'
  | 'lineStart'
  | 'lineEnd'
  | 'docStart'
  | 'docEnd';

export interface SetCaretOptions {
  /**
   * 落在字符前还是后（点击落在字的左半边还是右半边）。
   */
  side?: 'before' | 'after';

  /**
   * 同一段里有多行时落第几行。
   */
  line?: 'first' | 'last';
}

/**
 * 一个控件值的快照（前置回调用它描述"切换之前是什么"）。
 */
export interface ControlValueSnapshot {
  /**
   * 单控件的选中态。
   */
  flag: boolean;

  /**
   * 单选组 / 下拉 / 日期的值。
   */
  text: string;

  /**
   * 复选组选中的值集合。
   */
  items: string[];
}

/**
 * 设值时的入参：只给要改的那几项（值形态决定读哪一项）。
 */
export interface ControlValueInput {
  /**
   * 单控件的选中态。
   */
  flag?: boolean;

  /**
   * 单选组 / 下拉 / 日期的值。
   */
  text?: string;

  /**
   * 复选组选中的值集合。
   */
  items?: string[];
}

/**
 * 一次选择类控件的切换请求，交给 onBeforeControlChange 裁决。
 */
export interface ControlChange {
  /**
   * 控件下标（文档顺序）。
   */
  index: number;

  /**
   * 组控件的选项下标；单控件为 -1。
   */
  option: number;

  /**
   * 业务类型（radioButton / checkBox / radioGroup / checkBoxGroup）。
   */
  type: string;

  /**
   * 业务编码（可能为空）。
   */
  key: string;

  /**
   * 切换之前的值。
   */
  oldValue: ControlValueSnapshot;
}

/**
 * 插入内容控件（表单字段）的选项。全部可省：省掉时是一个文本字段。
 */
export interface ControlInsertOptions {
  /**
   * 业务类型（text / date / checkbox …）。默认 text。
   */
  type?: string;

  /**
   * 业务编码。同一个编码可以绑多个控件，按编码批量设值会一起改。
   */
  key?: string;

  /**
   * 控件内容。文本类控件的值就是它，也是插进去之后光标停的地方。
   */
  label?: string;

  /**
   * 提示文字：内容为空时画在控件里的那句话（灰字，只读与打印态不画）。
   */
  placeholder?: string;

  /**
   * 业务只读：填表的人只能看（插入之后仍然可以改回来）。
   */
  readonly?: boolean;

  /**
   * 必填：提交校验用；隐藏的控件不参与。
   */
  required?: boolean;

  /**
   * 值形态（决定值放内容里还是属性上）。默认按类型推断。
   */
  shape?: 'text' | 'bool' | 'enum' | 'list' | 'resource';

  /**
   * 选项表：单选组、复选组与下拉用。值与标签分开 —— 值进数据，标签进纸面。
   */
  options?: Array<{ value: string; label: string }>;

  /**
   * 互斥组名：同组的单选最多一个选中（单控件用；组控件自身即一组）。
   */
  group?: string;

  /**
   * 下拉允许输入选项表以外的值。
   */
  freeInput?: boolean;

  /**
   * 日期显示格式（yyyy / MM / dd / HH / mm / ss 与字面字符）。
   */
  format?: string;

  /**
   * 日期能选中哪些单位。默认六个全选。
   */
  dateUnits?: Array<'year' | 'month' | 'day' | 'hour' | 'minute' | 'second'>;

  /**
   * 选择类记号（圆圈 / 方框 / 对勾）的颜色。不给就跟随文字颜色。
   */
  glyphColor?: RGBA;
}

/**
 * 改一个控件定义的选项：只给要改的那几项，没给的不动。
 *
 * "不给"与"给空"是两件事：不给 options 是"选项表不动"，给空数组是"清空选项表"。
 */
export interface ControlUpdateOptions {
  /**
   * 业务类型。给了必须与当前一致 —— 换类型请删掉重插（值形态与内容形状都会变）。
   */
  type?: string;

  /**
   * 业务编码。
   */
  key?: string;

  /**
   * 单控件的标签（选择类）。组控件的文案在选项表里。
   */
  label?: string;

  /**
   * 提示文字：内容为空时画在控件里的那句话（不进内容、不进值）。
   */
  placeholder?: string;

  /**
   * 选项表（选择类与下拉）。
   */
  options?: Array<{ value: string; label: string }>;

  /**
   * 互斥组名（单选）。
   */
  group?: string;

  /**
   * 下拉允许输入选项表以外的值。
   */
  freeInput?: boolean;

  /**
   * 日期显示格式（yyyy / MM / dd / HH / mm / ss 与字面字符）。
   */
  format?: string;

  /**
   * 日期能选中哪些单位。给了就必须至少留一个。
   */
  dateUnits?: Array<'year' | 'month' | 'day' | 'hour' | 'minute' | 'second'>;

  /**
   * 选择类记号的颜色（圆圈 / 方框 / 对勾）。全透明 = 跟随文字颜色。
   */
  glyphColor?: RGBA;

  /**
   * 只读：填表的人只能看。不给就不动。
   *
   * 与其余几项不同，它落在运行态属性上；一次调用要改它又要改定义时是两步撤销。
   */
  readonly?: boolean;

  /**
   * 必填：提交校验用，隐藏的控件不参与。不给就不动。
   */
  required?: boolean;
}

/**
 * 插入图片的选项。资源 id 与尺寸至少要给一组 —— 三者都不给时插进去的是一个 4:3 的占位框。
 */
export interface ImageInsertOptions {
  /**
   * 资源 id（registerImage 注册时用的那个）。空串表示先占位。
   */
  resourceId?: string;

  /**
   * 原始像素宽。与高度一起给时，显示尺寸留空就按 DPI 折算。
   */
  pxWidth?: number;

  /**
   * 原始像素高。
   */
  pxHeight?: number;

  /**
   * 原始像素的 DPI，默认 96（行业惯例）。
   */
  dpi?: number;

  /**
   * 显示宽度（磅）。只给一边时按原图比例推另一边。
   */
  width?: number;

  /**
   * 显示高度（磅）。
   */
  height?: number;
}

/**
 * 注册图片像素的入参。引擎里没有解码器，RGBA 由宿主解码之后交上来。
 */
export interface ImageRegisterOptions {
  /**
   * 资源 id（文档里存的就是它）。
   */
  id: string;

  /**
   * 像素宽。
   */
  width: number;

  /**
   * 像素高。
   */
  height: number;

  /**
   * RGBA 像素（行优先，未预乘）。长度必须等于宽 x 高 x 4。
   */
  data: Uint8Array;
}

/**
 * 文档里一张图片的几何与状态。
 */
export interface ImageBox {
  /**
   * 文档顺序的下标（selectImage / resizeImage 收它）。
   */
  index: number;

  /**
   * 节点 id（编辑之后仍然有效）。
   */
  node: number;

  /**
   * 资源 id。
   */
  resourceId: string;

  /**
   * 显示宽度（磅）。
   */
  w: number;

  /**
   * 显示高度（磅）。
   */
  h: number;

  /**
   * 像素在不在资源表里。不在时画面上是占位框。
   */
  ready: boolean;

  /**
   * 版式档位（见 ImageLayout）。
   */
  layout: ImageLayout;

  /**
   * 它落在哪几页的哪些矩形上（页内磅）。
   */
  rects: Array<{ page: number; x: number; y: number; w: number; h: number }>;
}

/**
 * 图片在版面上的摆法，与 Word 的"环绕方式"一一对应。
 *
 * inline 随文走；后四档都是浮动：square 文字绕在两侧、topAndBottom 文字不进图片的纵向区间、
 * behind / inFront 完全不占位（图压在文字下或盖在文字上）。
 */
export type ImageLayout = 'inline' | 'square' | 'topAndBottom' | 'behind' | 'inFront';

/**
 * 图片命中：点到哪一张、哪个把手（-1 表示点在图上）。
 */
export interface ImageHit {
  /**
   * 图片下标；-1 = 没点到图片。
   */
  index: number;

  /**
   * 把手序号（0 左上、1 上、2 右上、3 右、4 右下、5 下、6 左下、7 左）。
   */
  handle: number;
}

/**
 * 表格结构命令的动词。
 *
 * 八条命令只有动词不同：主语都是"光标所在的那一格 / 那一行 / 那一列"，被拒的条件也都由
 *   引擎判（模式不对、删到只剩一行一列、要合并的格子不是矩形）。宿主不该自己再判一遍。
 */
export type TableOp =
  | 'insertRowAbove'
  | 'insertRowBelow'
  | 'removeRow'
  | 'insertColumnLeft'
  | 'insertColumnRight'
  | 'removeColumn'
  | 'mergeCells'
  | 'splitCell'
  | 'removeTable';

/**
 * 改一个单元格的选项。只给要改的项 —— 跨度由行列操作与合并拆分改写，不在这里。
 */
export interface TableCellOptions {
  /**
   * 格内垂直对齐。
   */
  vAlign?: 'top' | 'middle' | 'bottom';
}

/**
 * 改一行的选项。只给要改的项（不给 = 这一项不动）。
 */
export interface TableRowOptions {
  /**
   * 标题行：表格跨页时在每页顶端重复。只有从第 0 行起的连续几行才能是标题行。
   */
  header?: boolean;

  /**
   * 允许被分页线切开。
   */
  split?: boolean;

  /**
   * 行前强制分页。
   */
  breakBefore?: boolean;

  /**
   * 行高（磅）。0 或不给表示按内容。
   */
  heightPt?: number;

  /**
   * 行高规则：按内容 / 不小于 / 固定。
   */
  heightRule?: 'auto' | 'atLeast' | 'exact';
}

/**
 * 改一张表的选项。只给要改的项。
 */
export interface TableOptions {
  /**
   * 表格宽度（磅）。0 表示撑满可用宽度。
   */
  widthPt?: number;

  /**
   * 表格在内容区里的水平位置。
   */
  align?: 'start' | 'center' | 'end';
}

/**
 * 表格栅格：表的横向范围、列边界与这一页的行片段。长度一律是磅。
 *
 * 拖拽列边界 / 行边界要用它把指针位置换算成"哪一条线、拖到哪儿"。列边界相对表格左边
 *   （长度 = 列数 + 1）；行片段是页内坐标，一行跨页时每页各有一条。
 */
export interface TableGrid {
  /**
   * 点在不在表格里。为假时其余字段都是默认值。
   */
  ok: boolean;

  /**
   * 表格身份（改列宽 / 行高时原样带回去）。
   */
  table: number;

  /**
   * 这一页的页号。
   */
  page: number;

  /**
   * 表格左边（页内磅）。
   */
  x: number;

  /**
   * 表格总宽（磅）。
   */
  w: number;

  /**
   * 列边界，相对表格左边，长度 = 列数 + 1。
   */
  cols: number[];

  /**
   * 这一页上的行片段：逻辑行号、行顶（页内磅）与高。
   */
  rows: Array<{ row: number; y: number; h: number }>;
}

/**
 * 水印：衬在正文下方的一行半透明文字。
 *
 * 与 Word 的"自定义文字水印"同一种形态：一条文字、居中、可倾斜、半透明。
 */
export interface WatermarkOptions {
  /**
   * 画不画。省略时保持现状。
   */
  enabled?: boolean;

  /**
   * 水印文字。
   */
  text?: string;

  /**
   * 字号（磅）。
   */
  sizePt?: number;

  /**
   * 颜色与不透明度（RGBA，0~255）。alpha 就是"半透明"的唯一表达。
   */
  color?: [number, number, number, number];

  /**
   * 倾斜角（度，负值是逆时针）。
   */
  angle?: number;
}

/**
 * 当前水印的记录。字段与 WatermarkOptions 同名同序，面板回填不必翻译。
 */
export interface WatermarkState {
  /**
   * 画不画。
   */
  enabled: boolean;

  /**
   * 水印文字。
   */
  text: string;

  /**
   * 字号（磅）。
   */
  sizePt: number;

  /**
   * 颜色与不透明度。
   */
  color: [number, number, number, number];

  /**
   * 倾斜角（度）。
   */
  angle: number;
}

/**
 * 光标所在表格的现状：三层记录 + 整格选区的范围。
 *
 * 一份读全是有意的：属性面板要同时显示表宽、行高与格对齐，分三次问会拿到三个不同时刻
 *   的状态（每次之间光标可能已经变了）。
 */
export interface TableState {  /**
   * 光标在不在表格里。为假时下面各字段都是默认值，宿主不该据此施改。
   */
  inTable: boolean;

  /**
   * 网格行数。
   */
  rows: number;

  /**
   * 网格列数。
   */
  cols: number;

  /**
   * 表格宽度（磅）；-1 = 撑满可用宽度。
   */
  widthPt: number;

  /**
   * 表格的水平位置。
   */
  align: 'start' | 'center' | 'end';

  /**
   * 从第 0 行起连续标为标题行的行数。
   */
  headerRows: number;

  /**
   * 光标所在行的行号。
   */
  row: number;

  /**
   * 这一行是不是标题行。
   */
  rowHeader: boolean;

  /**
   * 这一行允不允许被分页线切开。
   */
  rowSplit: boolean;

  /**
   * 这一行之前是不是强制分页。
   */
  rowBreak: boolean;

  /**
   * 行高（磅）；-1 = 按内容。
   */
  heightPt: number;

  /**
   * 行高规则。
   */
  heightRule: 'auto' | 'atLeast' | 'exact';

  /**
   * 光标所在格的行号（合并格的左上角）。
   */
  cellRow: number;

  /**
   * 光标所在格的列号（合并格的左上角）。
   */
  cellCol: number;

  /**
   * 这一格的纵向跨度。
   */
  rowSpan: number;

  /**
   * 这一格的横向跨度。
   */
  colSpan: number;

  /**
   * 格内垂直对齐。
   */
  vAlign: 'top' | 'middle' | 'bottom';

  /**
   * 整格选区覆盖了几格（0 = 没有整格选区）。
   */
  selCells: number;

  /**
   * 整格选区覆盖几行。
   */
  selRows: number;

  /**
   * 整格选区覆盖几列。
   */
  selCols: number;

  /**
   * 现在插得进行。
   */
  canInsertRow: boolean;

  /**
   * 现在删得掉这一行（只剩一行、或行里有纵向合并时为假）。
   */
  canRemoveRow: boolean;

  /**
   * 现在插得进列。
   */
  canInsertColumn: boolean;

  /**
   * 现在删得掉这一列（只剩一列时为假）。
   */
  canRemoveColumn: boolean;

  /**
   * 现在的整格选区合得成（至少两格、铺满矩形、除锚点外是空格）。
   */
  canMerge: boolean;

  /**
   * 光标所在格拆得开（跨度大于一）。
   */
  canSplit: boolean;

  /**
   * 现在删得掉整张表（光标在表里、且删完还剩内容）。
   */
  canRemoveTable: boolean;
}

/**
 * 右键落点上压着什么（onContextMenu 的入参）。
 *
 * 优先级由引擎定：图片 → 控件 → 表格 → 正文。宿主拿它决定弹哪一份菜单，不必自己再猜
 *   —— 猜出来的那份在"图片压在字上"这类位置必然错。
 */
export interface ContextTarget {
  /**
   * 落点的种类。none = 点在页面之外（页间隙、装订区）。
   */
  kind: 'none' | 'text' | 'table' | 'control' | 'image';

  /**
   * 落点吸附到的文档位置；none 时为 null。
   */
  pos: Pos | null;

  /**
   * 落在表格里时给出格坐标；不在表格里为 null。
   */
  cell: { table: number; row: number; col: number } | null;

  /**
   * 落点上压着的控件下标；不在控件里为 -1。
   */
  control: number;

  /**
   * 落点上压着的图片下标；不在图片上为 -1。
   */
  image: number;
}

/**
 * 表单里的一个控件（form() 的精简视图）。
 */
export interface ControlSummary {
  /**
   * 序号（0 起，按文档顺序）。
   */
  index: number;

  /**
   * 引擎的种类号（旧口径，保留给已有调用方）。
   */
  kind: string;

  /**
   * 业务类型（text / number / radioButton / ...）。
   */
  type: string;

  /**
   * 业务编码（可能为空）。
   */
  key: string;

  /**
   * 单控件的标签（组控件为空：它的文案在选项表里）。
   */
  label: string;

  /**
   * 业务只读：填表的人只能看。为真时引擎拒写（值与勾选都改不动）。
   */
  readonly: boolean;

  /**
   * 必填（提交校验用；隐藏的控件不参与）。
   */
  required: boolean;

  /**
   * 下拉允许输入选项表以外的值。
   */
  freeInput: boolean;

  /**
   * 互斥组名（空 = 不与任何人互斥）。
   */
  group: string;

  /**
   * 内容的纯文本：文本与数值字段的值就是它。
   */
  text: string;

  /**
   * 提示文字：值（内容）为空时画在控件里的那句话。它不进内容，也不进值。
   */
  placeholder: string;

  /**
   * 当前值（记录上的那一份）。
   */
  value: ControlValueSnapshot;

  /**
   * 选项表（选择类与下拉；没有选项时为空）。
   */
  options: Array<{ value: string; label: string }>;

  /**
   * 选择类记号的颜色（alpha 0 = 跟随文字颜色）。
   */
  glyphColor: RGBA;

  /**
   * 日期的显示格式串（其余类型为空）。
   */
  format: string;

  /**
   * 日期能选中哪些单位（其余类型为空数组）。
   */
  dateUnits: Array<'year' | 'month' | 'day' | 'hour' | 'minute' | 'second'>;

  /**
   * 区间起点（段号 + 段内码点偏移）。
   */
  start: { para: number; offset: number };

  /**
   * 区间终点（段号 + 段内码点偏移）。
   */
  end: { para: number; offset: number };

  /**
   * 光标是否在这个控件里。
   */
  focused: boolean;
}

/**
 * 内容变更。
 */
export interface ChangeEvent {
  /**
   * 改动来自哪条路：真实输入（键盘、指针、IME、剪贴板）还是库的方法。
   */
  source: 'user' | 'api';

  /**
   * 内容版本，每次变更 +1。可用来跳过重复处理。
   */
  version: number;

  /**
   * 整篇纯文本（段间 \n）。引擎里本来就坐着，几乎零成本。
   */
  text: string;

  /**
   * 带格式的整篇 JSON。方法而不是属性：调它才序列化，同一版本只算一次。
   *
   * 2000 段文档序列化一次约 55 ms（Worker 传输还多一次往返）—— 每次 change 都读会
   *   直接卡住键盘。写成方法是为了让"读它"显式：属性会被 JSON.stringify(ev) 顺带算出来，
   *   而那正好是最糟的一拍。大文档请只标脏，debounce 之后调 await editor.save()。
   */
  json(): Promise<string>;
}

/**
 * 事件表。on() 的键集就是这六个。
 */
export interface EditorEvents {
  /**
   * 内容变了（每次提交一次，含撤销与重做）。
   */
  change: ChangeEvent;

  /**
   * 选区变了（两端或矩形变了才发）。
   */
  selection: Selection;

  /**
   * 光标变了（段内位置、页号或可见性变了才发）。
   */
  caret: Caret;

  /**
   * 呈现完成。pages = 这一拍重画的页数。
   */
  render: { pages: number; ms: number };

  /**
   * 生效值真的变了才发（设成同一个值不发）。
   */
  configchange: { changed: Partial<EditorConfig>; config: EditorConfig };

  /**
   * 失败：fatal = 引擎整条不可用。降级也会发，同时上一条提示条。
   */
  error: { message: string; fatal: boolean };
}

/**
 * 诊断面：给无头探针与审计用。不承诺跨版本稳定，产品代码不该依赖它。
 *
 * 参数与返回暂为 unknown：它们对应引擎内部的账本载荷（19 份 / 245 个键），
 * 形状由 schema 生成到 payloads.gen.d.ts 之后再把这里换成具体类型。
 *
 * 带 Direct 的那几个是"每次都真的问引擎一次"的读：同线程传输下是同步值，
 * Worker 传输下是一次往返。调用方写成 await，两种传输都对（await 普通值是恒等）。
 */
type Direct<T> = T | Promise<T>;

export interface Diag {
  /**
   * 排版那本账：页数、行数、各阶段耗时。
   */
  stats(): unknown;

  /**
   * 编辑那本账：本拍重画像素、脏页、排版账、光标矩形。
   */
  editStats(): unknown;

  /**
   * 同 editStats；保留给"读一次编辑账"的老写法。
   */
  edit(): unknown;

  /**
   * 帧自审：当前帧与从零重画逐字节比，diff = 0 表示一致。
   */
  auditFrame(): unknown;

  /**
   * 底图自审：页位图与从零光栅比。
   */
  auditBase(): unknown;

  /**
   * 某页的缓存与从零光栅比。参数：page（页号）。
   */
  auditPage(): unknown;

  /**
   * 增量排版与全量重排逐字段比。
   */
  auditLayout(): unknown;

  /**
   * 模型重新编译一遍，与引擎里那一份逐字段比。
   */
  auditCompile(): unknown;

  /**
   * 字形页归属的自审（分段与分页两套口径）。
   */
  auditGlyphPages(): unknown;

  /**
   * 每页画布上的像素与引擎导出像素逐点比：屏幕 == 引擎帧。
   */
  selfCheck(): unknown;

  /**
   * 每个可编辑字段的左括号在帧上画了没有（扫左缘那一列）。
   */
  markCheck(): unknown;

  /**
   * 帧的一份快照（RGBA），拷出来之后不受后续呈现影响。
   */
  rawFrame(): unknown;

  /**
   * 帧指纹（FNV-1a，只算 RGB 三通道）。
   */
  frameDigest(): Direct<number>;

  /**
   * 帧宽（设备像素）。
   */
  frameW(): Direct<number>;

  /**
   * 帧高（设备像素）= 窗口页数 x 步距。
   */
  frameH(): Direct<number>;

  /**
   * 呈现一次（不重排）。
   */
  present(): void;

  /**
   * 控件表：区间、只读、矩形、当前焦点。
   */
  controls(): unknown;

  /**
   * 括号槽清单：每枚括号的位置、侧别、属于哪个控件。
   */
  bracketSlots(): unknown;

  /**
   * 命中测试：文档坐标（磅）折成段内位置与控件归属。
   * @param x 横坐标（磅）
   * @param y 纵坐标（磅）
   */
  hit(x: number, y: number): unknown;

  /**
   * 某个点上压着哪个控件（-1 = 不在任何控件里）。
   * @param x 横坐标（磅）
   * @param y 纵坐标（磅）
   */
  controlAt(x: number, y: number): unknown;

  /**
   * 控件表的人读版，给调试输出用。
   */
  describeControls(): Direct<string>;

  /**
   * 被拒绝的编辑次数（引擎那本账）。
   */
  blockedEdits(): number;

  /**
   * 本拍写过哪几页。
   */
  dirtyPages(): number[];

  /**
   * 页度量（设备像素）：宽、高、间隙。
   */
  pagePx(): { w: number; h: number; gap: number };

  /**
   * 页画布张数。
   */
  pageCanvases(): unknown;

  /**
   * 页画布元素列表（按页号升序）。
   */
  pageCanvasList(): unknown;

  /**
   * 预热某一页的字形（滚动方向上提前一页）。
   * @param page 页号
   */
  prewarmPage(page: number): void;

  /**
   * 重排重绘的基准：重复若干次，回报每阶段耗时。
   */
  bench(): unknown;

  /**
   * 光标的一点表：行内起点、本偏移的沿轴位置、侧别。
   */
  caretReport(): string;

  /**
   * 引擎里的光标偏移（直读，不经过 editStats 那本账）。
   */
  caretLive(): number;

  /**
   * 运行环境：UA、地址、dpr、wasm 堆占用。
   */
  env(): unknown;

  /**
   * 字体装载的账：模式、字节数、缺字计数。
   */
  fonts(): unknown;

  /**
   * 最近一次贴图的账：毫秒、页数、实际写入的像素面积。
   */
  blitStats(): { ms: number; pages: number; px: number };

  /**
   * 旧宿主的"上一条贴图"；新库不维护，恒 null（判据改用 blitStats）。
   */
  lastBlit(): unknown;

  /**
   * 按坐标点一下（自动化用；产品面不该用它替代 setCaret）。
   * @param pt 文档坐标（设备像素）
   */
  clickAt(pt: { x: number; y: number }): Promise<void>;

  /**
   * 选中 pos 所在单元格（自动化用）。
   * @param pos 格内任一个位置
   */
  selectCell(pos: Pos): Promise<void>;

  /**
   * 删控件；keepContent = true 是拆壳（内容留下来变成普通正文）。
   * @param index 控件序号
   * @param keepContent true = 拆壳，内容留下变成普通正文
   * @returns 引擎的判定（锁定状态下删不掉会返回 false）
   */
  removeControl(index: number, keepContent?: boolean): Promise<boolean>;

  // 装载：内置场景与压力文档

  /**
   * 装载内置场景（id 同旧宿主：6 单段演示、7 多段长文）。zoom 是密度，不是倍数。
   * @param id 场景号
   * @param zoom 密度（设备像素 / 磅），不是倍数
   */
  loadScenario(id: number, zoom?: number): Promise<unknown>;

  /**
   * 回到默认场景。
   * @param id 场景号，默认 6
   * @param zoom 密度，默认沿用引擎当前值
   */
  reset(id?: number, zoom?: number): Promise<unknown>;

  /**
   * 造 N 段、每段 M 字的压力文档（整篇同一个字）。
   * @param paras 段数
   * @param chars 每段字数
   */
  stress(paras: number, chars: number): Promise<unknown>;

  /**
   * 同上，但每段字不同；量字形缓存要用它，否则全命中失真。
   * @param paras 段数
   * @param chars 每段字数
   */
  stressDiverse(paras: number, chars: number): Promise<unknown>;

  /**
   * 表单样例（控件区间与只读标记都在里面）。
   * @param variant 样例档位（0 单字段、2 相邻、3 嵌套）
   */
  loadFormSample(variant?: number): Promise<unknown>;

  /**
   * 病历样例：读 assets/samples/medical-records.md，清洗后按段落建文档。
   * @param repeat 正文重复几遍
   */
  loadMedicalSample(repeat?: number): Promise<unknown>;

  /**
   * 把焦点放回承接层：装载之后要让真实键盘能打进文档。
   */
  focus(): void;

  /**
   * 选区的原始载荷：两端是全局流位置（对外 API 用的是段内位置），外加页面矩形。
   */
  selectionRaw(): unknown;

  /**
   * 拖选状态：在拖、动过、搬过几次焦点端、是否在自动滚动。
   */
  dragState(): unknown;

  /**
   * 在途的命中都回来才 resolve。
   */
  dragSettled(): Promise<void>;

  /**
   * 整批重建控件（区间树的父子关系是全局性质，只能整棵换）。
   * @param list 每个控件的区间与属性
   */
  setFormControls(list: Array<{ start: number; end: number; kind?: number; id?: string; ro?: boolean }>): Promise<unknown>;

  // 光标与相位：复现手势、定格一帧

  /**
   * 带归属的定位（field 是控件序号）：复现"点后一个字段的开头"。
   * @param para 段号
   * @param offset 段内偏移
   * @param field 控件序号（-1 = 不在控件里）
   */
  setCaretAt(para: number, offset: number, field?: number): Promise<unknown>;

  /**
   * 只改位置、不呈现 —— 给"改位置"与"呈现"分账的对照实验。
   * @param para 段号
   * @param offset 段内偏移
   */
  setCaretOnly(para: number, offset: number): unknown;

  /**
   * 直接设定光标可见相位（不重绘）。
   * @param on 可见相位
   */
  showCaret(on: boolean): unknown;

  /**
   * 重启闪烁（回到可见相位）。
   */
  startBlink(): void;

  /**
   * 停表：光标常亮，也不重绘。
   */
  stopBlink(): void;

  // 字体与窗口

  /**
   * 重装字体；mode = 'lazy' 走 MEMFS 按需读，'full' 整份交给引擎。
   * @param mode 装载模式，默认 full
   * @returns mode 与实际占用的字节数
   */
  reloadFonts(mode?: 'full' | 'lazy'): Promise<{ mode: string; bytes: number }>;

  /**
   * 字体块缓存配置（字节 / 块数）。改完要重装字体才干净。
   * @param blockSize 块大小（字节）
   * @param maxBlocks 最多几块
   */
  setFontCacheConfig(blockSize: number, maxBlocks: number): unknown;

  /**
   * 直接设窗口锚点（绕过滚动事件），量"换窗口要读多少字形"。
   * @param first 首页号
   * @param count 页数
   */
  setViewportAnchor(first: number, count: number): unknown;

  /**
   * 页面级错误历史（与 error 事件同源，按发生顺序）。
   */
  errors(): string[];

  /**
   * 能力探测现场：supported 是库的判断；probe 是那段含 v128 的探针字节本身
   * （判据可以拿它自己跑一次 validate，再查里面有没有 SIMD 指令）。
   */
  simd(): unknown;
}

/**
 * 字符格式补丁：只给要改的字段，没给的一律保持不变。
 *
 * 语义与文档模型里的三态一致 —— 按下"加粗"只该动 bold，不该连带把字号写死。
 */
export interface CharFormatPatch {
  /**
   * 加粗。字重走可变字体的 wght 轴（700），字宽会跟着变。
   */
  bold?: boolean;

  /**
   * 斜体。字体没有斜体面时由绘制层做合成倾斜。
   */
  italic?: boolean;

  /**
   * 下划线。
   */
  underline?: boolean;

  /**
   * 字体：下标（见 fonts()）或族名 / PostScript 名 / 文件名 / 显示名。
   *
   * 认名字是为了让调用方不必记下标：注册顺序一变，写死的下标就指向另一份字体，
   *   而画面"看起来还能用"。
   */
  font?: number | string;

  /**
   * 字号（磅）。
   */
  size?: number;

  /**
   * 字色，[r, g, b]。
   */
  color?: [number, number, number];
}

/**
 * 行距规则，与 Word 段落面板的三档一一对应。
 *
 * auto 按单倍行高的倍数；exact 就是那个磅值，内容更高时裁掉（与 Word 同一档）；
 * atLeast 取固定值与内容高的较大者。
 */
export type LineSpacingRule = 'auto' | 'exact' | 'atLeast';

/**
 * 行距：规则 + 那一档用的值。
 *
 * 倍数与磅值并存而不是折算成一个数：换个字号，倍数的结果该跟着变，磅值不该。
 * exact 与 atLeast 只看 exact，auto 只看 factor（缺省 1，即单倍）。
 */
export interface LineSpacing {
  /**
   * 规则。
   */
  rule: LineSpacingRule;

  /**
   * 倍数（auto 档），缺省 1 = 单倍行距。
   */
  factor?: number;

  /**
   * 固定值（exact / atLeast 档，磅）。
   */
  exact?: number;
}

/**
 * 段落格式补丁：只给要改的字段。
 */
export interface ParaFormatPatch {
  /**
   * 段落主字体：下标或族名（与字符格式的 font 同一套名字）。
   */
  font?: number | string;

  /**
   * 段落字号（磅）。
   */
  size?: number;

  /**
   * 行距：规则（auto / exact / atLeast）与那一档的值。
   *
   * auto 的倍数是相对单倍行高（字体的 ascent + descent + lineGap），1 即 Word 的单倍行距；
   * exact / atLeast 收磅值。exact 小于内容时按行裁字，与 Word 一致。
   */
  lineSpacing?: LineSpacing;

  /**
   * 段前距（磅）。页顶那一段不加；与上一段的段后距相加，不折叠。
   */
  spaceBefore?: number;

  /**
   * 段后距（磅）。
   */
  spaceAfter?: number;

  /**
   * 段前分页。
   */
  pageBreakBefore?: boolean;
}

/**
 * 属性面板的字段种类。渲染器按它挑控件，七种之外不增。
 */
export type FormFieldKind =
  | 'heading'
  | 'text'
  | 'lines'
  | 'number'
  | 'select'
  | 'toggle'
  | 'color'
  | 'datetime';

/**
 * 属性面板的一个字段。
 *
 * 描述由引擎算（哪种控件该有哪些项、表格三层各有什么），标签与样式由宿主定。
 *   heading 不是字段，只用来把表单分段。
 */
export interface FormField {
  /**
   * 种类。
   */
  kind: FormFieldKind;

  /**
   * 取值用的键；heading 为空串。
   */
  key: string;

  /**
   * 左列的标签；heading 时是分段标题。
   */
  label: string;

  /**
   * 当前值：文本类与下拉是字符串，数值是数字，开关是布尔；heading 不给。
   */
  value?: string | number | boolean;

  /**
   * 下拉的候选。
   */
  options?: Array<{ value: string; text: string }>;

  /**
   * 数值的下限、上限与步长。
   */
  min?: number;
  max?: number;
  step?: number;

  /**
   * 数值的单位，显示在输入框右边。
   */
  unit?: string;

  /**
   * 输入框的占位提示。
   */
  placeholder?: string;

  /**
   * 字段下方的一句说明。
   */
  hint?: string;

  /**
   * 多行文本显示几行。
   */
  rows?: number;

  /**
   * 置灰：看得见、改不动。
   */
  disabled?: boolean;

  /**
   * 日期与时间字段该用哪种原生输入控件（kind 为 datetime 时给）。
   *
   * 值已经按它裁好：date 是 YYYY-MM-DD、time 是 HH:MM[:SS]、datetime-local 是
   *   YYYY-MM-DDTHH:MM[:SS]。宿主直接塞给 input.value 就行 —— 长度不对的话浏览器
   *   会把 value 当成空串，表现是"点了日期，框里什么都没有"。
   */
  inputType?: 'date' | 'time' | 'datetime-local';
}

/**
 * 面板读出来的一份值，按字段键收好。
 */
export type FormValues = Record<string, string | number | boolean>;

/**
 * 字段标签的替换表：键是字段的 key，值是这家宿主想显示的字。
 *
 * 不给就用中文默认标签。要给整套界面换语言，把用到的键各给一条即可。
 */
export type FormLabels = Record<string, string>;

/**
 * 造字段表的选项。
 */
export interface FormOptions {
  /**
   * 标签替换表。
   */
  labels?: FormLabels;
}

/**
 * 一个控件的属性表单。
 */
export interface ControlForm {
  /**
   * 控件下标。
   */
  index: number;

  /**
   * 业务类型。
   */
  type: string;

  /**
   * 面板标题（含类型名）。
   */
  title: string;

  /**
   * 字段表，按显示顺序。
   */
  fields: FormField[];
}

/**
 * 光标所在表格的属性表单（表 / 行 / 格三层放进同一张）。
 */
export interface TableForm {
  /**
   * 光标在不在表格里。为假时 fields 为空，宿主不该据此施改。
   */
  inTable: boolean;

  /**
   * 面板标题（含行列数）。
   */
  title: string;

  /**
   * 字段表，按显示顺序。
   */
  fields: FormField[];
}

/**
 * 页面设置的属性表单（纸张、方向、宽高、四边页边距）。
 */
export interface PageSetupForm {
  /**
   * 面板标题。
   */
  title: string;

  /**
   * 字段表，按显示顺序。宽高两项只在纸张为自定义时可用。
   */
  fields: FormField[];
}

/**
 * 编辑器实例。
 *
 * 64 个方法 + element / diag 两个只读属性。分组只为可读，实例上是平铺的。
 */
export interface Editor {
  // 内容

  /**
   * 打开一份文档。首字符 { 且能解析出版本头按 JSON，否则按纯文本；失败时引擎状态不变。
   * @param data 文档内容（JSON 或纯文本）
   * @param opts 显式指定按哪种格式解析
   */
  open(data: string, opts?: { format?: 'json' | 'text' }): Promise<void>;

  /**
   * 用纯文本重置。repeat 复用同一段（压测用）。
   * @param text 段文本（段间用 \n 分段）
   * @param opts repeat = 把这段重复几遍
   */
  setText(text: string, opts?: { repeat?: number }): Promise<void>;

  /**
   * 整篇文本（段间 \n）。
   */
  text(): string;

  /**
   * 第 i 段的文本；越界返回空串。
   * @param i 段号（0 起）
   */
  paragraph(i: number): string;

  /**
   * 整篇分段。
   */
  paragraphs(): string[];

  /**
   * 序列化成 JSON 文本。
   */
  save(): Promise<string>;

  // 字体

  /**
   * 注册字体。构造之后随时可调，注册完就能用 —— 不必重建编辑器。
   *
   * 给地址、给 File / Blob、给 ArrayBuffer 都行；同一份（装载方式 + 地址或文件名）
   *   重复注册复用既有下标，不会在库里多出一张脸。只加脸，不重排：已经排好的内容
   *   不会因为多装一份字体而变样，新字体从下一次 format / paragraphFormat 开始生效。
   * @param sources 一份或几份字体；也可以直接给地址串
   * @returns 这一次注册的那几份（按入参顺序），带下标、族名与角色
   */
  addFont(...sources: Array<FontInput | FontInput[]>): Promise<FontInfo[]>;

  /**
   * 已注册的字体清单（同步读）。名字由字体文件自己报，工具栏照它建下拉框就行，
   *   不要另抄一份"下标 → 名字"。
   */
  fonts(): FontInfo[];

  /**
   * 在指定位置插入一段文本（模板填充），进撤销栈。
   * @param pos 插入位置
   * @param text 要插入的文本
   * @returns 有没有改动
   */
  insertText(pos: Pos, text: string): Promise<boolean>;

  /**
   * 在当前段之后插一张空表格，光标落进第一格（接着打字就是填这一格）。
   *
   * 表格是块级元素，只能落在两段之间；当前段的内容保持不变，表格自成一段段位。
   * 只在文档模式可用 —— 表单模式在填值、只读模式不许改，两者都不该长出新的表。
   * @param opts rows 行数、cols 每行格数、header 首行是否作为标题行
   * @returns 有没有插进去（模式不对或行列越界时 false，文档不动）
   */
  insertTable(opts: { rows: number; cols: number; header?: boolean }): Promise<boolean>;

  /**
   * 跑一条表格结构命令。主语是光标所在的那一格（或那一行 / 那一列）。
   *
   * 只在文档模式可用。被拒时文档一个字节都不动：光标不在表格里、删到只剩一行一列、
   *   行里有纵向合并（删行会连带删掉内容）、要合并的格子没铺满一个矩形、要并进去的格子
   *   还有内容 —— 这些都由引擎判，宿主不必先自己算一遍。
   * @param op 动词
   * @returns 有没有改（false = 被拒，文档没动）
   */
  tableOp(op: TableOp): Promise<boolean>;

  /**
   * 改光标所在格的记录（目前是垂直对齐）。
   *
   * 幂等：给的项与现状相同返回 true 但不进撤销栈。
   * @param opts 要改的项
   * @returns 有没有改
   */
  setTableCell(opts: TableCellOptions): Promise<boolean>;

  /**
   * 改光标所在行的记录：标题行、跨页断行、行前分页、行高与行高规则。
   *
   * 标题行只能是"从第 0 行起的连续前缀"：把第 2 行单独标成标题行会让这张表不再是合法形状。
   * @param opts 要改的项
   * @returns 有没有改
   */
  setTableRow(opts: TableRowOptions): Promise<boolean>;

  /**
   * 改光标所在表格的记录：宽度与水平位置。
   *
   * widthPt 传 0 表示撑满可用宽度（不是"零宽"）。
   * @param opts 要改的项
   * @returns 有没有改
   */
  setTable(opts: TableOptions): Promise<boolean>;

  /**
   * 光标所在表格的现状：表 / 行 / 格三层记录 + 整格选区范围。
   *
   * 一次读全：属性面板要同时显示这三层，分三次问会拿到三个不同时刻的状态。
   * @returns 现状；光标不在表格里时 inTable 为假
   */
  tableState(): Promise<TableState>;

  /**
   * 表格栅格：点所在的表的横向范围、列边界与这一页的行片段。
   *
   * 拖拽列边界 / 行高边界要用它换算指针位置。坐标是文档磅（与 hitTest 同一坐标系）。
   * @param xPt 文档坐标 x（磅）
   * @param yPt 文档坐标 y（磅）
   * @returns 栅格；不在表格里时 ok 为假
   */
  tableGrid(xPt: number, yPt: number): Promise<TableGrid>;

  /**
   * 把一张表的列宽钉成显式值（拖完列边界之后落地用）。
   *
   * 传 tableGrid() 的那一份和每列一个宽度（磅，按栅格列序）。列数必须与栅格的列边界
   * 数减一相同 —— 多一条少一条都会把表排歪，而"少一条"在画面上只表现为最后一列被
   * 拉满，看不出是宿主算错了，所以这里直接拒绝。
   * @param grid 这一张表的栅格（tableGrid 的返回值）
   * @param widths 每列宽度（磅）
   * @returns 有没有改
   */
  setTableColWidths(grid: TableGrid, widths: number[]): Promise<boolean>;

  /**
   * 改水印（第一节的页面设置）。只给要改的项，其余保持现状。
   *
   * 它是设计动作：只在文档模式生效，一步撤销。
   * @param opts 要改的项
   * @returns 有没有改
   */
  setWatermark(opts: WatermarkOptions): Promise<boolean>;

  /**
   * 当前水印。
   * @returns 水印记录；没有水印时 enabled 为假
   */
  watermark(): Promise<WatermarkState>;

  /**
   * 把表格现状摊成一张属性表单（表 / 行 / 格三层字段放在一起）。
   *
   * 宽度与行高的"自动"在面板上写 0 而不是 -1：用户不该看到负的磅值。
   * @param state 表格现状（tableState() 的那一份）
   * @param opts 标签替换表
   * @returns 字段表；不在表格里时 inTable 为假、字段为空
   */
  describeTable(state: TableState, opts?: FormOptions): TableForm;

  /**
   * 按一份面板值改表格：只提交真的变了的层。
   *
   * 三层各走各的写入口，但同一层里的多项是一次提交 —— 一次"确定"因此最多三小步。
   * @param values 面板读出来的值
   * @returns 有没有改动（不在表格里、或面板没改时返回 false）
   */
  applyTableForm(values: FormValues): Promise<boolean>;

  /**
   * 在光标处插一个内容控件（表单字段），光标落进它的内容末尾。
   *
   * 控件是行内容器：它不切断段落，落点就是光标处。只在文档模式可用（理由同上）。
   * @param opts 类型 / 业务编码 / 内容 / 只读 / 必填 / 值形态
   * @returns 有没有插进去（模式不对时 false，文档不动）
   */
  insertControl(opts?: ControlInsertOptions): Promise<boolean>;

  /**
   * 删掉一个控件。keepContent 为真时拆壳留内容（那一段文字变成普通正文）。
   *
   * 只在文档模式可用：删字段是设计动作。表单模式里结构默认锁住，调用会返回 false。
   * @param index 控件下标（form() 的第几项）
   * @param opts keepContent = 保留内容、只拆掉字段本身
   * @returns 有没有删掉
   */
  removeControl(index: number, opts?: { keepContent?: boolean }): Promise<boolean>;

  /**
   * 改一个控件的定义（选项表、标签、互斥组、自由输入、日期格式与单位、业务编码），
   *   以及只读与必填这两个运行态开关。
   *
   * 只改给出的项。改完内容按新定义重建；指向已删选项的值一起清掉 —— 都算在同一步撤销里。
   * 只在文档模式可用（改定义是设计动作）。
   * @param index 控件下标（form() 的第几项）
   * @param opts 要改的项
   * @returns 有没有改动
   */
  updateControl(index: number, opts?: ControlUpdateOptions): Promise<boolean>;

  /**
   * 改一个控件的值，并按类型重建它的显示内容（图形、标签、日期格式）。
   *
   * 文本类的值就是内容：给 text 会整段替换；选择类改的是记录上的值。
   * @param index 控件下标（form() 的第几项）
   * @param value 新值（只给要改的那几项）
   * @returns 有没有改动
   */
  setControlValue(index: number, value?: ControlValueInput): Promise<boolean>;

  /**
   * 切换一个选择类控件的第 option 个选项（单控件传 -1）。互斥与多选规则在引擎里。
   * @param index 控件下标
   * @param option 选项下标；单控件传 -1
   * @returns 有没有改动
   */
  toggleControl(index: number, option?: number): Promise<boolean>;

  /**
   * 在光标处插一张图片。图片是行内原子：与文字同流，参与断行。
   *
   * 只在文档模式可用。像素要先注册（见 registerImage）—— 没注册时画面上是一个带资源
   * 标识的占位框，盒子尺寸照旧。
   * @param opts 资源 id 与尺寸（像素 / 显示两组至少给一组）
   * @returns 有没有插进去
   */
  insertImage(opts?: ImageInsertOptions): Promise<boolean>;

  /**
   * 注册一张图片的像素。宿主解码（createImageBitmap / canvas）之后把 RGBA 交进来。
   *
   * 重复注册同一个 id 表示"这张图换了"：所有画着它的页重光栅。
   * @param opts 资源 id、像素宽高与 RGBA
   */
  registerImage(opts: ImageRegisterOptions): Promise<void>;

  /**
   * 删掉一张图片的像素。文档里的引用保留，画面上退回占位框。
   * @param id 资源 id
   */
  dropImage(id: string): Promise<void>;

  /**
   * 文档里全部图片（文档顺序）。
   * @returns 每张图的下标、资源 id、显示尺寸与落页矩形
   */
  images(): Promise<ImageBox[]>;

  /**
   * 命中：文档坐标（磅）落在哪张图的哪个把手上。
   * @param x 文档坐标 x（磅）
   * @param y 文档坐标 y（磅）
   * @returns 命中的图片与把手；没点到时 index 为 -1
   */
  imageHit(x: number, y: number): Promise<ImageHit>;

  /**
   * 选中一张图片：画面上出拖拽框与八个把手。传 -1 取消选中。
   *
   * 图片选区与文本选区互斥：任何光标定位、选区改动、文档提交都会清掉它。
   * @param index 图片下标
   */
  selectImage(index: number): Promise<void>;

  /**
   * 改一张图片的显示尺寸（磅）。尺寸落成绝对长度，按 DPI 折算的 Auto 就此钉住。
   * @param index 图片下标
   * @param widthPt 新宽度（磅）
   * @param heightPt 新高度（磅）
   * @param opts merge 为真时与上一帧合成一步撤销（拖拽的后续每一帧都传它）
   * @returns 有没有改动
   */
  resizeImage(index: number, widthPt: number, heightPt: number, opts?: { merge?: boolean }): Promise<boolean>;

  /**
   * 改一张图片的版式（环绕方式）。改的是断行，会整篇重排。
   *
   * 一次一步撤销；设成与现状相同的档位不产生修订。
   * @param index 图片下标
   * @param layout 版式档位
   * @returns 有没有改动
   */
  setImageLayout(index: number, layout: ImageLayout): Promise<boolean>;

  // 编辑（返回"有没有改动"）

  /**
   * 打一段字（走与键盘同一条命令，一次提交）。
   * @param text 要插入的文本
   * @returns 有没有改动
   */
  type(text: string): Promise<boolean>;

  /**
   * 向前删（退格）。
   * @param n 删几次，默认 1
   * @returns 有没有改动
   */
  deleteBackward(n?: number): Promise<boolean>;

  /**
   * 向后删（Delete）。
   * @param n 删几次，默认 1
   * @returns 有没有改动
   */
  deleteForward(n?: number): Promise<boolean>;

  /**
   * 在光标处断段。
   * @returns 有没有改动
   */
  splitParagraph(): Promise<boolean>;

  /**
   * 与相邻段合并。
   * @param dir 与哪一侧合并，默认 prev
   * @returns 有没有改动
   */
  mergeParagraph(dir?: 'prev' | 'next'): Promise<boolean>;

  /**
   * 撤销一步。
   * @returns 有没有改动（栈空时 false）
   */
  undo(): Promise<boolean>;

  /**
   * 重做一步。
   * @returns 有没有改动（没有可重做的时 false）
   */
  redo(): Promise<boolean>;

  // 剪贴板（与 Ctrl+C / Ctrl+X / Ctrl+V 同一条路）

  /**
   * 复制选区到系统剪贴板。片段里带样式、控件与表格，粘回本编辑器时是同一段内容。
   * @returns 复制成了没有（选区为空时 false，剪贴板内容不变）
   */
  copy(): Promise<boolean>;

  /**
   * 剪切：复制成功之后才删选区。
   * @returns 复制并删掉了没有
   */
  cut(): Promise<boolean>;

  /**
   * 粘贴：自家片段优先（保样式、控件、表格），其次纯文本（按行拆段）。
   *
   * 需要浏览器授权读剪贴板（键盘粘贴不需要）；被拒时返回 false。
   * @returns 粘上了没有
   */
  paste(): Promise<boolean>;

  /**
   * 增减缩进：正数插一格缩进，负数去掉一格（与按 Tab / Shift+Tab 同一条命令）。
   *
   * 有选区时先把光标收到选区左端再插 —— 缩进不该把选中的字换成空格。
   * @param levels 正数加深，负数变浅（绝对值是层级数）
   * @returns 有没有改动
   */
  setIndent(levels: number): Promise<boolean>;

  /**
   * 对选区应用字符格式。
   *
   * 需要非空选区：空选区没有可改的字符。返回 false 有两种情形 —— 命令没执行（只读模式）
   *   与格式本来就等于目标值（幂等，不占撤销栈）。
   * @param patch 只含要改的字段
   * @returns 是否真的改了
   */
  format(patch: CharFormatPatch): Promise<boolean>;

  /**
   * 对选区覆盖的段落应用段落格式；空选区时作用于光标所在的那一段。
   * @param patch 只含要改的字段
   * @returns 是否真的改了
   */
  paragraphFormat(patch: ParaFormatPatch): Promise<boolean>;

  /**
   * 读光标处的字符标记（工具栏回显）。
   *
   * 默认只报"这一处写了什么"，不含样式层与段落默认值：界面据此点亮按钮，
   *   而"看起来是不是粗的"由排版结果回答。
   * @param opts.effective 传 true 时报实际生效的那一份（样式链与默认值都算进去，
   *   字体与字号永远有值）—— 工具栏要显示"当前是什么字体、多少磅"就该读它
   * @returns 只含已显式设置的字段；effective 时上面那些字段全部给出实测值
   */
  formatAt(opts?: { effective?: boolean }): Promise<CharFormatPatch>;

  /**
   * 读光标所在段落的格式覆盖（工具栏回显）。
   * @returns 只含已显式设置的字段
   */
  paragraphFormatAt(): Promise<ParaFormatPatch>;

  // 光标与选区（程序化定位）

  /**
   * 当前光标。
   * @returns 位置、页号与页内矩形（同步读镜像）
   */
  caret(): Caret;

  /**
   * 程序化定位，并把光标滚进视野。
   * @param pos 目标位置
   * @param opts 侧别与行别（折行处同一个偏移压着两行）
   */
  setCaret(pos: Pos, opts?: SetCaretOptions): Promise<void>;

  /**
   * 按档位移动光标。
   * @param step 往哪儿走
   */
  moveCaret(step: CaretStep): Promise<void>;

  /**
   * 设置选区（不滚动：选区可以在任何地方）。
   * @param anchor 起点
   * @param focus 焦点端
   */
  select(anchor: Pos, focus: Pos): Promise<void>;

  /**
   * 全选。
   */
  selectAll(): Promise<void>;

  /**
   * 取消选区（焦点端不动）。
   */
  clearSelection(): Promise<void>;

  /**
   * 把键盘焦点交回编辑器。
   *
   * 宿主的工具栏、菜单、对话框都会把焦点拿走去处理点击；处理完要还回来，否则之后敲字
   *   与快捷键全落进那个按钮里。不滚视口（preventScroll）—— 只是还焦点，不该动画面。
   */
  focus(): void;

  /**
   * 当前选区。
   * @returns 两端位置与每页矩形（同步读镜像）
   */
  selection(): Selection;

  // 控件（表单）

  /**
   * 聚焦下一个 / 上一个控件。不是按序号：引擎的焦点只有"往前走/往回走"这一种操作，
   * 按序号聚焦得先知道每个控件的可编辑性（只读的会被跳过），那是引擎的判定不是宿主的。
   * @param dir 往前走还是往回走
   */
  focusControl(dir?: 'next' | 'prev'): Promise<void>;

  /**
   * 改控件属性。prop 是引擎的属性号（见 ControlProp），value 目前只有布尔。
   * @param index 控件序号
   * @param prop 属性号（见 ControlProp）
   * @param value 目标值
   * @returns 有没有改动（被锁住的控件返回 false）
   */
  setControlProp(index: number, prop: number, value: boolean): Promise<boolean>;

  /**
   * 切换一个开关型字符格式（粗体 / 斜体 / 下划线）。
   *
   * 取反按"光标处实际生效的值"算，不看宿主那份缓存 —— 缓存什么时候刷新完不该决定
   *   点下去会不会生效。连点两下按顺序落地，不会两次都读到改之前的值。
   *
   * 与 format 同一条边界：字符格式只作用在选区上，选区为空（只有光标）时不生效并返回
   *   false —— 引擎没有"打字样式"这一层，光标处改格式不是"改一个空区间"。
   * @param field 哪个开关
   * @returns 有没有改动（没有选区时为 false）
   */
  toggleFormat(field: 'bold' | 'italic' | 'underline'): Promise<boolean>;

  /**
   * 把光标处（或选中范围内）的字号按一份阶梯调大一档或小一档。
   *
   * 阶梯归宿主：哪几档是产品口味，不是引擎的事。当前值落在两档之间时取它上方（下方）
   *   的最近一档。与 toggleFormat 同一条边界：没有选区时不生效。
   * @param dir 1 = 变大，-1 = 变小
   * @param sizes 字号阶梯（升序降序都行，库自己排）
   * @returns 有没有改动（没有选区、或已经在最大 / 最小档时返回 false）
   */
  stepFontSize(dir: 1 | -1, sizes: number[]): Promise<boolean>;

  /**
   * 文档规模：码点数与字数。
   *
   * 两个口径都在引擎里，换一个宿主也是同样的数。字数不计空白，控件内容与表格里的字
   *   算在内 —— 它们就是流里的文字；图片这类原子不产字符。
   * @returns chars 是全部码点，words 是非空白码点
   */
  counts(): { chars: number; words: number };

  /**
   * 控件清单。
   * @returns 按文档顺序的控件（只读视图）
   */
  form(): ControlSummary[];

  /**
   * 把一个控件摊成属性面板的字段表。
   *
   * 给哪些项、每项是什么种类、当前值是什么，全按控件类型算 —— 宿主只负责画。
   *   这是纯数据，不读写文档；现读一份随参数给进来，面板显示的就是菜单看到的那一拍。
   * @param control 控件现状（form() 的一项）
   * @param opts 标签替换表
   * @returns 字段表（不认识类型时只有行为那几项）
   */
  describeControl(control: ControlSummary, opts?: FormOptions): ControlForm;

  /**
   * 按一份面板值改控件：只提交真的变了的项。
   *
   * 值的分流由库定（内容走内容、定义走定义、只读与必填走运行态开关），宿主不必记；
   *   一次"确定"因此是最少的提交步数，撤销不用按三次。
   * @param index 控件下标（form() 的第几项）
   * @param values 面板读出来的值
   * @returns 有没有改动（面板没改就返回 false）
   */
  applyControlForm(index: number, values: FormValues): Promise<boolean>;

  // 视图

  /**
   * 视口与页度量。
   * @returns 页宽高、间隙、页数、窗口（同步读镜像）
   */
  geometry(): Geometry;

  /**
   * 滚到某页或某个纵向位置（CSS 像素）。
   * @param target page 走页号，y 走纵向位置（给了 y 就用 y）
   */
  scrollTo(target: { page?: number; y?: number }): Promise<void>;

  /**
   * 转发 configure({ zoom })，为可读性保留。
   * @param zoom 倍数（1 = 100%），越界夹到 0.375~2.25
   */
  setZoom(zoom: number): Promise<void>;

  // 配置

  /**
   * 唯一配置写入口：一次调用 = 一次原子提交，只做最贵的那一档代价。
   * @param patch 要改的项（不认识的键同步抛 TypeError）
   * @returns 生效后的完整配置（越界值由引擎夹取）
   */
  configure(patch: Partial<EditorConfig>): Promise<EditorConfig>;

  /**
   * 当前配置。
   * @returns 生效值的只读快照
   */
  config(): Readonly<EditorConfig>;

  /**
   * 把页面设置摊成属性面板的字段表。
   *
   * 边距与宽高按厘米给值（人按厘米想页边距），折回磅在 applyPageSetupForm 里做。
   *   纸张不是自定义时，宽高两项带 disabled —— 宿主按纸张字段的变化再开一次。
   * @param opts 标签替换表
   * @returns 字段表与面板标题
   */
  describePageSetup(opts?: FormOptions): PageSetupForm;

  /**
   * 按一份面板值改页面设置。
   *
   * 与现状相同就不调用 configure：整篇重排是这一层最贵的一档，面板没改不该付它。
   * @param values 面板读出来的值
   * @returns 有没有改动
   */
  applyPageSetupForm(values: FormValues): Promise<boolean>;

  // 事件与生命周期

  /**
   * 订阅；返回退订函数。
   * @param event 事件名
   * @param cb 回调
   @returns 退订函数
   */
  on<K extends keyof EditorEvents>(event: K, cb: (ev: EditorEvents[K]) => void): () => void;

  /**
   * 等队列排空（批量 fire-and-forget 之后用它一次，比逐个 await 便宜）。
   */
  flush(): Promise<void>;

  /**
   * 收起定时器、释放 Worker、移除 DOM。之后所有调用 reject。
   */
  destroy(): Promise<void>;

  /**
   * 宿主要的那一个 DOM 入口（内部画布/输入层不对外）。
   */
  readonly element: HTMLElement;

  /**
   * 诊断面。只有 createEditor({ debug: true }) 时存在；否则访问抛 TypeError。
   * 探针与审计用，产品代码不该依赖它。
   */
  readonly diag: Diag;
}

/**
 * 造一个编辑器。返回时已就绪（排版跑完、首帧画完、输入层已接上）。
 * @param opts 挂载点、传输、素材、初始配置
 * @returns 已就绪的编辑器实例
 */
export declare function createEditor(opts: EditorOptions): Promise<Editor>;

declare global {
  interface Window {
    /**
     * <script src="heditor.js"> 之后挂在全局（UMD 产物）。
     */
    HEditor: {
      createEditor: typeof createEditor;
    };
  }
}
