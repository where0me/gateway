import { defineConfig, loadEnv } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig(({ mode }) => {
  // 加载当前根目录下的 .env 环境变量
  const env = loadEnv(mode, process.cwd(), '')

  return {
    plugins: [react()],
    server: {
      proxy: {
        '/api': {
          // 核心：直接读取 .env 里的后端地址，告别“写死”代码！
          target: env.VITE_API_TARGET || 'http://127.0.0.1:3000',
          changeOrigin: true,
        }
      }
    }
  }
})