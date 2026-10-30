// 编辑面判据的公共前奏：开页、等就绪、收 console 错误、量"整页重画"的分母、收尾。
//
// 判据按域拆成多份 spec 之后，这些动作必须只有一份实现 —— 否则"全量分母"的口径会各自漂，
// 同一个像素判据在两份文件里用的分母不是同一个数。
import { test } from '@playwright/test';

import { check } from './check';
import type { OpenPage, ProbePage } from './probe';

// 探针统一用的缩放倍数（1.5 = 150%）。
export const ZOOM = 1.5;

// 单段场景的正文，全量基准与键盘路径两节的像素分母都建立在它上面。
export const S6 = '患者主诉：反复心悸三天，加重一天。';

// 页面内的码点长度：不能用 .length，那是 UTF-16 码元数，星平面字符会与码点数不等。
export const CP_LEN = 'const cplen=(s)=>Array.from(s).length;';

/**
 * 等一拍。
 *
 * @param ms 毫秒
 * @returns 到点后完成的 Promise
 */
export const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

/**
 * 往真实 textarea 派键盘事件（走页面的 onKeyDown，不是引擎 API）。
 *
 * @param k key 字段
 * @param extra 额外的修饰键声明，例如 ", ctrlKey: true"
 * @returns 页面内执行片段
 */
export const key = (k: string, extra = '') => `
  IME().dispatchEvent(
    new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true,
                                   cancelable: true ${extra} }));
  return true;`;

/**
 * 打开编辑面探针页，并开始收集 console 错误。
 *
 * 每个 test 一个干净页面：判据之间不共享引擎状态，失败也只影响自己那一份。
 *
 * @param open 探针页工厂
 * @returns 探针页与错误收集数组（收尾时交给 finishEdit）
 */
export async function openEdit(
  open: OpenPage,
): Promise<{ probe: ProbePage; consoleErrors: string[] }> {
  const probe = await open('index.html', {
    query: { debug: '1' },
    viewport: { width: 1500, height: 1200 },
  });
  const consoleErrors: string[] = [];
  probe.page.on('console', (m) => {
    if (m.type() === 'error') consoleErrors.push(m.text());
  });
  probe.page.on('pageerror', (e) => consoleErrors.push('uncaught: ' + e.message));
  return { probe, consoleErrors };
}

/**
 * 切到可编辑场景并拿到真正的全量作为分母。
 *
 * 分母只能是首次呈现的写入量：第二次 repaint 本来就是增量（帧尺寸没变，只贴了一个旧光标），
 * 拿它当分母会得出"增量占了百分之百"的荒谬结论。而且必须用 reset 的返回值，不能用 reset
 * 之后再读一次 editStats —— 中间那句 stopBlink 不动像素，但任何一次 present 都会把统计
 * 覆盖成"只重画了个光标"。
 *
 * @param probe 探针页
 * @returns 那次全量呈现的账
 */
export async function resetAndGetFullBaseline(probe: ProbePage) {
  return probe.read(`
    const s = await LOAD.short();
    await D.stopBlink();
    return s;`);
}

/**
 * 收尾：判这一份 spec 没有页面异常与 console 错误，并把取证账挂到报告上。
 *
 * 每份 spec 各判自己那一个页面：拆开之后，某一份页面出错不会再被别处的绿盖住。
 *
 * @param probe 探针页
 * @param consoleErrors 开页以来收集到的 console 错误
 * @param state 取证账，原样挂成 JSON 附件
 * @returns 无
 */
export async function finishEdit(
  probe: ProbePage,
  consoleErrors: string[],
  state: Record<string, any>,
): Promise<void> {
  const pageErrors = await probe.read('return D.errors();');
  state.pageErrors = pageErrors;
  state.consoleErrors = consoleErrors;
  check('无 page error', pageErrors.length === 0, JSON.stringify(pageErrors));
  check('无 console error', consoleErrors.length === 0, JSON.stringify(consoleErrors));
  await test.info().attach('state', {
    body: JSON.stringify(state, null, 2),
    contentType: 'application/json',
  });
}
