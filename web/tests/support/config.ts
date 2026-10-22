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
    // 并行度默认 2：每个 worker 各自编译一份 wasm、各自解析中文字体，开满核不一定更快。
    workers: Number(process.env.HEDITOR_TEST_WORKERS ?? 2),
    timeout: 180_000,
    expect: { timeout: 5_000 },
    reporter: [
      ['list'],
      ['json', { outputFile: join(ROOT, 'out/web/pw-results.json') }],
      ['html', { outputFolder: join(ROOT, 'out/web/pw-report'), open: 'never' }],
    ],
    outputDir: join(ROOT, 'out/web/pw-artifacts'),
    use: {
      baseURL: 'http://' + host + ':' + port,
      channel: 'chrome',
      headless: true,
      // 视口与 DPR 是判据的一部分：一批断言是绝对像素账，改了它们就不是同一个基线。
      viewport: { width: 1500, height: 1200 },
      deviceScaleFactor: 1,
      trace: 'retain-on-failure',
      screenshot: 'only-on-failure',
      launchOptions: {
        args: ['--hide-scrollbars', '--disable-gpu', '--no-first-run', '--no-default-browser-check'],
      },
      pageBase,
      wasmDir,
    },
    projects: [
      { name: 'inline', use: { transport: 'inline' } },
      { name: 'worker', use: { transport: 'worker' } },
    ],
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
