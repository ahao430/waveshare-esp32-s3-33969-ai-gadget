<script setup lang="ts">
import { RouterLink, RouterView } from 'vue-router'

const nav = [
  { to: '/', label: '仪表盘' },
  { to: '/subscriptions', label: '订阅额度' },
  { to: '/chat', label: '聊天' },
  { to: '/monitoring', label: '监控' },
  { to: '/network', label: '网络' },
  { to: '/provisioning', label: '配网' },
]
</script>

<template>
  <!-- 手机 <640px：顶部横向导航；≥640px：侧边栏（PLAN §9.1 断点） -->
  <div class="flex min-h-screen flex-col sm:flex-row">
    <aside
      class="flex shrink-0 flex-row items-center gap-3 border-b border-line bg-side px-4 py-2.5 sm:w-56 sm:flex-col sm:gap-6 sm:border-b-0 sm:border-r sm:px-3.5 sm:py-5"
    >
      <div class="flex items-center gap-2 text-[17px] font-bold sm:px-2">
        <span class="h-2.5 w-2.5 shrink-0 rounded-full bg-gradient-to-br from-accent to-accent2"></span>
        AI Gadget
      </div>
      <nav class="flex flex-row gap-1 overflow-x-auto sm:flex-col">
        <RouterLink
          v-for="item in nav"
          :key="item.to"
          :to="item.to"
          class="whitespace-nowrap rounded-xl px-3 py-2 text-sm text-dim transition-colors hover:bg-card hover:text-txt"
          active-class="!bg-card !font-semibold !text-txt"
        >
          {{ item.label }}
        </RouterLink>
      </nav>
      <div class="ml-auto hidden px-2 text-xs text-dim sm:mt-auto sm:block">
        Phase 2 · 设备 API 为 Mock
      </div>
    </aside>

    <main class="min-w-0 flex-1 p-4 sm:p-6">
      <RouterView />
    </main>
  </div>
</template>
