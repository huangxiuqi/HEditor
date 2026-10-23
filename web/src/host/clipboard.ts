// 系统剪贴板：载荷由引擎生成，这一层只负责"搬"。
// 为什么单独成文件：宿主与编辑器的公开方法都要用它，而它要碰 navigator.clipboard、
//   ClipboardItem 与 execCommand 三样浏览器专有的东西 —— 这些不该散进编辑器编排里。
// 边界：不解析片段的内容（那是引擎的格式），只认载荷信封的 f / t / e 三个键。

/**
 * 自家格式的类型名。写进事件的 clipboardData 用它，别的应用看不懂也没关系。
 */
export const FRAGMENT_MIME = 'application/x-heditor-fragment';

/**
 * 写进系统剪贴板（ClipboardItem）时用的名字。
 *
 * 自定义类型必须带 web 前缀：不带前缀的类型浏览器会在写入时静默丢掉，读回来的只剩
 * text/plain —— 症状正是"复制粘贴成了纯文本，控件与表格都没了"。读端两种名字都认。
 */
const FRAGMENT_SYSTEM_MIME = 'web ' + FRAGMENT_MIME;

/**
 * 最近一次写出去的那一份载荷。
 *
 * 浏览器不保证自定义类型能穿过剪贴板：paste 事件的 getData 只给标准类型，
 *   navigator.clipboard.read 在没授权时整条拒绝。于是"复制带格式的一段、粘回自己"
 *   在真实浏览器上会退化成纯文本。这份缓存就是那条路的保证 —— 文本相等就用它，
 *   编辑器内的复制粘贴因此不依赖任何权限。
 */
let lastPayload: ClipPayload | null = null;

/**
 * 一份剪贴板载荷：片段（本编辑器的格式）、纯文本投影、以及带格式的 HTML。
 */
export interface ClipPayload {
  fragment: string;

  text: string;

  /**
   * text/html 那一份（字符格式折成内联 CSS）。
   *
   * 只给"粘到别的应用"用：Word / 浏览器读的是它。段内往返仍走 fragment —— HTML 是有损的
   * 表示（控件边界、字段属性都表达不了）。
   */
  html: string;

  /**
   * 这一份是哪个实例写出去的（宿主每次建实例时生成一个）。
   *
   * 用途只有一个：判断"这一份是不是本实例写出去的"。缓存回退只服务本实例写的内容 ——
   *   文本相等不等于同源（别处复制来的同样的字，格式完全不同）。
   */
  origin: string;
}

/**
 * 解析结果：成功时给载荷，失败时给一句给人看的原因。
 */
export interface ParsedPayload {
  payload?: ClipPayload;

  error?: string;
}

/**
 * 记住这一份载荷（写剪贴板的两条路都先记一次）。
 * @param p 载荷
 */
function rememberPayload(p: ClipPayload): void {
  // 空片段是一次"清空"：缓存也一起清，否则下一次粘贴会把上一份内容交出去。
  lastPayload = p.fragment ? p : null;
}

/**
 * 取"与这段纯文本对应"的那一份载荷。
 *
 * 判据是整段文本相等：剪贴板里只剩纯文本时它是唯一能对上的线索，文本相同就当作同一份
 *   内容 —— 退化到纯文本的代价是丢格式，而这里宁可多留一份格式。
 * @param text 剪贴板里的纯文本
 * @param mine 本实例的身份（来源不同就不用这份缓存）
 * @returns 载荷；对不上或来源不同时为 null
 */
export function recallPayload(text: string, mine: string): ClipPayload | null {
  if (!lastPayload || !lastPayload.fragment) return null;
  // 必须同源：文本相等不等于同一份内容 —— 用户在别处复制了同样的字，格式可能完全不同，
  //   拿这里的片段去粘就是拿旧格式覆盖新内容。来源不同就交给纯文本那条路。
  if (!mine || lastPayload.origin !== mine) return null;
  // 换行按 LF 归一之后再比：剪贴板在系统那一层可能把 \n 换成 \r\n。
  const norm = (s: string): string => s.replace(/\r\n/g, '\n');
  return norm(lastPayload.text) === norm(text) ? lastPayload : null;
}

/**
 * 最近一次写出去的载荷（读剪贴板整条失败时用它）。
 * @param mine 本实例的身份（只认本实例写的那一份）
 * @returns 载荷；没写过或来源不同时为 null
 */
