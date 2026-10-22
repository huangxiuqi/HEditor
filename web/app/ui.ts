// 示例页的工具条与状态栏。
//
// 抽成一份而不是写在 main.ts 里：动作表是"这一页能做什么"的清单，与"怎么接编辑器"
//   是两件事。接线的部分在 main.ts，这里只管 DOM 与事件委托。

/**
 * 一个工具条动作。
 */
export interface Action {
  /**
   * 按钮上显示的文字。
   */
  label: string;

  /**
   * data-act 的值（事件委托按它分发）。
   */
  act: string;

  /**
   * 按下去做什么（返回值没用，工具条只关心它跑完了没有）。
   */
  run(): unknown;
}

/**
 * 造工具条与状态栏。
 *
 * @param host 放工具条的元素（页面里的 header）
 * @param title 左边那行标题
 * @param actions 按钮表
 * @returns 状态栏的两个入口：写一行状态、读当前那一行
 */
export function mountChrome(host: HTMLElement, title: string, actions: Action[]) {
  host.innerHTML = '';
  const h1 = document.createElement('h1');
  h1.textContent = title;
  host.appendChild(h1);

  const byAct = new Map<string, Action>();
  for (const a of actions) {
    byAct.set(a.act, a);
    const btn = document.createElement('button');
    btn.dataset.act = a.act;
    btn.textContent = a.label;
    host.appendChild(btn);
  }

  const status = document.createElement('span');
  status.id = 'status';
  status.textContent = '启动中…';
  host.appendChild(status);

  host.addEventListener('click', (ev) => {
    const act = (ev.target as HTMLElement | null)?.dataset?.act;
    const action = act ? byAct.get(act) : undefined;
    if (action) void action.run();
  });

  return {
    /**
     * 写一行状态。
     *
     * @param text 要说的话
     */
    say(text: string): void {
      status.textContent = text;
    },

    /**
     * 当前那一行状态（探针读它判"页面看起来在做什么"）。
     *
     * @returns 状态栏文字
     */
    text(): string {
      return status.textContent ?? '';
    },
  };
}
