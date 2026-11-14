<script setup lang="ts">
// 配置总览：公共配置 + 各应用配置入口（Web=配置后台，不做查看）。
import { onMounted, ref } from 'vue'
import { RouterLink } from 'vue-router'

import { request } from '@/api/client'

const bridgeOk = ref<boolean | null>(null)
const themeName = ref('')

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
      { to: '/appearance', label: '外观', desc: '主题色下发、自定义背景图' },
      { to: '/network', label: '网络与配网', desc: 'Wi-Fi 配置方式说明、设备热点' },
      { to: '/provisioning', label: '后台二维码', desc: '本后台地址全屏码（设备扫码直达）' },
    ],
  },
  {
    title: '应用配置',
    items: [
      { to: '/subscriptions', label: '订阅中心', desc: '订阅实例增删（智谱/Kimi/OpenCode/NewAPI/Mock）' },
      { to: '/subscriptions', label: '聊天机器人', desc: 'AstrBot 实例配置（大肥鱼对话对接）' },
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
      说明：订阅实例中的「聊天机器人」即 AstrBot 实例（大肥鱼对话与语音
      经 Bridge 路由到对应机器人）；添加/删除即时生效，无需重启 Bridge。
    </div>
  </section>
</template>
