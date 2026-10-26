// 属性面板判据：字段表由库算（describeControl / describeTable），提交只发变化量。
//
// 为什么单独一份：字段表一旦由每个宿主各写一遍，写歪一条不会抛异常 —— 只表现为
//   "改完看着没变"或"撤销要按三次"。所以按三条来判：字段表随类型与三层记录走、
//   一层里改几项都只算一次提交、没改动不产生撤销步。
import { check } from '../support/check';
import { expect, test } from '../support/probe';

/**
 * 等一个条件在页面里成立，最多等 ms 毫秒。
 *
 * 面板的"确定"是把改动排进队列的（不阻塞界面），所以点完再读会拿到改动之前的那一拍。
 */
const UNTIL = (expr: string, ms = 1000): string =>
  `for (let i = 0; i < ${Math.ceil(ms / 25)}; i++) {
     if (${expr}) break;
     await new Promise((r) => setTimeout(r, 25));
   }`;

test.describe('属性面板', () => {
  test('表格属性：面板字段来自库，改一层两项一起落，一步撤销回到原样', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙');
      await H.flush();
      await H.setCaret({ para: 0, offset: 2 });
      await H.insertTable({ rows: 1, cols: 2 });
      await H.flush();
      await H.setCaret({ para: 1, offset: 0 });
      await H.flush();
      await D.stopBlink();
      const c = H.caret();
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               x: c.rect.x, y: c.rect.y, h: c.rect.h };
    `);
    const px = geo.left + (geo.x + 8) * geo.sx;
    const py = geo.top + (geo.y + geo.h / 2) * geo.sy;
    await p.page.mouse.click(px, py, { button: 'right' });
    await p.page.locator('.heditor-menu__item', { hasText: '表格属性…' }).click();

    const panel = p.page.locator('.heditor-props');
    await expect(panel).toBeVisible();
    const acts = await panel.locator('[data-act^="prop-"]').evaluateAll((els) =>
      els.map((el) => el.getAttribute('data-act')),
    );
    check('★ 面板三层都在（表宽 / 行高 / 垂直对齐）',
      ['prop-tableWidth', 'prop-rowHeight', 'prop-cellVAlign'].every((a) => acts.includes(a)),
      JSON.stringify(acts));
    check('面板上有自动档的说明（磅数写 0 而不是 -1）',
      (await panel.locator('[data-act="prop-rowHeight"]').inputValue()) === '0',
      await panel.locator('[data-act="prop-rowHeight"]').inputValue());

    await panel.locator('[data-act="prop-rowHeight"]').fill('42');
    await panel.locator('[data-act="prop-rowRule"]').selectOption('exact');
    await panel.locator('[data-act="prop-apply"]').click();

    const st = await p.read<any>(`
      ${UNTIL('(await H.tableState()).heightPt === 42')}
      return await H.tableState();
    `);
    check('★ 行高落到文档', st.heightPt === 42, String(st.heightPt));
    check('同一层的行高规则一起落了', st.heightRule === 'exact', st.heightRule);
    check('没碰的两层没动', st.align === 'start' && st.vAlign === 'top',
      st.align + ' / ' + st.vAlign);

    const back = await p.read<any>(`await H.undo(); await H.flush(); return await H.tableState();`);
    check('★ 一步撤销把这一层的两项都退回去',
      back.heightPt !== 42 && back.heightRule !== 'exact',
      back.heightPt + ' / ' + back.heightRule);
  });

  test('控件属性：字段随类型给，不认识的类型只留行为那几项', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const out = {};
      await H.setText('甲');
      await H.setCaret({ para: 0, offset: 1 });
      await H.insertControl({ type: 'dropdownList', options: [{ value: 'a', label: '甲' }] });
      await H.flush();
      await H.setCaret({ para: 0, offset: 99 });
      await H.insertControl({ type: 'date' });
      await H.flush();
      const list = await H.form();
      out.dropdown = H.describeControl(list[0]).fields.map((f) => f.key);
      out.date = H.describeControl(list[1]).fields.map((f) => f.key);
      out.title = H.describeControl(list[1]).title;
      return out;
    `);
    check('★ 下拉给选项表与自由输入',
      r.dropdown.includes('options') && r.dropdown.includes('free'),
      JSON.stringify(r.dropdown));
    check('下拉不给日期那两项',
      !r.dropdown.includes('format') && !r.dropdown.includes('precision'),
      JSON.stringify(r.dropdown));
    check('★ 日期给格式与精度',
      r.date.includes('format') && r.date.includes('precision'),
      JSON.stringify(r.date));
    check('日期不给选项表', !r.date.includes('options'), JSON.stringify(r.date));
    check('两种类型都有行为那三项（只读 / 必填 / 编码）',
      ['readonly', 'required', 'key'].every((k) => r.date.includes(k) && r.dropdown.includes(k)),
      JSON.stringify(r.date));

    const empty = await p.read<any>(`
      const fake = (await H.form())[0];
      fake.type = 'unknownKind';
      return H.describeControl(fake).fields.map((f) => f.key);
    `);
    check('★ 不认识的类型只剩行为三项',
      empty.length === 4 && empty[3] === 'key',
      JSON.stringify(empty));
  });

  test('控件提交：只发变化量，只读与必填走同一条路，没改动不算一步', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const step1 = await p.read<any>(`
      await H.setText('甲');
      await H.setCaret({ para: 0, offset: 1 });
      await H.insertControl({ type: 'dropdownList', options: [{ value: 'a', label: '甲' }] });
      await H.flush();
      const c = (await H.form())[0];
      const changed = await H.applyControlForm(c.index, {
        options: 'x|甲\\ny|乙', readonly: true, required: true, label: '不该落',
      });
      const after = (await H.form())[0];
      const again = await H.applyControlForm(c.index, {
        options: 'x|甲\\ny|乙', readonly: true, required: true,
      });
      return { changed, again, opts: after.options, ro: after.readonly, req: after.required,
               label: after.label };
    `);
    check('★ 选项表按"值|标签"改成两项',
      step1.opts.length === 2 && step1.opts[0].value === 'x' && step1.opts[1].label === '乙',
      JSON.stringify(step1.opts));
    check('★ 只读与必填都落了',
      step1.ro === true && step1.req === true,
      String(step1.ro) + ' / ' + String(step1.req));
    check('★ 这份面板不收的键（label）没被当成改定义', step1.label === '', step1.label);
    check('再提交一次一模一样的面板值返回 false', step1.again === false, String(step1.again));

    const undo = await p.read<any>(`
      const out = [];
      for (let i = 0; i < 3; i++) {
        await H.undo();
        await H.flush();
        const c = (await H.form())[0];
        out.push({ ro: c.readonly, req: c.required, n: c.options.length });
      }
      return out;
    `);
    check('一步撤销退回必填（提交顺序里的最后一步）',
      undo[0].req === false && undo[0].ro === true && undo[0].n === 2, JSON.stringify(undo[0]));
    check('★ 三步撤到底：只读与选项表都回到改之前',
      undo[2].ro === false && undo[2].n === 1, JSON.stringify(undo[2]));
  });

  test('页面设置：宽高在固定纸张下置灰，选自定义之后可用，改完页宽跟着走', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const w0 = await p.read<number>(`return H.geometry().page.w;`);

    // 页面设置的按钮在"视图"页签里，默认那一页看不见它 —— 按 DOM 直接点，与工具栏那半同一手法。
    await p.page.evaluate(() => {
      const b = document.querySelector('.heditor-ribbon__button[data-act="page-setup"]');
      if (b instanceof HTMLElement) b.click();
    });
    const panel = p.page.locator('.heditor-props');
    await expect(panel).toBeVisible();
    const width = panel.locator('[data-act="prop-width"]');
    check('★ 固定纸张下宽高是灰的（尺寸是规格本身的一部分）', await width.isDisabled(), 'enabled');

    await panel.locator('[data-act="prop-paper"]').selectOption('Custom');
    check('★ 选自定义之后宽高可以改了', !(await width.isDisabled()), 'disabled');
    check('宽是当前纸张的尺寸（A4 21 厘米）',
      (await width.inputValue()) === '21', await width.inputValue());

    await width.fill('15');
    await panel.locator('[data-act="prop-height"]').fill('20');
    await panel.locator('[data-act="prop-apply"]').click();

    const w1 = await p.read<number>(`
      ${UNTIL('Math.abs(H.geometry().page.w - 15 * 28.3465 * 4 / 3) < 2')}
      return H.geometry().page.w;
    `);
    check('★ 应用之后页宽按填的 15 厘米走',
      Math.abs(w1 - 15 * 28.3465 * 4 / 3) < 2, w0 + ' → ' + w1);
  });
});
