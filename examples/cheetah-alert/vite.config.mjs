import { resolve } from 'node:path';
const root = import.meta.dirname;
const repo = resolve(root, '../..');
const runtime = resolve(repo, 'examples/react-vite/node_modules');
const zustand = resolve(repo, 'third_party/zustand/source/esm');
export default ({ mode }) => ({
  root, base: './',
  resolve: { alias: [
    { find: /^react$/, replacement: resolve(runtime, 'react/index.js') },
    { find: /^react\/jsx-runtime$/, replacement: resolve(runtime, 'react/jsx-runtime.js') },
    { find: /^react-dom\/client$/, replacement: resolve(runtime, 'react-dom/client.js') },
    { find: /^zustand$/, replacement: resolve(zustand, 'index.mjs') },
    { find: /^zustand\/react$/, replacement: resolve(zustand, 'react.mjs') },
    { find: /^zustand\/vanilla$/, replacement: resolve(zustand, 'vanilla.mjs') },
    { find: '@', replacement: resolve(repo, 'tests/fixtures/casecheetah/UI/src') },
  ] },
  define: { 'process.env.NODE_ENV': JSON.stringify('production') },
  build: mode === 'native' ? {
    outDir: 'dist/native', target: 'es2020', minify: false,
    lib: { entry: resolve(root, 'src/main.tsx'), name: 'CaseCheetahAlert', formats: ['iife'], fileName: () => 'app.js', cssFileName: 'profile' },
  } : { outDir: 'dist/browser', target: 'es2020', minify: false, modulePreload: false },
});
