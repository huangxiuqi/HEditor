// 弹层：挂在某个按钮下面的一块面板。
//
// 只剩"内容与开关"两层：定位与关闭时机在 layer（右键菜单与属性面板共用那一份），这里
//   负责"打开前让调用方把内容塞进来、关掉时清空"。加第三个会开面板的按钮时，只要在动作
//   表里写一条，DOM 与事件都不用再碰。
import { createLayer } from './layer';

/**
 * 一块弹层。
 */
export interface Popover {
  /**
   * 面板根元素（已挂在 body 上，未打开时 display:none）。
   */
  el: HTMLElement;

  /**
   * 在锚点下面打开。
   * @param anchor 锚点元素（按钮）
   */
  open(anchor: HTMLElement): void;

  /**
   * 居中打开（表单类面板用这一档：标题栏拖得动、带遮罩、滚正文时不跑）。
   * @param handle 拖动把手（面板的标题栏）
   */
  openCentered(handle?: HTMLElement): void;

  /**
   * 关掉（幂等）。
   */
  close(): void;

  /**
   * 现在是否打开。
   * @returns 真 = 打开着
   */
  isOpen(): boolean;
}

/**
 * 造一块弹层。
 * @param onClose 关掉之后的回调（清理面板内容用）
 * @returns 弹层
 */
export function createPopover(onClose?: () => void): Popover {
  const layer = createLayer('heditor-popover', onClose);
  return {
    el: layer.el,
    open: (anchor) => {
      layer.el.classList.remove('heditor-popover--dialog');
      layer.openAt(anchor);
    },
    openCentered: (handle) => {
      // 内边距归面板自己排（标题栏要贴边），所以换一个修饰类。
      layer.el.classList.add('heditor-popover--dialog');
      layer.openCentered({ scrim: true });
      if (handle) layer.dragBy(handle);
    },
    close: () => layer.close(),
    isOpen: () => layer.isOpen(),
  };
}
