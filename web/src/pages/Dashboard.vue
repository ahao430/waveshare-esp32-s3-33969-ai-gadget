<script setup lang="ts">
// 配置总览：公共配置 + 各应用配置入口（Web=配置后台，不做查看）。
import { onMounted, ref } from 'vue'
import { RouterLink } from 'vue-router'

import { request } from '@/api/client'

const bridgeOk = ref<boolean | null>(null)
const themeName = ref('')

const adminUrl = typeof window !== 'undefined' ? window.location.origin : ''

const themes = ['星云紫', '极光青', '落日橙', '蔷薇粉']

onMounted(async () => {
  try {
    await request('/api/v1/device/status')
    bridgeOk.value = true
  } catch {
    bridgeOk.value = false
  }
  try {
    const r = await request<{ theme: number }>('/api/v1/appearance/theme')
    themeName.value = themes[r.theme] ?? ''
  } catch {
    themeName.value = ''
  }
})

const groups = [
  {
    title: '公共配置',
    items: [
      { to: '/appearance', label: '外观', desc: '主题色、自定义背景、主页排布（一屏 4 个自动分页）' },
      { to: '/network', label: '网络与配网', desc: 'Wi-Fi 管理、首次配网流程、后台二维码' },
      { to: '/proxy', label: '代理', desc: '出站 HTTP 代理（Direct / HTTP CONNECT）' },
      { to: '/keys', label: '物理按键', desc: '三键短按/长按映射（长按电源/BOOT 固定）' },
    ],
  },
  {
    title: '应用配置',
    items: [
      { to: '/subscriptions', label: '订阅中心', desc: '额度订阅实例（智谱/Kimi/DeepSeek/Coding Plan/NewAPI）' },
      { to: '/bots', label: 'AstrBot', desc: '机器人实例与连接（设备端对话对接，可多个）' },
      { to: '/voice', label: '语音设置', desc: '语音识别 Key（可跳转官方控制台获取）与各 App 音色' },
      { to: '/muse', label: 'Muse', desc: '云端连接凭据（配对导出）与连通性测试' },
      { to: '/xiaozhi', label: '小智', desc: '官方服务未接入（规划中）；当前走 Bridge 语音链路，可配台湾腔音色' },
      { to: '/zcode', label: 'ZCode 远程', desc: '远程编码会话源（状态/待确认数量）' },
      { to: '/monitor', label: '服务器监控', desc: 'Komari 面板对接（节点列表与详情数据源）' },
      { to: '/weather', label: '天气', desc: 'Open-Meteo 免 Key；自动 IP 定位或手动城市' },
    ],
  },
]
</script>

<template>
  <section class="space-y-6">
    <header>
      <h1 class="text-xl font-semibold">配置总览</h1>
      <p class="mt-1 text-sm text-dim">
        Web 是设备的配置后台——公共配置与各应用配置在这里完成；数据查看
        （额度/监控/对话）在设备端体验。
        Bridge 状态：
        <span :class="bridgeOk ? 'text-ok' : 'text-err'">
          {{ bridgeOk === null ? '检查中…' : bridgeOk ? '已连接' : '未连接（:8081）' }}
        </span>
        <template v-if="themeName"> · 当前主题：{{ themeName }}</template>
      </p>
    </header>

    <div class="card">
      <h3 class="mb-2 font-semibold">使用与访问</h3>
      <ul class="space-y-1 text-sm text-dim">
        <li>当前后台：<span class="text-accent2">{{ adminUrl }}</span></li>
        <li>
          局域网：手机/另一台电脑打开
          <code>http://&lt;Mac 局域网 IP&gt;:5173</code>（Vite 已监听 0.0.0.0；
          首次可能需要在 macOS 防火墙里放行 node）。
        </li>
        <li>
          设备：刷好固件后走 Wi-Fi；接了电池即可拔掉 USB 独立运行。
          AI 回复依赖本机 Bridge 常驻，且 Mac 不能休眠。
        </li>
        <li>
          安全：仅限可信局域网；若设置了 <code>BRIDGE_TOKEN</code>，浏览器需在
          localStorage 写入 <code>bridge_token</code>。
        </li>
      </ul>
    </div>

    <div v-for="g in groups" :key="g.title" class="space-y-3">
      <h2 class="text-sm font-medium text-dim">{{ g.title }}</h2>
      <div class="grid grid-cols-1 gap-4 md:grid-cols-2">
        <RouterLink
          v-for="it in g.items"
          :key="it.label"
          :to="it.to"
          class="card block transition hover:border-accent"
        >
          <h3 class="font-semibold">{{ it.label }}</h3>
          <p class="mt-1 text-sm text-dim">{{ it.desc }}</p>
        </RouterLink>
      </div>
    </div>

    <div class="card text-xs text-dim">
      说明：机器人（AstrBot）、ZCode、监控（Komari）与按键映射均由 Bridge
      加密落库并即时生效；设备端开机联网后拉取最新配置。
    </div>
  </section>
</template>
