// 宿主侧的错误降级层：把失败变成页面上看得见的东西。
//
// 为什么要有独立的一层：引擎的失败只有错误事件与内部数组知道，而事件不一定有人监听。
//   于是"引擎起不来""Worker 死了""这条命令失败了"在页面上完全看不见 —— 页面要么空白、
//   要么停在一个看起来还能输入、实际打不进字的编辑器上。失败必须分出两档，因为对用户的
//   含义完全不同：
//
//   致命：引擎已经不可用（胶水层取不到、Worker 崩溃、实例化抛异常）。呈现为常驻横幅，
//   并把容器标成不可用；此后命令一律快速失败，不再假装能编辑。
//
//   降级：单次操作失败，引擎还活着（未知命令、查询被拒、一次请求超时、输入链被拒）。
//   呈现为一条会自动消失的提示条，编辑器继续可用。
//
// 这一层自己绝不允许抛异常：错误显示代码炸掉会把"看得见的失败"变回"看不见的失败"，
//   所以每个 DOM 操作都各自收在 guard 里。
//
// 与判据的约定：每块界面都带 data-host-error 属性（值为 fatal 或 warn），
//   role 分别是 alert 与 status —— 探针按这两个属性取证，不依赖样式或文案。

/**
 * 一条降级记账：探针与产品面板都可以列出来。
 */
export interface ErrorEntry {
  level: 'fatal' | 'warn';
  text: string;
  at: number;
  count: number;
  detail: string;
}

/**
 * 错误面。fatal 之后不该再被调用（引擎已经不可用），但调用它也不会抛。
 */
export interface ErrorSurface {
  /**
   * 记一条致命失败：常驻横幅 + 容器标记。
   * @param message 标题（一句话说清哪里坏了）
   * @param info 详情、下一步、恢复入口
   * @returns 这一条记账
   */
  fatal(message: string, info?: FatalInfo): ErrorEntry;

  /**
   * 记一条降级失败：会自动消失的提示条。
   * @param message 提示文字
   * @returns 这一条记账
   */
  warn(message: string): ErrorEntry;

  /**
   * 抹掉界面（不抹记账，判据要能事后取证）。
   */
  clear(): void;

  /**
   * 记账快照（按发生顺序）。
   * @returns 每条失败的等级、文字与时间
   */
  entries(): ErrorEntry[];

  /**
   * 横幅是否还挂着。
   * @returns true = 引擎整条不可用
   */
  fatalActive(): boolean;
}

export interface FatalInfo {
  detail?: string;
  hint?: string;

  /**
   * 恢复入口（通常是重新加载页面）。只有它存在时横幅上才出现按钮。
   */
  retry?: () => void;
}

// 同类警告在这段时间内合并计数，不重复堆叠。
const DUP_MS = 800;
// 提示条自动消失的时间。
const WARN_TTL_MS = 6000;
// 页面上最多同时挂几条提示条（超出的从最早的开始移除）。
const MAX_WARN = 4;
const STYLE_ID = 'heditor-error-style';

/**
 * 只注入一次的内联样式。
 *
 * 自带样式而不是依赖页面 CSS：这一层要能在任意宿主页面上工作，包括那些根本没有样式表
 *   的产品页；顺带保证"降级界面一定可见"不取决于谁忘了写 CSS。
 */
function ensureStyle(doc: Document): void {
  if (doc.getElementById(STYLE_ID)) return;
  const st = doc.createElement('style');
  st.id = STYLE_ID;
  st.textContent = [
    '[data-host-error]{box-sizing:border-box;font:13px/1.55 "Segoe UI","Microsoft YaHei",sans-serif}',
    '[data-host-error="fatal"]{position:absolute;z-index:20;left:12px;right:12px;top:12px;',
    'padding:10px 12px;border:1px solid #b54708;border-left-width:4px;border-radius:6px;',
    'background:#fff8f1;color:#1b1f24;box-shadow:0 2px 8px rgba(0,0,0,.16)}',
    '[data-host-error="fatal"] .heditor-error__title{font-weight:600;color:#b54708}',
    '[data-host-error="fatal"] .heditor-error__detail{margin-top:4px;white-space:pre-wrap;word-break:break-word}',
    '[data-host-error="fatal"] .heditor-error__hint{margin-top:6px;color:#5b6572}',
    '[data-host-error="warn"]{position:absolute;z-index:19;left:12px;right:12px;bottom:12px;',
    'padding:6px 10px;border:1px solid #d8dde4;border-left:4px solid #b54708;border-radius:6px;',
    'background:rgba(255,255,255,.97);color:#1b1f24;box-shadow:0 1px 4px rgba(0,0,0,.12)}',
    '[data-host-fatal="1"]{outline:1px dashed #b54708;outline-offset:-2px}',
  ].join('');
  (doc.head || doc.documentElement).appendChild(st);
}

// 所有对外动作都从这里走：这一层抛异常比不显示更糟。
function guard<T>(fn: () => T): T | undefined {
  try {
    return fn();
  } catch {
    return undefined;
  }
}

