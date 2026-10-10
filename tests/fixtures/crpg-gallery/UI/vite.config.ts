import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import path from 'path'

export default defineConfig({
  plugins: [tailwindcss(), react()],

  // Relative paths so file:/// protocol works in CEF
  base: './',

  resolve: {
    alias: {
      '@': path.resolve(__dirname, 'src'),
    },
  },

  build: {
    outDir: 'dist',
    chunkSizeWarningLimit: 600,
    rollupOptions: {
      output: {
        manualChunks: {
          'd3': ['d3'],
          'three': ['three'],
          'framer': ['framer-motion'],
          'spring': ['@react-spring/web'],
        },
      },
    },
  },

  server: {
    port: 3000,
  },
})
