import { defineConfig } from 'vite';

/**
 * Worker 壳：web/src/transport/worker_main.ts → web/dist/heditor_worker.js。
 *
 * 为什么是经典脚本而不是 ESM：那一侧要用 importScripts 装 emscripten 的胶水
 *   （它不是模块），而 importScripts 在 module worker 里不可用。IIFE 就够了 ——
 *   这一份只被 new Worker(url) 装一次。
 *
 * 构建顺序要紧：先 vite build（会清空 dist），再这一份；反过来会把 worker 壳清掉。
 */
export default defineConfig({
  build: {
    outDir: 'dist',
    emptyOutDir: false,
    target: 'es2020',
    minify: false,
    lib: {
      entry: 'src/transport/worker_main.ts',
      name: '__heditorWorker',
      formats: ['iife'],
      fileName: () => 'heditor_worker.js',
    },
  },
});
