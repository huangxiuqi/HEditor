// 工具条的描述层：这一层只说"有什么动作"，不碰 DOM。
//
// 分成"描述"与"组件"两半的理由：动作表要能单独读（它是这一页能做什么的清单），
//   而组件只管把描述变成元素与回显。两半混在一起时，加一个按钮要同时改 DOM、
//   事件与状态刷新三处，改错一次就是"按钮点得动但回显不动"。

/**
 * 一个按钮。
 */
export interface Button {
  /**
   * 稳定标识（页签内唯一，组件把它写到 data-act 上）。
   */
  act: string;

  /**
   * 按钮的名字：大按钮显示在图标下面，小按钮只在悬停提示里出现。
   */
  label: string;

  /**
   * 图标名，对应 web/app/icons 下的文件名（不含扩展名）。不填就只显示文字。
   */
  icon?: string;

  /**
   * 大按钮：图标在上、文字在下。小按钮只有图标。
   */
  big?: boolean;

  /**
   * 回显：返回真表示"按下了"（粗体开着、连页开着）。
   */
  on?(): boolean;

  /**
   * 按下去做什么（返回值没用，按钮只关心它跑完了没有）。
   */
  run(): unknown;
}

/**
 * 一个下拉框。
 */
export interface Choice {
  /**
   * 稳定标识（同上）。
   */
  act: string;

  /**
   * 名字：写进悬停提示，不占版面（下拉框里那一格已经被值占满了）。
   */
  label: string;

  /**
   * 给下拉框配一个图标（不填就只有值）。
   */
  icon?: string;

  /**
   * 放宽一格（装得下"思源黑体"这种四个字的取值）。
   */
  wide?: boolean;

  /**
   * 选项；value 为空串表示"没有显式值"。可以给一个函数：候选随状态变的下拉框
   * （字体那一格按已注册清单现算）在每次回显时重读它。
   */
  options: Array<{ value: string; text: string }> | (() => Array<{ value: string; text: string }>);

  /**
   * 回显：当前该选哪个。
   */
  value(): string;

  /**
   * 选中之后做什么。
   * @param value 选中的值（可能是空串）
   */
  run(value: string): unknown;
}

/**
 * 竖着叠起来的一小组按钮（大按钮右边那一列）。
 *
 * 它存在的理由是对齐：大按钮高 54，旁边两个小按钮只有 27，摊平了摆会一个贴顶
 *   一个贴底；叠起来之后两者同高，组名也就落在同一条线上。
 */
export interface Stack {
  /**
   * 叠着的按钮，从上到下。
   */
  items: Button[];
}

/**
 * 会开面板的按钮（按下去在它下面弹出一块面板，比如表格尺寸的网格）。
 *
 * 与 Choice 的分工：取值是一个离散档位就用下拉框，取值是一块"要看着挑"的版面
 *   （行列数、取色板）就用面板。两者都不该塞进 Button 里靠 act 特判。
 */
export interface Menu {
  /**
   * 稳定标识（同上）。
   */
  act: string;

  /**
   * 按钮的名字（大按钮显示在图标下面）。
   */
  label: string;

  /**
   * 图标名（同上）。
   */
  icon?: string;

  /**
   * 大按钮（同上）。
   */
  big?: boolean;

  /**
   * 回显：返回真表示"按下了"。
   */
  on?(): boolean;

  /**
   * 造面板内容。
   * @param dismiss 关掉面板（选完就关，与点空白关闭同一条时机）
   * @returns 面板组件
   */
  panel(dismiss: () => void): Component;
}

/**
 * 组里的一项。
 */
export type Item = Button | Choice | Stack | Menu;

/**
 * 一组同类动作（组名显示在组的下面）。
 */
export interface Group {
  /**
   * 组名。
   */
  label: string;

  /**
   * 组里的动作，从左到右。
   */
  items: Item[];
}

/**
 * 一个页签。
 */
export interface Tab {
  /**
   * 页签上的字。
   */
  label: string;

  /**
   * 这一页的动作组。
   */
  groups: Group[];
}

/**
 * 一个组件：一个元素加一个刷新入口。
 *
 * 刷新不是"重建 DOM"：重建会丢掉焦点与按钮的按下态，而且在光标每移动一次都要跑一遍
 *   的场景里，重建出来的元素会让点击落到已经被替换掉的那个上。
 */
export interface Component {
  /**
   * 组件的根元素。
   */
  el: HTMLElement;

  /**
   * 重读一遍回显。
   */
  update(): void;
}

/**
 * 外壳对页面的接口。
 */
export interface Chrome {
  /**
   * 写一行状态。
   * @param text 要说的话
   */
  say(text: string): void;

  /**
   * 当前那一行状态（判据读它判"页面看起来在做什么"）。
   * @returns 状态栏文字
   */
  text(): string;

  /**
   * 绑一个实例：状态栏的缩放读数与滑块跟着它走。
   * @param editor 编辑器实例
   */
  bind(editor: ChromeHost): void;

  /**
   * 重读一遍回显。
   *
   * on / value 是同步闭包（它们读页面自己缓存的那份格式），所以调用方要在数据准备好
   *   之后自己叫一声 —— 让这里发查询的话，一次光标移动会变成七八个来回。
   */
  refresh(): void;

  /**
   * 写左下角的页数与字数。
   * @param pages 总页数
   * @param words 总字数（字符数，不计空白）
   */
  setCounts(pages: number, words: number): void;
}

/**
 * 外壳认识的编辑器侧面：只要缩放与改缩放这两件事。
 *
 * 不写 Editor 全量：组件不该为了"改个缩放"而依赖整个编辑器契约，那会让这一层
 *   在契约变动时跟着动，而它其实与文档内容无关。
 */
export interface ChromeHost {
  /**
   * 当前配置快照。
   */
  config(): { zoom: number };

  /**
   * 改缩放倍数。
   * @param z 目标倍数
   */
  setZoom(z: number): Promise<void>;

  /**
   * 把焦点交回编辑器（动作做完之后要把键盘还回去）。
   */
  focus(): void;
}

/**
 * 是不是下拉框。
 * @param item 组里的一项
 * @returns 真 = 下拉框
 */
export function isChoice(item: Item): item is Choice {
  const options = (item as Choice).options;
  return Array.isArray(options) || typeof options === 'function';
}

/**
 * 是不是竖着叠的一列按钮。
 * @param item 组里的一项
 * @returns 真 = 一列按钮
 */
export function isStack(item: Item): item is Stack {
  return Array.isArray((item as Stack).items);
}

/**
 * 是不是会开面板的按钮。
 * @param item 组里的一项
 * @returns 真 = 会开面板
 */
export function isMenu(item: Item): item is Menu {
  return typeof (item as Menu).panel === 'function';
}
