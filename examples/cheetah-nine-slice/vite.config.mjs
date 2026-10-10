import { resolve } from 'node:path';
const root = import.meta.dirname;
const runtime = resolve(root, '../react-vite/node_modules');
export default ({ mode }) => ({
  root, base: './',
  resolve: { alias: [
    { find: /^react$/, replacement: resolve(runtime, 'react/index.js') },
    { find: /^react\/jsx-runtime$/, replacement: resolve(runtime, 'react/jsx-runtime.js') },
    { find: /^react-dom\/client$/, replacement: resolve(runtime, 'react-dom/client.js') },
  ] },
  define: { 'process.env.NODE_ENV': JSON.stringify('production') },
  build: mode === 'native' ? {
    outDir: 'dist/native', target: 'es2020', minify: false,
    lib: { entry: resolve(root, 'src/main.tsx'), name: 'CaseCheetahFixture', formats: ['iife'], fileName: () => 'app.js', cssFileName: 'profile' },
  } : { outDir: 'dist/browser', target: 'es2020', minify: false, modulePreload: false },
});
