// 公开 API 的示例页。
//
// 这一份代码是"外部使用者"的样板：只允许用 HEditor.createEditor 与方法。
//   不许出现 window.__heditor / engine() / mod / api.read / api.write / HEAP / FS，
//   也不许自己 addEventListener 键盘事件 —— 输入层在库里。
//   判据：node scripts/api_check.mjs（静态）+ scripts/demo_probe.mjs（运行时）。
//
// 这里用 window.__demo 把实例交出去，只为让无头探针能拿到它；产品页不需要这一行。
const DEMO_TEXT = [
  'HEditor 示例文档',
  '',
  '这一页只加载了两个脚本：库产物与这份示例代码。引擎（wasm）、命令表、输入层、',
  '画布与滚动都在库里面 —— 页面拿到的是一个编辑器对象，不是一个毛坯房。',
  '',
  '可以试试：点画布落光标、敲字、Ctrl+Z 撤销、拖选。这些都没有在本文件里写一行事件处理。',
].join('\n');

const status = document.getElementById('status');
const el = document.getElementById('ed');

function show(text) {
  status.textContent = text;
}

(async () => {
  // 传输由地址决定：默认同线程（不依赖 Worker，控制台里最好查），?transport=worker
  //   换成引擎在另一条线程上跑 —— 两条路共用同一份 Editor 实现。
  const transport = new URLSearchParams(location.search).get('transport') === 'worker'
    ? 'worker'
    : 'inline';
  const editor = await HEditor.createEditor({
    container: el,
    transport,
    chrome: true,
  });
  window.__demo = editor;

  await editor.setText(DEMO_TEXT);
  // 探针要读的两个数（产品页不需要这一行）：变更次数与就绪标志。
  window.__demoState = { changes: 0, transport };
  editor.on('change', () => {
    ++window.__demoState.changes;
    const g = editor.geometry();
    show(`${editor.paragraphs().length} 段 · ${g.pages} 页 · 变更 ${window.__demoState.changes} 次`);
  });

  const g = editor.geometry();
  show(`${editor.paragraphs().length} 段 · ${g.pages} 页 · 就绪`);

  let mode = 'paged';
  document.querySelector('header').addEventListener('click', async (e) => {
    const act = e.target?.dataset?.act;
    if (!act) return;
    if (act === 'load') await editor.setText(DEMO_TEXT);
    if (act === 'type') await editor.type('（从这里开始编辑）');
    if (act === 'undo') await editor.undo();
    if (act === 'redo') await editor.redo();
    if (act === 'zoomIn') await editor.setZoom(Math.min(2.25, editor.config().zoom + 0.125));
    if (act === 'zoomOut') await editor.setZoom(Math.max(0.375, editor.config().zoom - 0.125));
    if (act === 'toggleMode') {
      mode = mode === 'paged' ? 'continuous' : 'paged';
      await editor.configure({ pageMode: mode });
    }
    if (act === 'save') {
      const json = await editor.save();
      // 示例里只把结果放到控制台 —— 产品里该做的是下载或上传。
      console.log('save:', json.length, 'bytes');
      show(`已序列化 ${json.length} 字节`);
    }
  });

  // 给探针一个"排空"点：所有 fire-and-forget 的调用之后读这个。
  window.__demoReady = true;
})();