/**
 * 错误界面挂在哪、标在哪。
 *
 * mount 要是一个定位过的元素（库给的是 .heditor-body）—— 横幅与提示条都是绝对定位，
 *   挂在没有定位的祖先上会跑到视口四角去。mount 被移除之后回落到 body，避免"实例已经
 *   不在页面上、错误无处显示"。
 * mark 是调用方给的那个容器：它带上 data-host-fatal 属性，页面 CSS 可以据此整块变灰。
 */
export interface SurfaceTargets {
  mount: HTMLElement;
  mark: HTMLElement;

  /**
   * 一条记账被记下时回调。
   * @param entry 这一条
   */
  onEntry?(entry: ErrorEntry): void;
}

/**
 * 造错误面：横幅挂 mount，容器标记打 mark。
 * @param targets 挂载点、标记点与记账回调
 * @returns 错误面句柄
 */
export function createErrorSurface(targets: SurfaceTargets): ErrorSurface {
  const { mount: container, mark } = targets;
  const onEntry = targets.onEntry;
  const doc = container.ownerDocument;
  const entries: ErrorEntry[] = [];

  let banner: HTMLElement | null = null;
  let toastEls: HTMLElement[] = [];

  const rootOf = (): HTMLElement =>
    container.isConnected ? container : (doc.body ?? doc.documentElement);

  function record(level: 'fatal' | 'warn', message: string, info?: FatalInfo): ErrorEntry {
    const at = Date.now();
    const text = String(message ?? '');
    const last = entries[entries.length - 1];
    let entry: ErrorEntry;
    // 同一条消息在极短时间内重复出现时只累加计数：引擎一次失败常会连带刷出好几条
    //   同源错误，逐条堆叠会把界面淹掉，反而看不到第一条。
    if (last && last.level === level && last.text === text && at - last.at < DUP_MS) {
      last.count += 1;
      last.at = at;
      entry = last;
    } else {
      entry = { level, text, at, count: 1, detail: String(info?.detail ?? '') };
      entries.push(entry);
    }
    if (onEntry) guard(() => onEntry(entry));
    return entry;
  }

  function renderFatal(entry: ErrorEntry, info?: FatalInfo): void {
    guard(() => {
      ensureStyle(doc);
      if (!banner) {
        banner = doc.createElement('div');
        banner.setAttribute('data-host-error', 'fatal');
        banner.setAttribute('role', 'alert');
        banner.innerHTML = '<div class="heditor-error__title"></div><div class="heditor-error__detail"></div>' +
                           '<div class="heditor-error__hint"></div>';
        rootOf().appendChild(banner);
      }
      // 按钮只在"调用方给了恢复方式"时才出现：凭空给一个点了没反应的按钮比不给更糟。
      const oldBtn = banner.querySelector('[data-he-retry]');
      if (info?.retry) {
        const btn = (oldBtn as HTMLButtonElement | null) ?? doc.createElement('button');
        if (!oldBtn) {
          btn.setAttribute('data-he-retry', '');
          btn.textContent = '重试';
          banner.appendChild(btn);
        }
        btn.onclick = () => guard(() => info.retry?.());
      } else if (oldBtn) {
        oldBtn.remove();
      }
      banner.querySelector('.heditor-error__title')!.textContent =
        entry.count > 1 ? `${entry.text}（${entry.count} 次）` : entry.text;
      banner.querySelector('.heditor-error__detail')!.textContent = entry.detail || info?.detail || '';
      banner.querySelector('.heditor-error__hint')!.textContent = info?.hint ?? '';
      mark.setAttribute('data-host-fatal', '1');
    });
  }

  function renderWarn(entry: ErrorEntry): void {
    guard(() => {
      ensureStyle(doc);
      const el = doc.createElement('div');
      el.setAttribute('data-host-error', 'warn');
      el.setAttribute('role', 'status');
      el.textContent = entry.count > 1 ? `${entry.text}（${entry.count} 次）` : entry.text;
      rootOf().appendChild(el);
      toastEls.push(el);
      while (toastEls.length > MAX_WARN) {
        toastEls.shift()?.remove();
      }
      setTimeout(() => {
        toastEls = toastEls.filter((x) => x !== el);
        el.remove();
      }, WARN_TTL_MS);
    });
  }

  return {
    fatal(message, info) {
      const e = record('fatal', message, info);
      renderFatal(e, info);
      return e;
    },
    warn(message) {
      const e = record('warn', message);
      renderWarn(e);
      return e;
    },
    // 抹掉界面（不抹 entries，判据要能事后取证）。实例销毁时用。
    clear() {
      guard(() => {
        banner?.remove();
        banner = null;
        for (const el of toastEls) el.remove();
        toastEls = [];
        mark.removeAttribute('data-host-fatal');
      });
    },
    entries: () => entries.slice(),
    fatalActive: () => !!banner,
  };
}
