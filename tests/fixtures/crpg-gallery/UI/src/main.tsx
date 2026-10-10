import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App'

// 启动金丝雀:带对象参数,既确认 JS 已启动,也验证 console → UE Output Log
// (LogUltralight [JS])的多参数序列化管道活着。每个 session 应能看到它一次。
console.log('[boot] WebUI up', { mode: import.meta.env.MODE, ts: new Date().toISOString() })

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
