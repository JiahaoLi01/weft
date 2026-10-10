import { defineConfig } from 'vite';
import { resolve } from 'node:path';

export default defineConfig(({ mode }) => ({
  define: { 'process.env.NODE_ENV': JSON.stringify('production') },
  build: mode === 'native' ? {
    outDir: 'dist/native',
    target: 'es2020',
    minify: false,
    lib: { entry: resolve(import.meta.dirname, 'src/main.jsx'), name: 'CaseElephantBundle', formats: ['iife'], fileName: () => 'app.js' },
  } : {
    outDir: 'dist/browser',
    target: 'es2020',
    modulePreload: false,
    minify: false,
  },
}));
