// 素材地址：胶水、字体、wasm，以及 Worker 壳。
//
// 与"引擎在哪条线程上跑"无关：这些是"东西从哪儿来"，两边共用同一份解析结果。
import type { FontInput, FontRole, FontSource } from '../types';

/**
 * 一条字体装载项：要么给地址，要么给字节，二选一。
 *
 * 两者都不带"名字给谁看"这一项 —— 显示名由字体文件的 name 表自己给（引擎读得到）。
 * 这里的 name 只用于字节的身份（字体身份的最后一级是文件名，见 fontFileId）。
 */
export interface FontEntry {
  /**
   * 取字体的绝对地址（相对地址已按产物目录解析过）。
   */
  url?: string;

  /**
   * 字体字节；宿主已经读过 Blob / File 之后交过来的那一份。
   */
  data?: Uint8Array;

  /**
   * 字节对应的文件名。
   */
  name?: string;

  /**
   * 显示名（界面那一行）。不传就用字体文件的族名。
   */
  label?: string;

  /**
   * 角色：正文 / 蒙文 / 拉丁回退。缺的档由引擎按"正文 / 跟随正文 / 关掉回退"补。
   */
  role?: FontRole;

  /**
   * 按需装载（整份不进常驻，按页取块）。
   */
  lazy?: boolean;
}

/**
 * 素材地址。
 */
export interface AssetPaths {
  /**
   * 胶水脚本的绝对地址（heditor.js）。
   */
  glue: string;

  /**
   * wasm 的绝对地址；空 = 由胶水按自己的目录推。
   */
  wasm?: string;

  /**
   * 缓存戳（?v= 之后那一串），会一路带到 wasm 的 locateFile。
   */
  bust?: string;

  /**
   * 字体清单：角色、绝对地址或字节。
   */
  fonts: FontEntry[];

  /**
   * 产物目录（相对地址按它解析）。
   */
  base: string;

  /**
   * Worker 壳的地址。
   */
  worker?: string;
}

/**
 * 内置三份 Noto：正文 / 蒙文 / 拉丁回退。角色与显示名只在这里写死 —— 调用方给的清单里
 * 两者都是可选的，缺省时按第一份当正文、显示名用字体自己的族名。
 *
 * 显示名给中文是因为界面是中文的：字体文件的 name 表里没有中文名（Noto 这三份只有
 *   英文族名），而"选字体"那一格给人看的就是这一行。
 */
const BUILTIN_FONTS: Array<{ url: string; role: FontRole; label: string }> = [
  { url: 'assets/fonts/NotoSansSC-VF.ttf', role: 'cjk', label: '思源黑体' },
  { url: 'assets/fonts/NotoSansMongolian-Regular.ttf', role: 'mongol', label: '蒙古文' },
  { url: 'assets/fonts/NotoSans-Latin-VF.ttf', role: 'latin', label: '拉丁' },
];

/**
 * 字节的三种来源统一成一份 Uint8Array。
 * @param data File / Blob / ArrayBuffer / Uint8Array 或其视图
 * @returns 字体字节
 */
async function bytesOf(data: FontSource['data']): Promise<Uint8Array> {
  if (data instanceof Uint8Array) return data;
  if (ArrayBuffer.isView(data)) {
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  }
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (data && typeof (data as Blob).arrayBuffer === 'function') {
    return new Uint8Array(await (data as Blob).arrayBuffer());
  }
  throw new TypeError('字体的 data 要是 File / Blob / ArrayBuffer / Uint8Array');
}

/**
 * 一条清单项 → 装载项。地址按产物目录解析；字节当场读出来（File 是异步的）。
 * @param base 产物目录
 * @param src 地址串、字体声明，或直接给的字节
 * @param index 这一条在清单里的序号，只用于缺省文件名
 * @returns 装载项
 */
async function fontEntry(base: string, src: FontInput, index: number): Promise<FontEntry> {
  // 直接递给来的字节（File / Blob / ArrayBuffer / Uint8Array）当 data 用：
  //   "把文件拖进来就注册"那条路不该再包一层 { data }。
  const bare = src instanceof Blob || src instanceof ArrayBuffer || src instanceof Uint8Array;
  const o: FontSource =
    typeof src === 'string' ? { url: src } : bare ? { data: src } : (src as FontSource);
  const out: FontEntry = { role: o.role, lazy: o.lazy, label: o.label };
  if (o.data !== undefined) {
    out.data = await bytesOf(o.data);
    const file = o.name || (typeof (o.data as File).name === 'string' ? (o.data as File).name : '');
    out.name = file || `font-${index}.ttf`;
  } else if (o.url) {
    out.url = new URL(o.url, base).href;
  } else {
    throw new TypeError('字体既没有 url 也没有 data');
  }
  return out;
}

/**
 * 构造期的字体清单：可给一项、一条地址串，或一整串；不给就用内置三份 Noto。
 * @param base 产物目录
 * @param list 调用方给的清单
 * @returns 每条都带绝对地址或字节
 */
export async function resolveFonts(
  base: string,
  list?: FontInput | FontInput[],
): Promise<FontEntry[]> {
  const src = list === undefined ? BUILTIN_FONTS : Array.isArray(list) ? list : [list];
  const out: FontEntry[] = [];
  for (let i = 0; i < src.length; ++i) out.push(await fontEntry(base, src[i], i));
  return out;
}
