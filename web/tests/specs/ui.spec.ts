// 弹窗与控件层：对话框的位置与拖拽、下拉的搜索、滑块的取值。
//
// 为什么单独一份：这三样都住在示例页（web/app）里，前面的 spec 只从"引擎语义"这一侧看
//   它们 —— 属性面板的字段对不对、菜单项点下去有没有落到文档。而"面板开在哪、拖不拖得动、
//   下拉能不能搜、滑块点在轨道上是不是改到那个位置"没有地方管，全看手感。
import { check } from '../support/check';
import { expect, test } from '../support/probe';

/**
 * 点功能区上那个会开面板的按钮（按 DOM 点，与工具栏那半同一手法）。
 * @param page 页面
 * @param act 动作名
 */
async function press(page: any, act: string): Promise<void> {
  await page.evaluate((name: string) => {
    const b = document.querySelector(`.heditor-ribbon__button[data-act="${name}"]`);
    if (b instanceof HTMLElement) b.click();
  }, act);
}

test.describe('弹窗与控件层', () => {
  test('对话框：居中打开，标题栏拖得动，点遮罩与 Esc 都关', async ({ open }) => {
    const p = await open('index.html', {
      query: { debug: '1' },
      viewport: { width: 1200, height: 800 },
    });
    await p.read(`await H.setText('甲乙'); await H.flush(); return true;`);
    await press(p.page, 'page-setup');
    const panel = p.page.locator('.heditor-props');
    await expect(panel).toBeVisible();

    const box = await panel.boundingBox();
    const view = p.page.viewportSize() ?? { width: 1200, height: 800 };
    check('★ 左右居中（误差 12 像素以内）',
      box !== null && Math.abs(box.x + box.width / 2 - view.width / 2) < 12,
      JSON.stringify(box));
    check('不顶在视口最上面（重心偏上三分之一）', box !== null && box.y > 60,
      String(box === null ? 'n/a' : Math.round(box.y)));
    check('底下铺了一层遮罩', (await p.page.locator('.heditor-scrim').count()) === 1, '0');

    // 拖标题栏：整块跟着走（拖的是一块空白处，不是关闭按钮）。
    const bar = p.page.locator('.heditor-props__bar');
    const bb = await bar.boundingBox();
    if (bb === null) throw new Error('标题栏没有几何');
    await p.page.mouse.move(bb.x + 30, bb.y + bb.height / 2);
    await p.page.mouse.down();
    await p.page.mouse.move(bb.x + 30 - 140, bb.y + bb.height / 2 + 70, { steps: 6 });
    await p.page.mouse.up();
    const moved = await panel.boundingBox();
    check('★ 拖过标题栏之后面板跟着走',
      moved !== null && box !== null && moved.x < box.x - 100 && moved.y > box.y + 50,
      JSON.stringify(moved));

    // 点遮罩关掉。
    await p.page.mouse.click(6, view.height - 6);
    await expect(panel).toHaveCount(0);
    check('点遮罩之后面板收了', (await p.page.locator('.heditor-scrim').isHidden()) === true,
      'still visible');

    // 再开一次，Esc 关。
    await press(p.page, 'page-setup');
    await expect(panel).toBeVisible();
    await p.page.keyboard.press('Escape');
    await expect(panel).toHaveCount(0);
  });

  test('下拉：候选多时出搜索框，过滤后只剩匹配项，选中回填到触发器', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 先选一段字：字号是字符格式，只有光标时改不上，回显会被真实值刷回去 ——
    //   那样这条判据就变成"比谁跑得快"，而不是"选中之后回填对不对"。
    await p.read(`
      await H.setText('甲乙丙丁');
      await H.flush();
      await H.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      return true;
    `);
    const trigger = p.page.locator('.heditor-select[data-act="size"]');
    await trigger.click();
    const pop = p.page.locator('.heditor-pop');
    await expect(pop).toBeVisible();

    const search = pop.locator('.heditor-select__search');
    check('★ 候选项多的下拉带搜索框', (await search.count()) === 1, '0');
    const all = await pop.locator('.heditor-select__item').count();
    check('列表里是全部候选', all >= 10, String(all));

    // 在候选列表里滚一下：浮层自己内部滚动不算"页面滚了"，下拉必须还开着。
    await pop.locator('.heditor-select__list').hover();
    await p.page.mouse.wheel(0, 140);
    await p.page.waitForTimeout(200);
    check('★ 在候选列表里滚动不会把下拉关掉', await pop.isVisible(), 'closed');

    await search.fill('1');
    const filtered = await pop.locator('.heditor-select__item').count();
    check('★ 输入之后只剩匹配的项', filtered > 0 && filtered < all,
      all + ' → ' + filtered);

    // 清空过滤词，选一项（用 textContent 找，值不一定是数字）。
    await search.fill('');
    const target = pop.locator('.heditor-select__item', { hasText: /^18$/ }).first();
    await target.click();
    await expect(pop).toBeHidden();
    await expect
      .poll(async () => await trigger.locator('.heditor-select__text').textContent(), { timeout: 3000 })
      .toBe('18');
    const shown = await trigger.locator('.heditor-select__text').textContent();
    check('★ 选中之后触发器上换成那一项', shown === '18', String(shown));
    check('选中之后浮层收了', (await pop.isHidden()) === true, 'still visible');
  });

  test('滑块：点轨道改值，方向键走一档', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const pct = async (): Promise<string> =>
      (await p.page.locator('.heditor-status__pct').textContent()) ?? '';
    const p0 = await pct();
    const slider = p.page.locator('.heditor-slider');
    const box = await slider.boundingBox();
    if (box === null) throw new Error('滑块没有几何');

    await p.page.mouse.click(box.x + box.width * 0.85, box.y + box.height / 2);
    await expect
      .poll(async () => await pct(), { timeout: 2000 })
      .not.toBe(p0);
    const p1 = await pct();
    check('★ 点在轨道靠右处：缩放变大', parseInt(p1, 10) > parseInt(p0, 10), p0 + ' → ' + p1);

    await slider.focus();
    await p.page.keyboard.press('ArrowLeft');
    await expect
      .poll(async () => await pct(), { timeout: 2000 })
      .not.toBe(p1);
    const p2 = await pct();
    check('方向键退一档', parseInt(p2, 10) < parseInt(p1, 10), p1 + ' → ' + p2);
  });

  test('功能区：三个页签都是一行高，下拉开在按钮下面且键盘归菜单', async ({ open }) => {
    // 这条挡的是"往组里塞一个长竖排栈"：项行的设计高度是 56（大按钮 54 + 间隔），
    //   栈只有两个时才与它同高。多出来的项不会溢出，而是把整条功能区顶高 ——
    //   页面顶部多出上百像素的空白，按钮却还浮在中间。量高度是唯一能发现它的办法。
    const p = await open('index.html', { query: { debug: '1' } });

    const rows = await p.read<any>(`
      const tabs = [...document.querySelectorAll('.heditor-ribbon__tab')];
      const out = [];
      for (const t of tabs) {
        t.click();
        const pane = document.querySelector('.heditor-ribbon__pane--active');
        const items = [...pane.querySelectorAll('.heditor-ribbon__items')];
        const heights = items.map((el) => +el.getBoundingClientRect().height.toFixed(1));
        out.push({
          tab: t.textContent,
          max: heights.length ? Math.max(...heights) : 0,
          ribbon: +document.querySelector('.heditor-ribbon').getBoundingClientRect().height.toFixed(1),
        });
      }
      return out;`);
    for (const r of rows) {
      check(`★ 页签「${r.tab}」的项行不超过一行的高度`, r.max <= 57, `${r.max} px`);
      check(`页签「${r.tab}」的功能区不超过 120 px`, r.ribbon <= 120, `${r.ribbon} px`);
    }
    // 切页签不该让文档上下跳：项数不一样，高度钉住才有这个效果。
    const spread = Math.max(...rows.map((r: any) => r.ribbon)) - Math.min(...rows.map((r: any) => r.ribbon));
    check('切页签时功能区高度不变', spread <= 1, spread + ' px');

    // 下拉：开在按钮下面、键盘归它、Esc 收起并把焦点还回编辑器。
    await p.page.locator('.heditor-ribbon__tab').first().click();
    const btn = p.page.locator('.heditor-ribbon__button[data-act="sample"]');
    await btn.click();
    const menu = p.page.locator('.heditor-menu:not([hidden])');
    await expect(menu).toBeVisible();
    const bb = await btn.boundingBox();
    const mb = await menu.boundingBox();
    check('★ 下拉开在按钮下面（左右对齐，纵向贴着按钮底边）',
      bb !== null && mb !== null &&
        Math.abs(mb.x - bb.x) <= 2 && mb.y >= bb.y + bb.height - 1 && mb.y <= bb.y + bb.height + 8,
      JSON.stringify({ btn: bb, menu: mb }));
    check('菜单里是五份样例',
      (await menu.locator('.heditor-menu__item').count()) === 5,
      String(await menu.locator('.heditor-menu__item').count()));

    const focusIn = await p.read<boolean>(
      'return document.activeElement !== null && document.activeElement.closest(".heditor-menu") !== null;');
    check('★ 菜单开着时焦点在菜单上（否则方向键会去挪正文光标）', focusIn === true, String(focusIn));

    // 键盘走一项：高亮从第一项挪到第二项。
    const activeBefore = await p.read<string | null>(`
      const el = document.querySelector('.heditor-menu__item--active');
      return el ? el.dataset.act || null : null;`);
    await p.page.keyboard.press('ArrowDown');
    const activeAfter = await p.read<string | null>(`
      const el = document.querySelector('.heditor-menu__item--active');
      return el ? el.dataset.act || null : null;`);
    check('★ 方向键在菜单里走一项', activeBefore !== activeAfter,
      `${activeBefore} → ${activeAfter}`);

    await p.page.keyboard.press('Escape');
    await expect(menu).toBeHidden();
    const back = await p.read<string>(`return document.activeElement?.className || '';`);
    check('Esc 收起之后焦点回到编辑器的承接层', back === 'heditor-ime', back);
  });
});
