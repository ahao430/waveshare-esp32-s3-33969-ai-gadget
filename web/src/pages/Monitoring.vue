<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'

import { connectEvents, type EventsConnection } from '@/api/ws'
import { fmtBps, fmtUptime, komariApi, type KomariView } from '@/api/komari'

const view = ref<KomariView | null>(null)
const loading = ref(false)
const error = ref('')
const live = ref(false)

let wsConn: EventsConnection | null = null

onMounted(async () => {
  await load()
  /* 实时通道：任何人触发 /komari/nodes 时 Bridge 都会广播新视图 */
  wsConn = connectEvents(
    (topic, data) => {
      if (topic === 'komari.updated') view.value = data as KomariView
    },
    (s) => (live.value = s === 'open'),
  )
})

onUnmounted(() => wsConn?.close())

async function load(force = false) {
  loading.value = true
  error.value = ''
  try {
    view.value = await komariApi.nodes(force)
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
  } finally {
    loading.value = false
  }
}

function pctClass(pct?: number): string {
  if (pct == null) return 'bg-cardhi'
  if (pct >= 85) return 'bg-err'
  if (pct >= 60) return 'bg-warn'
  return 'bg-accent2'
}

function fmtTime(iso?: string): string {
  if (!iso) return '—'
  return new Date(iso).toLocaleTimeString('zh-CN', { hour12: false })
}
</script>

<template>
  <div class="flex items-end justify-between">
    <div>
      <h1 class="page-title">监控</h1>
      <p class="page-sub">Komari 服务器状态（数据来自 Bridge 聚合，当前为 Mock 队列）</p>
    </div>
    <div class="flex items-center gap-3">
      <span v-if="live" class="badge !bg-ok/15 !text-ok">● 实时</span>
      <button class="btn" :disabled="loading" @click="load(true)">
        {{ loading ? '加载中…' : '刷新' }}
      </button>
    </div>
  </div>

  <p v-if="error" class="mb-4 text-sm text-err">加载失败：{{ error }}（请确认 Bridge 已启动）</p>
  <p v-if="view?.stale" class="mb-4 text-sm text-warn">
    ⚠ 上游异常（{{ view.last_error }}），以下为缓存数据
  </p>

  <div class="grid grid-cols-1 gap-4 md:grid-cols-2 xl:grid-cols-3">
    <div v-for="s in view?.snapshot?.servers ?? []" :key="s.uuid" class="card">
      <div class="mb-3 flex items-center justify-between">
        <div class="flex items-center gap-2 font-semibold">
          <span class="h-2.5 w-2.5 rounded-full" :class="s.online ? 'bg-ok' : 'bg-err'"></span>
          {{ s.name }}
        </div>
        <span class="badge">{{ s.online ? '在线' : '离线' }}</span>
      </div>

      <div v-for="row in ([
        ['CPU', s.cpu_pct], ['内存', s.mem_pct], ['磁盘', s.disk_pct],
      ] as const)" :key="row[0]" class="mb-2">
        <div class="mb-1 flex justify-between text-xs">
          <span class="text-dim">{{ row[0] }}</span>
          <span>{{ row[1] != null ? row[1].toFixed(0) + '%' : '—' }}</span>
        </div>
        <div class="h-1.5 overflow-hidden rounded bg-cardhi">
          <div
            class="h-full rounded transition-all duration-300"
            :class="pctClass(row[1])"
            :style="{ width: (row[1] ?? 0) + '%' }"
          ></div>
        </div>
      </div>

      <div class="mt-3 flex justify-between text-xs text-dim">
        <span>↑ {{ fmtBps(s.net_up_bps) }} · ↓ {{ fmtBps(s.net_down_bps) }}</span>
        <span>运行 {{ fmtUptime(s.uptime_s) }}</span>
      </div>
      <div class="mt-1 text-xs text-dim">上报 {{ fmtTime(s.last_report) }}</div>
    </div>
  </div>

  <p v-if="view && !view.snapshot" class="mt-6 text-center text-sm text-dim">暂无监控数据</p>
  <p v-if="view?.snapshot" class="mt-4 text-xs text-dim">
    数据源：{{ view.snapshot.source }} · 更新于 {{ fmtTime(view.snapshot.fetched_at) }} · Bridge 侧 30s 节流
  </p>
</template>
