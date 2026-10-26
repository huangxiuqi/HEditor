// 滑块：一条轨道加一个可拖的圆点。
//
// 为什么不用原生 <input type="range">：它画出来是浏览器自己那一套（Chrome 是一条细沟、
//   Safari 是另一套高度），放在这条深色状态栏上宽窄与高度都对不齐；而且拿不到"拖动中"
//   与"松手"的分界 —— 缩放要跟着拖动连续变，但落点只该记一次。
//
// 位置与值的换算是这一层唯一的知识：轨道两端各留半个圆点，否则拖到端点时圆点会切掉半个。

/**
 * 轨道两端各留多少（像素），等于圆点半径。
 */
const kThumbHalf = 7;

/**
 * 造一个滑块。
 * @param spec 量程与回调
 * @returns 元素与操作入口
 */
export function createSlider(spec: {
  min: number;
  max: number;
  step: number;
  value: number;
  title?: string;
  onInput(value: number): void;
  onChange?(value: number): void;
}): {
  el: HTMLElement;
  setValue(v: number): void;
  value(): number;
  setDisabled(off: boolean): void;
} {
  let value = clamp(spec.value);

  const el = document.createElement('div');
  el.className = 'heditor-slider';
  el.tabIndex = 0;
  if (spec.title) {
    el.title = spec.title;
    el.setAttribute('aria-label', spec.title);
  }
  el.setAttribute('role', 'slider');
  el.setAttribute('aria-valuemin', String(spec.min));
  el.setAttribute('aria-valuemax', String(spec.max));

  const track = document.createElement('div');
  track.className = 'heditor-slider__track';
  const fill = document.createElement('div');
  fill.className = 'heditor-slider__fill';
  const thumb = document.createElement('div');
  thumb.className = 'heditor-slider__thumb';
  track.append(fill, thumb);
  el.appendChild(track);

  /**
   * 夹进量程并按步长量化。
   * @param v 想要的值
   * @returns 落得到的值
   */
  function clamp(v: number): number {
    const lo = Math.min(spec.min, spec.max);
    const hi = Math.max(spec.min, spec.max);
    const stepped = Math.round((v - lo) / spec.step) * spec.step + lo;
    // 量化会引入二进制小数（0.1 的整数倍），收一下小数位再交出去。
    return Math.min(hi, Math.max(lo, Math.round(stepped * 1000) / 1000));
  }

  /**
   * 把值画到位上。
   */
  const paint = (): void => {
    const span = spec.max - spec.min;
    const pct = span === 0 ? 0 : ((value - spec.min) / span) * 100;
    fill.style.width = `${pct}%`;
    thumb.style.left = `${pct}%`;
    el.setAttribute('aria-valuenow', String(value));
  };
  paint();

  /**
   * 把一个客户端 x 换算成值。
   * @param clientX 指针的客户端 x
   * @returns 落得到的值
   */
  const at = (clientX: number): number => {
    const r = el.getBoundingClientRect();
    const usable = Math.max(1, r.width - kThumbHalf * 2);
    const ratio = (clientX - r.left - kThumbHalf) / usable;
    return clamp(spec.min + Math.min(1, Math.max(0, ratio)) * (spec.max - spec.min));
  };

  /**
   * 改值并通知（值没变就不通知：拖动过程中大部分 move 落在同一档上）。
   * @param next 新值
   * @param commit 真 = 这是一次松手（要额外发 change）
   */
  const set = (next: number, commit: boolean): void => {
    if (next !== value) {
      value = next;
      paint();
      spec.onInput(value);
    }
    if (commit && spec.onChange) spec.onChange(value);
  };

  el.addEventListener('pointerdown', (ev) => {
    if (ev.button !== 0 || el.classList.contains('is-disabled')) return;
    ev.preventDefault();
    el.focus({ preventScroll: true });
    el.classList.add('is-dragging');
    el.setPointerCapture(ev.pointerId);
    set(at(ev.clientX), false);
    const move = (e: PointerEvent): void => set(at(e.clientX), false);
    const up = (e: PointerEvent): void => {
      if (el.hasPointerCapture(e.pointerId)) el.releasePointerCapture(e.pointerId);
      el.removeEventListener('pointermove', move);
      el.removeEventListener('pointerup', up);
      el.removeEventListener('pointercancel', up);
      el.classList.remove('is-dragging');
      set(at(e.clientX), true);
    };
    el.addEventListener('pointermove', move);
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
  });

  el.addEventListener('keydown', (ev) => {
    const big = spec.step * 10;
    let next: number | null = null;
    if (ev.key === 'ArrowRight' || ev.key === 'ArrowUp') next = clamp(value + spec.step);
    else if (ev.key === 'ArrowLeft' || ev.key === 'ArrowDown') next = clamp(value - spec.step);
    else if (ev.key === 'PageUp') next = clamp(value + big);
    else if (ev.key === 'PageDown') next = clamp(value - big);
    else if (ev.key === 'Home') next = clamp(spec.min);
    else if (ev.key === 'End') next = clamp(spec.max);
    if (next === null) return;
    ev.preventDefault();
    ev.stopPropagation();
    set(next, true);
  });

  return {
    el,

    /**
     * 从外面写一个值（快捷键改缩放、窗口变化时重新夹取）。
     * @param v 新值
     */
    setValue(v: number): void {
      const next = clamp(v);
      if (next === value) return;
      value = next;
      paint();
    },

    value: () => value,

    /**
     * 置灰。
     * @param off 真 = 不可用
     */
    setDisabled(off: boolean): void {
      el.classList.toggle('is-disabled', off);
      el.tabIndex = off ? -1 : 0;
    },
  };
}
