import { createReadStream, existsSync, statSync } from 'node:fs';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig, type Plugin } from 'vite';

/**
 * 开发服务器的根是 web/（页面与源码都在这一层），但运行期要取两处"仓库根"的东西：
 *   build/wasm/web/*  引擎胶水与 wasm
 *   assets/fonts/*    三份字体
 * 它们不在 web/ 下面，静态服务器不会发。这一个中间件把它们从仓库根发出去 ——
 *   比把 wasm 拷进 web/ 更诚实：这些文件的位置是构建产物的约定，不该为开发模式复制一份。
 */
const REPO = fileURLToPath(new URL('..', import.meta.url));

const MIME: Record<string, string> = {
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.ttf': 'font/ttf',
  '.otf': 'font/otf',
  '.json': 'application/json; charset=utf-8',
  '.md': 'text/markdown; charset=utf-8',
};

function repoServe(): Plugin {
  return {
    name: 'heditor-repo-serve',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const url = (req.url ?? '').split('?')[0];
        if (!/^\/(build|assets)\//.test(url)) return next();
        const file = join(REPO, normalize(decodeURIComponent(url)));
        if (!file.startsWith(REPO) || !existsSync(file) || !statSync(file).isFile()) return next();
        res.setHeader('Content-Type', MIME[extname(file)] ?? 'application/octet-stream');
        // 引擎胶水与 wasm 是固定文件名，浏览器与 Worker 会照常缓存 —— 改了 C++ 重新构建
        //   之后页面还跑旧的，表现是"改了没反应，而产物时间戳是新的"。开发服务器这两个
        //   目录一律不许缓存；生产由调用方给 bust（见 editor.ts 的 assets.bust）。
        res.setHeader('Cache-Control', 'no-store');
        createReadStream(file).pipe(res);
      });
    },
  };
}

/**
 * 示例页的开发/构建配置。
 *
 * 开发（npm run dev）：Vite 直接按源码编译 web/src，改一行就热更，不用先构建产物。
 * 构建（npm run build:app）：吐到 web/dist-app/。它仍然在运行期按地址取 build/wasm/web/
 *   与 assets/，所以静态服务器的根必须是仓库根（探针就是这么跑的）。
 */
export default defineConfig(({ command }) => ({
  root: fileURLToPath(new URL('.', import.meta.url)),
  // 示例页没有 public/ 目录：素材都在仓库根，由上面的中间件与静态服务器负责。
  publicDir: false,
  // base 只在构建时是 /web/dist-app/（产物放在 web/ 下面、由仓库根的静态服务器发）。
  //   开发时必须留在 /，否则 dev server 把页面也挂到那个前缀上，路径与源码里的绝对
  //   引用（/app/main.ts）对不上。
  base: command === 'build' ? '/web/dist-app/' : '/',
  server: {
    port: 5173,
    strictPort: true,
    host: '0.0.0.0',
    // 开发期既不热更也不整页重载，改完自己刷新。
    //   只关 hmr 不够：文件监视还在，改动一到 Vite 仍会推一次 full-reload ——
    //   而这一页的实例把白名单、协议表、宿主事件全都捕获进了闭包，重载换不掉它们，
    //   只会把文档状态清空。两处一起关，watcher 也就不必占着 CPU 了。
    hmr: false,
  },
  plugins: [repoServe()],
  build: {
    outDir: 'dist-app',
    emptyOutDir: true,
    target: 'es2020',
    rollupOptions: {
      // 两个入口：示例页与"同页两个实例"。后者是多实例的唯一验收面。
      input: {
        index: fileURLToPath(new URL('index.html', import.meta.url)),
        multi: fileURLToPath(new URL('multi.html', import.meta.url)),
      },
    },
  },
}));
