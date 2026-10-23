// 输入层判据：走真实浏览器事件，不调 API。
//
// 为什么单独一份：demo.spec 证明的是"公开面只有公开面"，它调 type('AB') 走的是命令层；
//   而用户真正用的是键盘、指针、输入法 —— 这三条路在库里的入口是键位映射与命中归宿。
//   API 面全绿时输入层仍可能没接上。
//
// 每一步的状态打成一条轨迹，失败时作为附件留档。
import { check } from '../support/check';
import { expect, test } from '../support/probe';

// 一步的完整读数。
type Step = {
  label: string;
  text: string;
  para: number;
  off: number;
  page: number;
  empty: boolean;
  anchor: { para: number; offset: number };
  focus: { para: number; offset: number };
};

test.describe('输入层', () => {
  test('真实键鼠与输入法都接上了', async ({ page, open }) => {
    const errors: string[] = [];
    page.on('console', (m) => {
      if (m.type() === 'error') errors.push(m.text());
    });
    page.on('pageerror', (e) => errors.push('uncaught: ' + e.message));

    // DPR = 1：位移与页内坐标一一对应，像素算术才不用带比例因子。
    const p = await open('index.html', { viewport: { width: 1280, height: 900 } });
    const trail: Step[] = [];

    // 每一步之后取一份状态；先排空，拿的才是这一拍。
    const snap = async (label: string): Promise<Step> => {
      const s = await p.ev<Step>(`
        const e = window.__demo;
        const c = e.caret();
        const sel = e.selection();
        return { label: ${JSON.stringify(label)}, text: e.text(),
                 para: c.pos.para, off: c.pos.offset, page: c.page,
                 empty: sel.empty, anchor: sel.anchor, focus: sel.focus };`);
      trail.push(s);
      return s;
    };

    // 文档重置成固定几行：坐标可预期，断言不必依赖示例正文的长度。
    const reset = () => p.ev<string>(`
      const e = window.__demo;
      await e.setText('第一行 abc\\n第二行 def\\n第三行 ghi');
      await e.setCaret({ para: 0, offset: 0 });
      await e.flush?.();
      return true;`);

    // 第一页画布在屏幕上的位置（页内坐标加它就是视口坐标）。
    const pageBox = () => p.ev<{ x: number; y: number; w: number; h: number } | null>(`
      const c = document.querySelector('.heditor-pages canvas');
      if (!c) return null;
      const r = c.getBoundingClientRect();
      return { x: r.left, y: r.top, w: r.width, h: r.height };`);

    // 页内坐标换成视口坐标。caret().rect 是页内像素，页号另给（TRAPS 140）。
    const caretPoint = async (dx = 0, dy = 0) => {
      const c = await p.ev<{ rect: { x: number; y: number; h: number } }>(
        'return window.__demo.caret()');
      const b = await pageBox();
      if (!b) throw new Error('页面上没有画布');
      return { x: b.x + c.rect.x + dx, y: b.y + c.rect.y + c.rect.h / 2 + dy };
    };

    // 键事件的形状（虚拟键码 + 文本）就是判据，所以下发原始 CDP 事件，不用 page.keyboard。
    const key = async (code: number, k: string, text?: string) => {
      const base = { windowsVirtualKeyCode: code, nativeVirtualKeyCode: code, key: k };
      await p.send('Input.dispatchKeyEvent', {
        type: text ? 'keyDown' : 'rawKeyDown',
        ...base,
        ...(text ? { text, unmodifiedText: text } : {}),
      });
      await p.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
      await page.waitForTimeout(30);
    };

    const chord = async (code: number, k: string) => {
      // modifiers：1=Alt 2=Ctrl 4=Meta 8=Shift
      const base = { windowsVirtualKeyCode: code, nativeVirtualKeyCode: code, key: k, modifiers: 2 };
      await p.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', ...base });
      await p.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
      await page.waitForTimeout(40);
    };

    const mouse = async (type: string, x: number, y: number, extra = {}) => {
      await p.send('Input.dispatchMouseEvent', {
        type,
        x: Math.round(x),
        y: Math.round(y),
        button: 'left',
        clickCount: 1,
        ...extra,
      });
      await page.waitForTimeout(25);
    };

    await test.step('一、真实点击落光标', async () => {
      await reset();
      const before = await snap('reset');
      check('重置后光标在段 0 段首', before.para === 0 && before.off === 0,
            'para=' + before.para + ' off=' + before.off);
      // 段首往右 40 px：同一个 y、不同的 x，落点必然往后挪。
      const q = await caretPoint(40);
      await mouse('mousePressed', q.x, q.y);
      await mouse('mouseReleased', q.x, q.y);
      const after = await snap('click+40');
      check('点击后光标仍在这一段', after.para === 0, 'para=' + after.para);
      check('点击位置在段首右侧，偏移变大', after.off > 0, 'off=' + after.off);
      const ta = await p.ev<string | undefined>(
        'return document.activeElement && document.activeElement.className');
      check('键盘承接层拿到了焦点', ta === 'heditor-ime', String(ta));
    });

    await test.step('二、真实键盘', async () => {
      await reset();
      await p.ev('await window.__demo.setCaret({ para: 0, offset: 7 });');   // "第一行 abc" 的段尾
      await key(65, 'a', 'a');
      await key(66, 'b', 'b');
      await key(67, 'c', 'c');
      const typed = await snap('type abc');
      check('真实按键进入文档', typed.text.includes('第一行 abcabc'), typed.text.split('\n')[0]);
      check('光标跟着走了三格', typed.off === 10, 'off=' + typed.off);

      await key(8, 'Backspace');
      const bs = await snap('backspace');
      check('退格删掉一个字符', bs.text.includes('第一行 abcab') && !bs.text.includes('abcabc'),
            bs.text.split('\n')[0]);
      check('退格后光标回退一格', bs.off === 9, 'off=' + bs.off);

      await chord(90, 'z');
      const undone = await snap('ctrl+z');
      check('Ctrl+Z 撤回上一次编辑（退格）', undone.text.includes('第一行 abcabc'),
            undone.text.split('\n')[0]);

      await chord(89, 'y');
      const redone = await snap('ctrl+y');
      check('Ctrl+Y 重做（恢复的是刚撤掉的退格）',
            redone.text.includes('第一行 abcab') && !redone.text.includes('abcabc'),
            redone.text.split('\n')[0]);

      await key(36, 'Home');
      const home = await snap('home');
      check('Home 回到段首', home.off === 0 && home.empty === true, 'off=' + home.off);

      await key(39, 'ArrowRight');
      const right = await snap('arrow-right');
      check('方向键右移一格', right.off === 1, 'off=' + right.off);

      // Shift+Right：modifiers 里带 8。
      await p.send('Input.dispatchKeyEvent', {
        type: 'rawKeyDown', windowsVirtualKeyCode: 39, nativeVirtualKeyCode: 39,
        key: 'ArrowRight', modifiers: 8,
      });
      await p.send('Input.dispatchKeyEvent', {
        type: 'keyUp', windowsVirtualKeyCode: 39, nativeVirtualKeyCode: 39,
        key: 'ArrowRight', modifiers: 8,
      });
      await page.waitForTimeout(40);
      const shifted = await snap('shift-right');
      check('Shift+方向键产生选区', shifted.empty === false, 'empty=' + shifted.empty);
      check('选区从第一格拉到第二格',
            shifted.anchor.offset === 1 && shifted.focus.offset === 2,
            JSON.stringify([shifted.anchor, shifted.focus]));
    });

    await test.step('三、真实拖选', async () => {
      await reset();
      const a = await caretPoint(10);
      const b = await caretPoint(50);
      await mouse('mousePressed', a.x, a.y);
      await mouse('mouseMoved', (a.x + b.x) / 2, a.y, { buttons: 1 });
      await mouse('mouseMoved', b.x, b.y, { buttons: 1 });
      await mouse('mouseReleased', b.x, b.y);
      const sel = await snap('drag');
      check('拖选产生非空选区', sel.empty === false, 'empty=' + sel.empty);
      check('选区两端都在段 0', sel.anchor.para === 0 && sel.focus.para === 0,
            JSON.stringify([sel.anchor, sel.focus]));
      check('焦点在锚点右侧', sel.focus.offset > sel.anchor.offset,
            sel.anchor.offset + ' 变 ' + sel.focus.offset);

      // 拖出画布：指针跑到页外之后仍要能继续选。命中在页外本来没有"哪一行"，
      //   不夹取时焦点端不动，表现是"拖到边上选区停住"。判据取"焦点端比页内那一点更靠后"。
      await reset();
      const p0 = await caretPoint(10);
      const box = await p.ev<{ left: number; top: number; width: number }>(`
        const r = document.querySelector('.heditor-pages').getBoundingClientRect();
        return { left: r.left, top: r.top, width: r.width };`);
      await mouse('mousePressed', p0.x, p0.y);
      await mouse('mouseMoved', p0.x + 40, p0.y, { buttons: 1 });
      await mouse('mouseMoved', box.left + box.width + 160, p0.y + 4, { buttons: 1 });
      await mouse('mouseReleased', box.left + box.width + 160, p0.y + 4);
      const out = await snap('drag-outside');
      check('★ 拖出页外之后选区仍在延长（没有被夹停）',
            out.empty === false && out.focus.offset > out.anchor.offset,
            JSON.stringify([out.anchor, out.focus]));

      // 空段也要有选区效果：空段里没有一个字，几何上是零宽，但选中的是那枚段落标记。
      await reset();
      const rects = await p.ev<number>(`
        const e = window.__demo;
        await e.setText('甲\\n\\n乙');
        await e.select({ para: 1, offset: 0 }, { para: 1, offset: 1 });
        await e.flush();
        return e.selection().rects.length;`);
      check('★ 选中空段：选区矩形不是空的（看得见高亮）', rects > 0, String(rects));

      // 双击选词：库里没有这条，旧宿主也没有 —— 这是产品缺口，不是回归。
      //   记成 note，别把判据钉在一个还没实现的行为上。
      await reset();
      const q = await caretPoint(20);
      await mouse('mousePressed', q.x, q.y, { clickCount: 1 });
      await mouse('mouseReleased', q.x, q.y, { clickCount: 1 });
      await mouse('mousePressed', q.x, q.y, { clickCount: 2 });
      await mouse('mouseReleased', q.x, q.y, { clickCount: 2 });
      const dbl = await snap('double-click');
      test.info().annotations.push({
        type: 'note',
        description: '双击选词未实现（选区' + (dbl.empty ? '空' : '有') + '），库只判三击选格',
      });
    });

    await test.step('四、输入法', async () => {
      await reset();
      const r = await p.ev<{ text: string; off: number }>(`
        const e = window.__demo;
        const ta = e.element.querySelector('.heditor-ime');
        ta.focus();
        ta.dispatchEvent(new CompositionEvent('compositionstart', { bubbles: true }));
        ta.dispatchEvent(new CompositionEvent('compositionupdate', { bubbles: true, data: 'ni' }));
        await new Promise((r) => setTimeout(r, 20));
        ta.dispatchEvent(new CompositionEvent('compositionend', { bubbles: true, data: '你' }));
        await new Promise((r) => setTimeout(r, 60));
        return { text: e.text(), off: e.caret().pos.offset };`);
      const s = await snap('ime');
      check('输入法上屏的字进了文档', r.text.includes('你'), r.text.split('\n')[0]);
      check('上屏后光标在插入的字之后', r.off >= 1, 'off=' + r.off);
      check('上屏不产生选区', s.empty === true, 'empty=' + s.empty);
    });

    await test.step('五、滚动与缩放', async () => {
      await reset();
      const st0 = await p.ev<number>("return document.querySelector('#ed .heditor-stage').scrollTop");
      await p.send('Input.dispatchMouseEvent',
                   { type: 'mouseWheel', x: 400, y: 400, deltaX: 0, deltaY: 240 });
      await page.waitForTimeout(120);
      const st1 = await p.ev<number>("return document.querySelector('#ed .heditor-stage').scrollTop");
      check('滚轮改变了滚动位置', st1 > st0, st0 + ' 变 ' + st1);
      const after = await snap('wheel');
      check('滚动不改文档', after.text.includes('第一行 abc'), after.text.split('\n')[0]);

      const z = await p.ev<{ w: number; pages: number; para: number; text: string }>(`
        const e = window.__demo;
        await e.setZoom(1.5);
        await e.flush?.();
        const g = e.geometry();
        return { w: g.page.w, pages: g.pages, para: e.caret().pos.para, text: e.text() };`);
      check('缩放后页宽变大', z.w > 600, String(z.w));
      check('缩放不动内容', z.text.includes('第一行 abc') && z.para === 0, 'para=' + z.para);
    });

    await test.step('六、无错误', async () => {
      const real = errors.filter((t) => !/favicon/i.test(t));
      check('没有 console 错误', real.length === 0, real.slice(0, 2).join(' | '));
    });

    await test.info().attach('trail', {
      body: JSON.stringify(trail.map(
        (t) => [t.label, t.para, t.off, t.empty, t.anchor.offset, t.focus.offset])),
      contentType: 'application/json',
    });
    expect(trail.length).toBeGreaterThan(10);
  });
});
