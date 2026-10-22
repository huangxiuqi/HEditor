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
 * 一份剪贴板载荷：片段（本编辑器的格式）与它的纯文本投影。
 */
export interface ClipPayload {
  fragment: string;

  text: string;
}

/**
 * 解析结果：成功时给载荷，失败时给一句给人看的原因。
 */
export interface ParsedPayload {
  payload?: ClipPayload;

  error?: string;
}

/**
 * 解析引擎给的载荷信封。
 * @param raw 引擎 copy 查询的返回值（JSON 文本）
 * @returns 载荷，或没有可复制内容的原因
 */
export function parseClipPayload(raw: unknown): ParsedPayload {
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
  return { payload: { fragment: env.f, text: typeof env.t === 'string' ? env.t : '' } };
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
  dt.setData('text/plain', p.text);
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
  const nav = typeof navigator === 'undefined' ? undefined : navigator;
  if (nav?.clipboard && typeof ClipboardItem !== 'undefined') {
    try {
      await nav.clipboard.write([
        new ClipboardItem({
          [FRAGMENT_SYSTEM_MIME]: new Blob([p.fragment], { type: FRAGMENT_MIME }),
          'text/plain': new Blob([p.text], { type: 'text/plain' }),
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
  return writeToSystemClipboard({ fragment: '', text: '' });
}

/**
 * 读系统剪贴板：优先自家格式，其次纯文本。
 *
 * 只有程序化粘贴（editor.paste()）走这条：键盘那次粘贴由浏览器的 paste 事件把数据直接
 * 送进处理器，不需要权限。读不到（没授权、或剪贴板里是图片这类格式）时返回 null。
 * @returns 自家片段、纯文本，或 null
 */
export async function readSystemClipboard(): Promise<{ fragment?: string; text?: string } | null> {
  const nav = typeof navigator === 'undefined' ? undefined : navigator;
  if (nav?.clipboard?.read) {
    try {
      const items = await nav.clipboard.read();
      let text: string | undefined;
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
      }
      if (text !== undefined) return { text };
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
