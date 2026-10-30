// 外壳装配：把功能区与状态栏装进页面，并把两者的回显接到一个实例上。
//
// 这一层只做三件事：摆位置、连回调、把 refresh 转发下去。它不含任何格式判定 ——
//   判定在页面的动作表里（main.ts），所以这一层在"没有实例"的页面上也照样能挂。
import { createRibbon } from './ribbon';
import { createStatusBar, type StatusBar } from './statusbar';
import type { Chrome, ChromeHost, Tab } from './types';

// 缩放的上下限，与库那一侧同一组数（这里只判一次夹取，不改语义）。
const ZOOM_MIN = 0.375;
const ZOOM_MAX = 2.25;

/**
 * 装外壳：顶部功能区进 host，底部状态栏进 body。
 *
 * 状态栏挂 body 末尾而不是 host 里：host 是页面骨架的第一段（编辑区在上面），
 *   而状态栏要在编辑区下面 —— 页面骨架只有"头 + 编辑区"两段，不加第三段就得挂 body。
 * @param host 放功能区的元素（页面里的 header）
 * @param title 左边的产品名
 * @param tabs 页签表
 * @returns 外壳的三个入口
 */
export function mountChrome(host: HTMLElement, title: string, tabs: Tab[]): Chrome {
  host.className = 'heditor-ribbon';
  host.innerHTML = '';

  const ribbon = createRibbon(tabs);
  const head = ribbon.el.querySelector<HTMLElement>('.heditor-ribbon__head');
  const name = document.createElement('span');
  name.className = 'heditor-ribbon__name';
  name.textContent = title;
  if (head) head.prepend(name);
  host.appendChild(ribbon.el);

  let editor: ChromeHost | null = null;
  let bar: StatusBar | null = null;

  /**
   * 把焦点交回编辑器。
   *
   * 按钮自己不抢焦点（见 button.ts），但下拉框必须拿到焦点才能展开 —— 选完焦点留在
   *   那个 select 上，之后敲字与快捷键就全落进它肚子里。所以外壳这一层统一补一次：
   *   组件不认实例，只认"动作做完该回到哪"。
   *
   * 例外是菜单：右键菜单与工具条上的下拉开着时，焦点归它（键盘上下与回车在那份实现里）。
   *   抢回来的话菜单看着是开的，方向键却把正文的光标挪走了 —— 同一下按键两处都响应。
   *   菜单关掉时自己会把键盘还回来（Dropdown 的 closed、右键菜单的 onClose）。
   */
  const refocus = (): void => {
    if (!editor) return;
    const active = document.activeElement;
    if (active instanceof Element && active.closest('.heditor-menu') !== null) return;
    editor.focus();
  };
  ribbon.el.addEventListener('click', (ev) => {
    // 点在"自己要拿焦点的控件"上时不能抢：原生下拉靠焦点展开，一抢就"点开就收回去"，
    //   根本选不了项。下拉框外面还包着一层 label（图标与值的整格），点在那上面同理。
    //   它收尾走下面那条 change。
    const t = ev.target instanceof Element ? ev.target : null;
    if (t && t.closest('select, input, textarea, label')) return;
    refocus();
  });
  ribbon.el.addEventListener('change', refocus);

  /**
   * 把缩放读数追平（实例还没绑上时什么都不做）。
   */
  const syncZoom = (): void => {
    if (editor && bar) bar.setZoomPct(editor.config().zoom * 100);
  };

  /**
   * 按一步改缩放；回来之后再追平读数。
   * @param z 目标倍数（已按上下限夹过）
   */
  const zoomTo = (z: number): void => {
    if (!editor) return;
    const clamp = Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, z));
    void editor.configure({ zoom: clamp }).then(syncZoom);
  };

  bar = createStatusBar(
    (dir) => {
      if (editor) zoomTo(editor.config().zoom + dir * 0.125);
    },
    (pct) => zoomTo(pct / 100),
  );
  document.body.appendChild(bar.el);

  const said = bar;
  return {
    /**
     * 写一行状态。
     * @param text 要说的话
     */
    say(text: string): void {
      said.say(text);
    },

    /**
     * 当前那一行状态。
     * @returns 状态栏文字
     */
    text(): string {
      return said.text();
    },

    /**
     * 绑一个实例：状态栏的缩放读数与滑块跟着它走。
     * @param ed 编辑器实例
     */
    bind(ed: ChromeHost): void {
      editor = ed;
      syncZoom();
    },

    /**
     * 写左下角的页数与字数。
     *
     * 这两个数由页面算好喂进来：外壳不读文档内容，只负责把它摆到该在的位置。
     * @param pages 总页数
     * @param words 总字数
     */
    setCounts(pages: number, words: number): void {
      said.setCounts(pages, words);
    },

    /**
     * 重读一遍回显。
     */
    refresh(): void {
      ribbon.update();
      syncZoom();
    },
  };
}
