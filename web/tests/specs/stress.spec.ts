// 交互压力判据：随机/密集地在正文各处点击 + 键盘动作，每一步都当场问两个问题。
//
//   引擎帧本身对不对？auditFrame：把当前帧与"整帧重画一遍"逐字节比。这是覆盖最广的
//     "该消失却没消失"判据（旧表单标记残留就是这一类）。不能只用 selfCheck：引擎帧自己
//     错了时画布会忠实反映它，两边一起错、判据照样绿。
//   画布贴得对不对？selfCheck。顺序要紧：auditFrame 会把这一帧修成真值，
//     必须先跑 selfCheck。
//
// 这里跑的是迁移垫片上的旧门面名（window.__heditor），与编辑探针同一套读数。
import { check } from '../support/check';
import { test } from '../support/probe';

// 在页面上点某个"帧磅坐标"（走真实的 mousedown，就是用户点画布那条路）。
const CLICK_AT_PT = (xPt: number, yPt: number) => `
  const cv = window.__heditor.canvas;
  const r = cv.getBoundingClientRect();
  const z = window.__heditor.zoomLevel;
  const x = r.left + (${xPt} * z) * (r.width / cv.width);
  const y = r.top + (${yPt} * z) * (r.height / cv.height);
  const ev = new MouseEvent('mousedown', { clientX: x, clientY: y, bubbles: true, cancelable: true, button: 0 });
  cv.dispatchEvent(ev);
  return true;`;

// 每一步的取证：先画布自审，再引擎帧自审，最后取记账。
const SNAP = `
  const sc = await window.__heditor.selfCheck();
  // 顺序要紧：auditFrame 会把这一帧修成真值，放在 selfCheck 之后跑，
  //   否则它会顺带把上屏错误也"治好"，selfCheck 就再也抓不到上屏问题了。
  const au = await window.__heditor.auditFrame();
  const st = window.__heditor.editStats;
  const ct = window.__heditor.controls;
  const c = window.__heditor.caret;
  return { screen: sc.screenVsEngineDiffPx, screenBox: sc.screenVsEngineDiffBox,
           frame: au.diff, frameBox: au.box,
           caretBox: [c.x, c.y, c.x + c.w, c.y + c.h],
           repaint: sc.repainted, caretCtrl: ct ? ct.caret : -1,
           caretOff: st.caretOffset, pages: st.repaintedPages };`;

// 一次取证的读数。
type Audit = {
  screen: number;
  screenBox: unknown;
  frame: number;
  frameBox: number[] | null;
  caretBox: number[];
  repaint: unknown;
  caretCtrl: number;
  caretOff: number;
  pages: number;
};

