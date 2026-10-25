// 场地配置的两种口径：跑构建产物（验收基准），或跑 Vite 源码直连（改一行就能测）。
//
// 两者的差别只有三处：静态服务是谁、页面在哪个路径、端口。
//   判据、视口、浏览器、断言一条都不变 —— 换口径不改判据是这里的前提。
import { existsSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { defineConfig, type PlaywrightTestConfig } from '@playwright/test';

import type { ProbeOptions } from './probe';

// 跑哪一份页面：构建产物是验收基准，Vite 源码直连是日常改代码用的快口径。
export type TestMode = 'dist' | 'dev';

// 本仓库根目录，已去掉末尾分隔符。
export const ROOT = fileURLToPath(new URL('../../..', import.meta.url)).replace(/[\\/]+$/, '');

const wasmDir = process.env.HEDITOR_WASM ?? 'build/wasm/web';
const python = process.env.HEDITOR_PYTHON ?? 'python';

/**
 * 按口径造配置。
 *
 * @param mode 跑构建产物还是跑 Vite 源码直连
 * @returns Playwright 配置
 */
export function makeConfig(mode: TestMode): PlaywrightTestConfig<ProbeOptions> {
  const glue = join(ROOT, wasmDir, 'heditor.wasm');
  if (!existsSync(glue)) {
    throw new Error('还没构建 WASM：' + glue + '（先跑 cmake --workflow --preset wasm）');
  }

  // 开发口径只省掉库与页面的打包：Worker 壳是经典 worker 产物，源码跑不起来，仍需构建。
  const workerShell = join(ROOT, 'web', 'dist', 'heditor_worker.js');
  if (mode === 'dev' && !existsSync(workerShell)) {
    throw new Error('还没构建 Worker 壳：' + workerShell + '（先跑 npm run build:worker）');
  }

  const port = Number(process.env.HEDITOR_TEST_PORT ?? (mode === 'dev' ? 5173 : 8781));
  const pageBase = mode === 'dev' ? '/' : '/web/dist-app/';
  // 主机名跟服务走：Vite 默认只监听 localhost（Windows 上解析成 ::1），
  //   拿 127.0.0.1 去探它会连接拒绝，于是复用判断落空、又去起第二个把端口撞掉。
  const host = mode === 'dev' ? 'localhost' : '127.0.0.1';

  return {
    testDir: './tests/specs',
    fullyParallel: true,
    forbidOnly: !!process.env.CI,
    // 并行度默认 4（本机四个物理核）。实测六 worker 更慢：每个 worker 各自编译一份 wasm、
    //   各自解析中文字体，再往上只是互相抢内存带宽，还会把负载敏感的判据顶红。
    workers: Number(process.env.HEDITOR_TEST_WORKERS ?? 4),
    timeout: 180_000,
    expect: { timeout: 5_000 },
    reporter: [
      ['list'],
      ['json', { outputFile: join(ROOT, 'out/web/pw-results.json') }],
      ['html', { outputFolder: join(ROOT, 'out/web/pw-report'), open: 'never' }],
    ],
    // 每次运行一个自己的产物目录。理由不是洁癖：Playwright 开跑前会整目录删除，
    //   而本机的安全删除守卫对单次超过 50 个目标的批量删除会拒绝执行 —— 同一目录复用
    //   几十轮之后，测试还没开始就被守卫拦在门外。
    outputDir: join(ROOT, 'out/web/pw-runs', process.env.HEDITOR_TEST_RUN ??
      new Date().toISOString().replace(/[^0-9]/g, '').slice(0, 14)),
    use: {
      baseURL: 'http://' + host + ':' + port,
      channel: 'chrome',
      headless: true,
      // 视口与 DPR 钉死：判据是按这个视口写的，换视口就是换一份基线。
      //   DPR 多钉一层是因为鼠标与拖选的坐标换算在 DPR=1 时最简 —— 实测 DPR=2 时
      //   帧自审与"屏幕==引擎帧"仍然成立（光栅在 wasm 里，与宿主 DPR 无关），
      //   但那一批"磅 → 屏幕像素"的换算会错位，不是判据的账。
      viewport: { width: 1500, height: 1200 },
      deviceScaleFactor: 1,
      // 默认不录 trace：它给每个 test 持续截图并抓 DOM 快照，全量因此多花近一倍时间
      //   （实测用例耗时之和 180 s → 298 s，最长的那份 45 s → 86 s）。失败时仍有截图、
      //   报错与附件；要回放就设 HEDITOR_TEST_TRACE=1 重跑那一条。
      trace: process.env.HEDITOR_TEST_TRACE ? 'retain-on-failure' : 'off',
      screenshot: 'only-on-failure',
      launchOptions: {
        args: ['--hide-scrollbars', '--disable-gpu', '--no-first-run', '--no-default-browser-check'],
      },
      pageBase,
      wasmDir,
    },
    projects: [{ name: 'chromium' }],
    webServer: {
      command: serverCommand(mode, port),
      url: 'http://' + host + ':' + port + pageBase + 'index.html',
      reuseExistingServer: true,
      timeout: 60_000,
      stdout: 'ignore',
      stderr: 'pipe',
    },
  };
}

/**
 * 性能判据的配置：场地与功能判据相同，但独占一个 worker。
 *
 * 耗时比值那几条（增量对全量、页指纹对整篇）会被同机上的其它 test 顶翻，所以它们单独
 * 成一份 spec、单独一条命令；跑的时候机器上不该有别的活。
 *
 * @param mode 跑构建产物还是跑 Vite 源码直连
 * @returns Playwright 配置
 */
export function makePerfConfig(mode: TestMode): PlaywrightTestConfig<ProbeOptions> {
  const base = makeConfig(mode);
  return {
    ...base,
    testDir: './tests/perf',
    fullyParallel: false,
    workers: 1,
    // 录 trace 会持续截图，把正在量的那段拖慢。
    use: { ...base.use, trace: 'off' },
  };
}

/**
 * 起对应口径的服务。
 *
 * @param mode 口径
 * @param port 端口
 * @returns 命令行
 */
function serverCommand(mode: TestMode, port: number): string {
  if (mode === 'dev') return 'npx vite --port ' + port + ' --strictPort';
  // 目录带引号且必须没有结尾分隔符，否则 Windows 上 "\"" 会把收尾引号转义掉。
  return '"' + python + '" "' + join(ROOT, 'scripts', 'dev_server.py') + '" --port ' + port +
    ' --root "' + ROOT + '"';
}