export function latestPayload(mine: string): ClipPayload | null {
  if (!lastPayload || !lastPayload.fragment) return null;
  // 同一条理由：只认自己写的那一份。读不到剪贴板时用别人的载荷会更糟 —— 粘出来的
  //   是另一个实例的内容，而用户以为是刚复制的那个。
  return mine && lastPayload.origin === mine ? lastPayload : null;
}

/**
 * 片段在 HTML 里的属性名。
 *
 * 自定义 MIME 与"最近一份载荷"都过不了进程/浏览器边界：前者不保证穿过剪贴板，后者只活
 * 在一个页面里。text/html 是唯一处处都在的标准类型，于是把片段本体塞进它的一个属性里 ——
 * 别的应用忽略这个属性，本编辑器读得到完整片段（样式、控件、表格都在），跨实例与跨浏览器
 * 因此走同一条路。
 */
const FRAGMENT_ATTR = 'data-heditor-fragment';

/**
 * 载体上记"谁写的"。
 *
 * 与片段本体分开一个属性：片段是内容，这一项是出处。粘的时候不据此拒绝（跨实例本来就
 *   要能粘），只为诊断与将来的"同源粘贴"语义留一手。
 */
const ORIGIN_ATTR = 'data-heditor-origin';

/**
 * 把片段藏进 HTML 的一个 div 上。属性值里的引号与 & 要转义，否则整段 HTML 会坏掉。
 * @param html 正文 HTML（可为空）
 * @param fragment 片段 JSON
 * @param origin 写出这一份的实例身份（可为空）
 * @returns 带片段的 HTML
 */
export function embedFragment(html: string, fragment: string, origin = ''): string {
  const attr = escapeAttr(fragment);
  const src = origin ? ` ${ORIGIN_ATTR}="${escapeAttr(origin)}"` : '';
  return `<div ${FRAGMENT_ATTR}="${attr}"${src}>${html}</div>`;
}

/**
 * 从 HTML 里取回片段（取不到就是 null）。
 * @param html 剪贴板里的 HTML
 * @returns 片段 JSON；没有这个属性时为 null
 */
export function fragmentInHtml(html: string): string | null {
  if (!html) return null;
  const m = new RegExp(FRAGMENT_ATTR + '="([^"]*)"').exec(html);
  return m ? unescapeAttr(m[1]) : null;
}

/**
 * 属性值转义（只处理会破坏属性的三个字符）。
 * @param s 原文
 * @returns 可放进双引号属性的串
 */
function escapeAttr(s: string): string {
  return s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
}

/**
 * 属性值反转义。
 * @param s 属性原文
 * @returns 原文
 */
function unescapeAttr(s: string): string {
  return s.replace(/&quot;/g, '"').replace(/&lt;/g, '<').replace(/&amp;/g, '&');
}

/**
 * 解析引擎给的载荷信封。
 * @param raw 引擎 copy 查询的返回值（JSON 文本）
 * @param origin 本实例的身份（记进载荷，供缓存判同源）
 * @returns 载荷，或没有可复制内容的原因
 */
export function parseClipPayload(raw: unknown, origin = ''): ParsedPayload {
  const text = String(raw ?? '');
  if (!text) return { error: '没有选中内容' };
  let env: any;
  try {
    env = JSON.parse(text);
  } catch {
    return { error: '剪贴板载荷不是 JSON' };
  }
  if (!env || typeof env !== 'object') return { error: '剪贴板载荷不是对象' };
  if (typeof env.e === 'string' && env.e) return { error: env.e };
  if (typeof env.f !== 'string' || !env.f) return { error: '剪贴板载荷里没有片段' };
  return {
    payload: {
      fragment: env.f,
      text: typeof env.t === 'string' ? env.t : '',
      html: typeof env.h === 'string' ? env.h : '',
      origin,
    },
  };
}

/**
 * 同步写进一个已经拿到的 DataTransfer（copy / cut 事件里那一个）。
 *
 * 这条路不需要权限、也不会被用户设置拦掉，是首选：事件里的 clipboardData 只在处理器
 * 同步执行期间可写，所以拿到载荷之后必须立刻写，不能等一个 await。
 * @param dt 事件的 clipboardData
 * @param p 载荷
 * @returns 写进去了没有
 */
export function writeToDataTransfer(dt: DataTransfer | null, p: ClipPayload): boolean {
  if (!dt) return false;
  rememberPayload(p);
  dt.setData('text/plain', p.text);
  try {
    dt.setData('text/html', embedFragment(p.html, p.fragment, p.origin));
  } catch {
    // 同上：纯文本已经写进去了。
  }
  try {
    dt.setData(FRAGMENT_MIME, p.fragment);
  } catch {
    // 个别浏览器只允许写标准类型：纯文本已经写进去了，跨实例粘贴退化为纯文本。
  }
  return true;
}