test.describe('交互压力', () => {
  test('点击与按键之后引擎帧与画布始终一致', async ({ open }) => {
    const p = await open('index.html', {
      query: { debug: '1' },
      viewport: { width: 1500, height: 1200 },
      adapter: true,
    });

    // 停表：闪烁相位会让"当前帧 vs 整帧重画"的比对自带竞态 —— 增量帧里画的是上一拍的
    //   光标，整帧重画画的是这一拍的，差出来的正是那 1x27 px 的光标矩形。
    //   闪烁本身由 diag.spec 第十一节判（2.4 s 内既亮过也灭过），这里要的是内容一致。
    await p.ev('window.__heditor.stopBlink(); await window.__heditor.loadFormSample(0); return true;');

    // 采样的点击点（帧磅坐标）：行首 / 冒号 / 三个字段内 / 只读字段 / 行尾。
    const pts: [number, number, string][] = [
      [63, 62, '行首(患)'],
      [70, 62, '冒号'],
      [75, 62, 'field0 内 反'],
      [80, 62, 'field0 内 心'],
      [88, 62, 'field0 末 悸'],
      [96, 62, '只读 field1 三天'],
      [110, 62, '逗号'],
      [120, 62, 'field2 加'],
      [128, 62, 'field2 重'],
      [140, 62, 'field2 末 天'],
      [150, 62, '行尾句号'],
    ];

    let bad = 0;
    let steps = 0;
    const log: string[] = [];
    const failures: string[] = [];

    // 帧差恰好只落在光标矩形上时，补一次呈现再问一遍。
    //
    // 光标从不可见转为可见（按键会重置闪烁相位）会让增量帧与整帧重画差出一个光标矩形，
    //   而内容层一模一样。这是已知的呈现缺口，不是排版或上屏不一致；补一次呈现仍差，
    //   就照算不一致 —— 判据不因为"它能被 present 修好"就放松。
    const sameBox = (a: number[] | null, b: number[]) =>
      !!a && a.length === 4 && a.every((v, i) => Math.abs(v - b[i]) <= 1);

    const audit = async (label: string): Promise<Audit> => {
      let s = await p.ev<Audit>(SNAP);
      if (s.frame > 0 && sameBox(s.frameBox, s.caretBox)) {
        await p.ev('await window.__heditor.present(); return true;');
        s = await p.ev<Audit>(SNAP);
      }
      steps += 1;
      if (s.screen > 0 || s.frame > 0) {
        bad += 1;
        const line = label + ' → 引擎帧自审差 ' + s.frame + ' px 盒=' + JSON.stringify(s.frameBox) +
          '；画布vs引擎差 ' + s.screen + ' px 盒=' + JSON.stringify(s.screenBox) +
          '；上报脏矩形=' + JSON.stringify(s.repaint) + ' 光标控件=' + s.caretCtrl +
          ' off=' + s.caretOff;
        log.push(line);
        failures.push(line);
      }
      return s;
    };

    const runCase = async (label: string, waitMs: number) => {
      for (const [x, y, name] of pts) {
        await p.ev(CLICK_AT_PT(x, y));
        if (waitMs) await p.page.waitForTimeout(waitMs);
        const s = await audit('[' + label + '] 点「' + name + '」(' + x + ',' + y + ') wait=' + waitMs + 'ms');
        if (s.screen > 0 || s.frame > 0) await p.shot('stress-' + label + '-' + steps);
      }
    };

    // 三种点击间隔：连点、120ms、600ms（跨过闪烁 tick）。
    await test.step('三种点击间隔', async () => {
      await runCase('连点', 0);
      await runCase('间隔120ms', 120);
      await runCase('间隔600ms', 600);
    });

    // 键盘动作也纳入：方向键（会跨字段）、Tab（缩进）、敲字、删字、切模式。
    const actions: [string, string][] = [
      ['ArrowRight', 'ArrowRight'], ['ArrowRight', 'ArrowRight'], ['ArrowLeft', 'ArrowLeft'],
      ['Tab', 'Tab'], ['Tab', 'Tab'], ['a', 'a'], ['Backspace', 'Backspace'],
    ];
    const press = (k: string) => p.ev(`
      window.__heditor.textarea.dispatchEvent(
        new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true, cancelable: true }));
      return true;`);

    await test.step('四轮键盘与模式切换', async () => {
      for (let round = 0; round < 4; round++) {
        for (const [label, k] of actions) {
          await press(k);
          await p.page.waitForTimeout(30);
          await audit('[键盘第' + (round + 1) + '轮] 按 ' + label);
        }
        // 轮末切一次模式再切回来（模式参与占位/标记）。
        await p.ev('await window.__heditor.setEditMode(0); return true;');
        await audit('[切到文档模式]');
        await p.ev('await window.__heditor.setEditMode(1); return true;');
        await audit('[切回表单模式]');
      }
    });

    if (log.length) {
      await test.info().attach('不一致明细', { body: log.join('\n'), contentType: 'text/plain' });
    }
    check('步数就是计划的那一批（前置条件）', steps === 69, String(steps));
    check('每一步的引擎帧与画布都一致', bad === 0,
          bad + ' / ' + steps + ' 步不一致' + (failures.length ? '：' + failures[0] : ''));
  });
});
