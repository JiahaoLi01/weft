import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import path from 'path'

export default defineConfig({
  plugins: [tailwindcss(), react()],

  // 相对路径：Ultralight 通过 file:// 加载 UI/dist/index.html，绝对路径会解析失败。
  base: './',

  resolve: {
    alias: {
      '@': path.resolve(__dirname, 'src'),
    },
  },

  build: {
    outDir: 'dist',
    // Ultralight 的调试只能靠 Output Log，保留 sourcemap 让报错行号有意义。
    sourcemap: true,
  },

  server: {
    port: 3000,
  },
})
