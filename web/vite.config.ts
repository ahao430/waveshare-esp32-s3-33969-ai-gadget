import { fileURLToPath, URL } from 'node:url'

import vue from '@vitejs/plugin-vue'
import { defineConfig } from 'vite'

// Bridge 地址；局域网设备访问时由 Vite 服务端代理到本机 Bridge
const proxy = {
  '/api': {
    target: process.env.VITE_BRIDGE_URL ?? 'http://127.0.0.1:8081',
    changeOrigin: true,
    ws: true, // /api/v1/events WebSocket 升级透传
  },
}

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  server: {
    // 0.0.0.0：手机/另一台电脑可用 http://<Mac局域网IP>:5173 打开后台
    // （仅开发服务器；只在可信局域网使用，见 README「局域网访问」）
    host: true,
    port: 5173,
    proxy,
  },
  preview: {
    host: true,
    port: 4173,
    proxy,
  },
})
