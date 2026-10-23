// 素材地址：胶水、字体、wasm，以及 Worker 壳。
//
// 与"引擎在哪条线程上跑"无关：这些是"东西从哪儿来"，两边共用同一份解析结果。
import type { FontSource } from '../types';

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
   * 字体清单：角色与绝对地址。
   */
  fonts: Array<{ role: string; path: string; label?: string }>;

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
 * 字体清单：默认三份 Noto（正文 / 蒙文 / 拉丁），路径相对产物目录。
 * @param base 产物目录
 * @param list 调用方给的清单
 * @returns 角色与绝对地址
 */
export function defaultFonts(base: string, list?: FontSource[]): Array<{ role: string; path: string }> {
  const src = list && list.length
    ? list.map((f, i) => ({ role: f.role ?? ['cjk', 'mongol', 'latin'][i] ?? 'cjk', path: f.url }))
    : [
        { role: 'cjk', path: 'assets/fonts/NotoSansSC-VF.ttf' },
        { role: 'mongol', path: 'assets/fonts/NotoSansMongolian-Regular.ttf' },
        { role: 'latin', path: 'assets/fonts/NotoSans-Latin-VF.ttf' },
      ];
  return src.map((f) => ({ role: f.role, path: new URL(f.path, base).href }));
}