/**
 * 异步写进系统剪贴板（Worker 传输拿不到事件里的 DataTransfer，只能走这条）。
 *
 * 自定义类型需要 clipboard-write 权限，浏览器对"当前获得焦点的标签页"通常直接放行；
 * 被拒时退化成只写纯文本 —— 宁可少一份格式，也不要让 Ctrl+C 看起来什么也没发生。
 * @param p 载荷
 * @returns 写进去了没有
 */
export async function writeToSystemClipboard(p: ClipPayload): Promise<boolean> {
  rememberPayload(p);
  const nav = typeof navigator === 'undefined' ? undefined : navigator;
  if (nav?.clipboard && typeof ClipboardItem !== 'undefined') {
    try {
      await nav.clipboard.write([
        new ClipboardItem({
          [FRAGMENT_SYSTEM_MIME]: new Blob([p.fragment], { type: FRAGMENT_MIME }),
          'text/plain': new Blob([p.text], { type: 'text/plain' }),
          // 带格式的那一份：粘到 Word / 浏览器里还看得出粗斜下划线；片段本体藏在它的
          //   属性里，于是跨实例、跨浏览器也能整份还原。
          'text/html': new Blob([embedFragment(p.html, p.fragment, p.origin)], {
            type: 'text/html',
          }),
        }),
      ]);
      return true;
    } catch {
      // 落到下面那条纯文本退路
    }
    try {
      await nav.clipboard.writeText(p.text);
      return true;
    } catch {
      // 落到 execCommand 那条
    }
  }
  return copyTextFallback(p.text);
}

/**
 * 文档里没有可复制的选区时，把剪贴板上的旧内容清掉。
 *
 * 不清的后果是"复制一次 A，再选空按 Ctrl+C，粘贴出来的还是 A" —— 用户会认为自己刚复制了
 * 空白，而编辑器在用上一份内容回答他。自定义类型也一起清：留着它更糟。
 * @returns 清掉了没有
 */
export async function clearClipboard(): Promise<boolean> {
  return writeToSystemClipboard({ fragment: '', text: '', html: '', origin: '' });
}

/**
 * 读系统剪贴板：优先自家格式，其次纯文本。
 *
 * 只有程序化粘贴（editor.paste()）走这条：键盘那次粘贴由浏览器的 paste 事件把数据直接
 * 送进处理器，不需要权限。读不到（没授权、或剪贴板里是图片这类格式）时返回 null。
 * @returns 自家片段、纯文本，或 null
 */
export async function readSystemClipboard(): Promise<
  { fragment?: string; text?: string; html?: string } | null
> {
  const nav = typeof navigator === 'undefined' ? undefined : navigator;
  if (nav?.clipboard?.read) {
    try {
      const items = await nav.clipboard.read();
      let text: string | undefined;
      let html: string | undefined;
      for (const item of items) {
        // 两种名字都认：走事件写的没有前缀，走 ClipboardItem 写的有（见上一条常量）。
        const fragType = item.types.find((t) => t === FRAGMENT_MIME || t === FRAGMENT_SYSTEM_MIME);
        if (fragType) {
          const blob = await item.getType(fragType);
          const fragment = await blob.text();
          if (fragment) return { fragment };
        }
        if (item.types.includes('text/plain')) {
          const blob = await item.getType('text/plain');
          text = await blob.text();
        }
        if (item.types.includes('text/html')) {
          const blob = await item.getType('text/html');
          html = await blob.text();
        }
      }
      if (text !== undefined || html !== undefined) return { text, html };
    } catch {
      return null;
    }
  }
  return null;
}

/**
 * 最后的退路：借一个临时的 textarea 走 execCommand。
 * @param text 纯文本
 * @returns 复制成了没有
 */
function copyTextFallback(text: string): boolean {
  if (typeof document === 'undefined') return false;
  const el = document.createElement('textarea');
  el.value = text;
  el.setAttribute('readonly', '');
  el.style.position = 'fixed';
  el.style.left = '-9999px';
  document.body.appendChild(el);
  el.select();
  let ok = false;
  try {
    ok = document.execCommand('copy');
  } catch {
    ok = false;
  }
  document.body.removeChild(el);
  return ok;
}
