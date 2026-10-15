// 宿主侧的错误降级层：把失败变成页面上看得见的东西。
//
// 为什么要有独立的一层：宿主此前把所有异常都收集进一个数组，那个数组只有探针会读。
//   于是"引擎起不来""Worker 死了""这条命令失败了"在页面上完全看不见 —— 页面要么空白、
//   要么停在一个看起来还能输入、实际打不进字的编辑器上。失败必须分出两档，因为对用户的
//   含义完全不同：
//
//     致命（fatal）：引擎已经不可用（胶水层取不到、Worker 崩溃、实例化抛异常）。
//       呈现为常驻横幅，并把容器标成不可用；此后命令一律快速失败，不再假装能编辑。
//     降级（warn）：单次操作失败，引擎还活着（未知命令、查询被拒、一次请求超时）。
//       呈现为一条会自动消失的提示条，编辑器继续可用。
//
// 这一层自己绝不允许抛异常：错误显示代码炸掉会把"看得见的失败"变回"看不见的失败"，
//   所以每个 DOM 操作都各自兜住。
//
// 与探针的约定：每块界面都带 data-host-error 属性（值为 fatal 或 warn），
//   role 分别是 alert 与 status —— 探针按这两个属性取证，不依赖样式或文案。
(function () {
  'use strict';

  // 同类警告在这段时间内合并计数，不重复堆叠。
  const DUP_MS = 800;
  // 提示条自动消失的时间。
  const WARN_TTL_MS = 6000;
  // 页面上最多同时挂几条提示条（超出的从最早的开始移除）。
  const MAX_WARN = 4;

  const STYLE_ID = 'host-error-style';

  /**
   * 只注入一次的内联样式。
   *
   * 自带样式而不是依赖页面 CSS：这一层要能在任意宿主页面上工作，包括那些
   * 根本没有样式表的产品页；顺带保证"降级界面一定可见"不取决于谁忘了写 CSS。
   */
  function ensureStyle(doc) {
    if (doc.getElementById(STYLE_ID)) return;
    const st = doc.createElement('style');
    st.id = STYLE_ID;
    st.textContent = [
      '[data-host-error]{box-sizing:border-box;font:13px/1.55 "Segoe UI","Microsoft YaHei",sans-serif}',
      '[data-host-error="fatal"]{position:absolute;z-index:20;left:12px;right:12px;top:12px;',
      'padding:10px 12px;border:1px solid #b54708;border-left-width:4px;border-radius:6px;',
      'background:#fff8f1;color:#1b1f24;box-shadow:0 2px 8px rgba(0,0,0,.16)}',
      '[data-host-error="fatal"] .he-t{font-weight:600;color:#b54708}',
      '[data-host-error="fatal"] .he-d{margin-top:4px;white-space:pre-wrap;word-break:break-word}',
      '[data-host-error="fatal"] .he-h{margin-top:6px;color:#5b6572}',
      '[data-host-error="fatal"] button{margin-top:8px}',
      '[data-host-error="warn"]{position:absolute;z-index:19;left:12px;right:12px;bottom:12px;',
      'padding:6px 10px;border:1px solid #d8dde4;border-left:4px solid #b54708;border-radius:6px;',
      'background:rgba(255,255,255,.97);color:#1b1f24;box-shadow:0 1px 4px rgba(0,0,0,.12)}',
      '[data-host-fatal="1"]{outline:1px dashed #b54708;outline-offset:-2px}',
    ].join('');
    (doc.head || doc.documentElement).appendChild(st);
  }

  // 所有对外动作都从这里走：这一层抛异常比不显示更糟。
  function guard(fn) {
    try {
      return fn();
    } catch (e) {
      return undefined;
    }
  }

  /**
   * 造一块错误界面，绑在某个容器上。
   *
   * container 是这块界面的宿主元素（通常就是编辑器根节点）；它被移除之后，
   * 界面自动回落到 document.body，避免"实例已经不在页面上、错误无处显示"。
   */
  function createErrorSurface(container, opts) {
    const o = opts || {};
    const entries = [];
    const onError = typeof o.onError === 'function' ? o.onError : null;
    const doc = (container && container.ownerDocument) || document;

    let banner = null;
    let bannerBtn = null;
    let retryFn = null;
    const toasts = [];

    const rootOf = () =>
      (container && container.isConnected ? container : doc.body || doc.documentElement);

    function record(level, message, info) {
      const at = Date.now();
      const text = String(message == null ? '' : message);
      const last = entries[entries.length - 1];
      let entry;
      // 同一条消息在极短时间内重复出现时只累加计数：引擎一次失败常会连带刷出
      //   好几条同源错误，逐条堆叠会把界面淹掉，反而看不到第一条。
      if (last && last.level === level && last.text === text && at - last.at < DUP_MS) {
        last.count += 1;
        last.at = at;
        entry = last;
      } else {
        entry = { level, text, at, count: 1, detail: info && info.detail ? String(info.detail) : '' };
        entries.push(entry);
      }
      if (onError) guard(() => onError(entry));
      return entry;
    }

    function renderFatal(entry, info) {
      guard(() => {
        ensureStyle(doc);
        if (!banner) {
          banner = doc.createElement('div');
          banner.setAttribute('data-host-error', 'fatal');
          banner.setAttribute('role', 'alert');
          banner.innerHTML = '<div class="he-t"></div><div class="he-d"></div>' +
                             '<div class="he-h"></div>';
          bannerBtn = doc.createElement('button');
          bannerBtn.setAttribute('data-he-retry', '');
          bannerBtn.textContent = '重试';
          bannerBtn.style.display = 'none';
          bannerBtn.addEventListener('click', () => guard(() => retryFn && retryFn()));
          banner.appendChild(bannerBtn);
          rootOf().appendChild(banner);
        }
        banner.querySelector('.he-t').textContent = entry.count > 1
          ? `${entry.text}（${entry.count} 次）`
          : entry.text;
        banner.querySelector('.he-d').textContent =
          entry.detail || (info && info.detail) || '';
        banner.querySelector('.he-h').textContent = (info && info.hint) || '';
        if (typeof (info && info.retry) === 'function') {
          retryFn = info.retry;
          bannerBtn.style.display = '';
        }
        if (container) container.setAttribute('data-host-fatal', '1');
      });
    }

    function renderWarn(entry) {
      guard(() => {
        ensureStyle(doc);
        const el = doc.createElement('div');
        el.setAttribute('data-host-error', 'warn');
        el.setAttribute('role', 'status');
        el.textContent = entry.count > 1 ? `${entry.text}（${entry.count} 次）` : entry.text;
        rootOf().appendChild(el);
        toasts.push(el);
        while (toasts.length > MAX_WARN) {
          const old = toasts.shift();
          if (old && old.parentNode) old.parentNode.removeChild(old);
        }
        setTimeout(() => {
          const i = toasts.indexOf(el);
          if (i >= 0) toasts.splice(i, 1);
          if (el.parentNode) el.parentNode.removeChild(el);
        }, WARN_TTL_MS);
      });
    }

    const api = {
      // 致命失败：常驻横幅 + 容器标记。info 可带 detail / hint / retry。
      fatal(message, info) {
        const e = record('fatal', message, info);
        renderFatal(e, info);
        return e;
      },
      // 单次失败：会自动消失的提示条，不影响可用性。
      warn(message, info) {
        const e = record('warn', message, info);
        renderWarn(e);
        return e;
      },
      // 抹掉界面（不抹 entries，探针要能事后取证）。实例销毁时用。
      clear() {
        guard(() => {
          if (banner && banner.parentNode) banner.parentNode.removeChild(banner);
          banner = null;
          bannerBtn = null;
          while (toasts.length) {
            const el = toasts.pop();
            if (el && el.parentNode) el.parentNode.removeChild(el);
          }
          if (container) container.removeAttribute('data-host-fatal');
        });
      },
      get entries() { return entries.slice(); },
      get counts() {
        return {
          fatal: entries.filter((e) => e.level === 'fatal').length,
          warn: entries.filter((e) => e.level === 'warn').length,
        };
      },
      get fatalActive() { return !!banner; },
      get bannerText() {
        const t = banner && banner.querySelector('.he-t');
        return t ? t.textContent : '';
      },
      get bannerDetail() {
        const d = banner && banner.querySelector('.he-d');
        return d ? d.textContent : '';
      },
      get toastTexts() { return toasts.map((el) => el.textContent); },
      // 探针与宿主共用的取证句柄：界面元素本身。
      get element() { return banner; },
    };
    return api;
  }

  window.createErrorSurface = createErrorSurface;
})();
