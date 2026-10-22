import { defineConfig } from 'vite';

/**
 * 只打包 web/src/（公开层）。旧的宿主脚本与 emscripten 胶水不进 bundle ——
 * 它们由库在运行期按地址加载（assets.base），这样 wasm 构建与前端构建互不牵连。
 *
 * 两种产物：ESM（heditor.mjs）给 npm 包用，import { createEditor } from 'heditor'；
 *   IIFE（heditor.iife.js）给 <script> 直接用，全局名 HEditor。
 */
export default defineConfig({
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: 'es2020',
    lib: {
      entry: 'src/index.ts',
      name: 'HEditor',
      formats: ['es', 'iife'],
      fileName: (format) => (format === 'es' ? 'heditor.mjs' : 'heditor.iife.js'),
    },
  },
});
