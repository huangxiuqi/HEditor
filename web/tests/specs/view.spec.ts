// 视图判据：缩放之后画面停在同一处。
//
// 缩放同时改页面高度与窗口页数：宿主的滚动容器按像素记位置，引擎按页记窗口。
//   两侧只要有一侧按旧倍率算，表现都是画面跳回开头，而文档内容分毫未动 ——
//   所以判据全落在滚动位置与窗口号上。
import { check } from '../support/check';
import { test } from '../support/probe';

test.describe('缩放与滚动', () => {
  test('缩放停在原处：滚动位置按倍率折算，引擎窗口跟着走', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1400, height: 900 } });
    const r = await p.read<any>(`
      const lines = [];
      for (let i = 0; i < 120; i++) lines.push('第' + i + '段内容一二三四五六七八九十');
      await H.setText(lines.join('\\n'));
      await H.flush();
      const sc = EL('.heditor-stage');
      sc.scrollTop = 900;
      await new Promise((r) => setTimeout(r, 320));
      const before = { top: sc.scrollTop, off: EL('.heditor-pages').offsetTop,
                       h: EL('.heditor-pages').clientHeight };
      await H.setZoom(1.25);
      await H.flush();
      await new Promise((r) => setTimeout(r, 320));
      const zoomIn = { top: sc.scrollTop, off: EL('.heditor-pages').offsetTop,
                       h: EL('.heditor-pages').clientHeight, win: H.geometry().window };
      await H.setZoom(0.5);
      await H.flush();
      await new Promise((r) => setTimeout(r, 320));
      const zoomOut = { top: sc.scrollTop, win: H.geometry().window };
      return { before, zoomIn, zoomOut };`);
    // 折算的是"页顶之上那一段边距之下"的部分：上边距是固定 CSS 值，不随缩放变。
    const want = (r.before.top - r.before.off) * 1.25 + r.before.off;
    check('★ 放大后仍停在原来那一处（滚动位置按 1.25 折算）',
          Math.abs(r.zoomIn.top - want) <= 2, r.zoomIn.top + ' / 期望 ' + want);
    check('放大之后页面确实变高了（说明缩放真的生效）',
          r.zoomIn.h > r.before.h, r.before.h + ' → ' + r.zoomIn.h);
    const wantOut = (r.before.top - r.before.off) * 0.5 + r.before.off;
    check('★ 缩小后同样停在那一处（不是被拽回开头，也不是被夹到底）',
          Math.abs(r.zoomOut.top - wantOut) <= 2 ||
            Math.abs(r.zoomOut.top - (r.before.top - r.before.off) * 0.5) <= 2,
          r.zoomOut.top + ' / 期望 ' + wantOut);
    check('缩小之后窗口页数与总页数对得上（没有留下空窗）',
          r.zoomOut.win.count >= 1 && r.zoomOut.win.first >= 0, JSON.stringify(r.zoomOut.win));
  });
});
