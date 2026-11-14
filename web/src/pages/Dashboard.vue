<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'

import { api, type PrewarmResult, type StoredSnapshot } from '@/api/client'
import { deviceApi } from '@/api/device'
import { connectEvents, type EventsConnection } from '@/api/ws'
import { useQuotaStore } from '@/stores/quota'

const store = useQuotaStore()
const refreshing = ref('')
const device = ref<Awaited<ReturnType<typeof deviceApi.status>>>()

/* 预热（PLAN §5.6）：手动触发 + 二次确认 + 前后对比展示 */
const prewarmConfirming = ref(false)
const prewarmBusy = ref(false)
const prewarmResult = ref<PrewarmResult | null>(null)
const prewarmError = ref('')

async function prewarm() {
  if (prewarmConfirming.value) {
    prewarmConfirming.value = false
    prewarmBusy.value = true
    prewarmError.value = ''
    try {
      prewarmResult.value = await api.prewarmZhipu(crypto.randomUUID())
      await store.loadSnapshot('zhipu', true)
    } catch (e) {
      prewarmError.value = e instanceof Error ? e.message : String(e)
    } finally {
      prewarmBusy.value = false
    }
  } else {
    prewarmConfirming.value = true // 第一次点击只进入确认态
    prewarmResult.value = null
  }
}

function pctOf(r: PrewarmResult, which: 'before' | 'after', win: string): string {
  const snap = which === 'before' ? r.before : r.after
  const w = snap?.windows.find((x) => x.id === win)
  return w?.used_percent?.toFixed(1) ?? '—'
}

const prewarmStatusText: Record<PrewarmResult['status'], string> = {
  executed: '已发出轻量请求',
  failed: '请求失败',
  skipped_low_quota: '已跳过：周额度保护',
  skipped_cooldown: '已跳过：冷却中',
  duplicate: '重复请求（幂等命中）',
}

onMounted(async () => {
  store.loadAll()
  device.value = await deviceApi.status()
  /* 实时通道：quota.<id> 推送直接应用（Bridge 侧刷新即推送） */
  wsConn = connectEvents((topic, data) => {
    if (topic.startsWith('quota.')) {
      store.applySnapshot(topic.slice('quota.'.length), data as StoredSnapshot)
    }
  })
})

onUnmounted(() => wsConn?.close())

async function refresh(id: string) {
  prewarmConfirming.value = false // 其他操作取消预热确认态
  refreshing.value = id
  try {
    await store.loadSnapshot(id, true)
  } finally {
    refreshing.value = ''
  }
}

function pctColor(pct: number): string {
  if (pct >= 85) return 'bg-err'
  if (pct >= 60) return 'bg-warn'
  return 'bg-accent2'
}

function fmtTime(iso?: string): string {
  if (!iso) return '—'
  return new Date(iso).toLocaleString('zh-CN', { hour12: false })
}

const bridgeToken = ref(localStorage.getItem('bridge_token') ?? '')
function saveToken() {
  if (bridgeToken.value) localStorage.setItem('bridge_token', bridgeToken.value)
  else localStorage.removeItem('bridge_token')
  location.reload() // Token 变更后重连 WS（连接 URL 携带旧 token）
}

let wsConn: EventsConnection | null = null
</script>

<template>
  <h1 class="page-title">仪表盘</h1>
  <p class="page-sub">设备总览与订阅额度（额度来自 Go Bridge，设备数据为 Mock）</p>

  <!-- UI 定稿概念图（产品风格基准，2026-11-14） -->
  <div class="card mb-5 overflow-hidden !p-0">
    <img src="/assistant-ui-final.png" alt="UI 定稿概念图" class="w-full" />
    <div class="border-t border-line px-4 py-2 text-xs text-dim">
      产品 UI 风格基准（2026-11-14 定稿）——三 AI 对话页 + 桌面九宫格
    </div>
  </div>

  <!-- 设备总览（Mock） -->
  <div v-if="device" class="card mb-5 flex flex-wrap items-center gap-x-8 gap-y-2 text-sm">
    <div>
      <div class="text-dim">设备</div>
      <div>{{ device.model }}</div>
    </div>
    <div>
      <div class="text-dim">Wi-Fi</div>
      <div>{{ device.wifi_ssid ?? '未连接' }} · {{ device.ip ?? '—' }}</div>
    </div>
    <div>
      <div class="text-dim">电池</div>
      <div>{{ device.battery_pct }}%</div>
    </div>
    <div>
      <div class="text-dim">运行</div>
      <div>{{ Math.floor(device.uptime_s / 3600) }}h {{ Math.floor((device.uptime_s % 3600) / 60) }}m</div>
    </div>
    <div class="ml-auto text-xs text-dim">fw {{ device.firmware }}</div>
  </div>

  <!-- Bridge 连接（Token 认证，PLAN §19） -->
  <div class="card mb-5 flex flex-wrap items-end gap-3">
    <div class="min-w-56 flex-1">
      <div class="mb-1 text-xs text-dim">Bridge Token（启用 BRIDGE_TOKEN 时填写；仅存本机浏览器）</div>
      <input v-model="bridgeToken" class="input" type="password" placeholder="留空 = 无认证" />
    </div>
    <button class="btn" @click="saveToken">保存</button>
  </div>

  <p v-if="store.error" class="mb-4 text-sm text-warn">
    加载失败：{{ store.error }}（确认 Bridge 已启动：bridge/run.sh；若启用 Token 请在上方填写）
  </p>

  <div class="grid grid-cols-1 gap-4 md:grid-cols-2 xl:grid-cols-3">
    <div v-for="p in store.providers" :key="p.id" class="card">
      <h3 class="mb-3 flex items-center justify-between gap-2 text-[15px] font-semibold">
        {{ p.display_name }}
        <span class="badge" :class="{ '!bg-warn/15 !text-warn': p.capabilities.experimental }">
          {{ p.capabilities.trust }} 级{{ p.capabilities.experimental ? ' · 实验性' : '' }}
        </span>
      </h3>

      <template v-if="store.snapshots[p.id]?.snapshot">
        <div v-for="w in store.snapshots[p.id].snapshot?.windows ?? []" :key="w.id" class="mb-3">
          <div class="mb-1.5 flex items-baseline justify-between text-[13px]">
            <span>{{ w.label }}</span>
            <span class="text-xs text-dim" v-if="w.reset_at">重置 {{ fmtTime(w.reset_at) }}</span>
          </div>
          <div class="mb-1 h-2 overflow-hidden rounded bg-cardhi">
            <div
              class="h-full rounded transition-all duration-300"
              :class="pctColor(w.used_percent ?? 0)"
              :style="{ width: (w.used_percent ?? 0) + '%' }"
            ></div>
          </div>
          <div class="text-xs text-dim">
            已用 {{ w.used_percent?.toFixed(1) ?? '—' }}% · 剩余 {{ w.remaining_pct?.toFixed(1) ?? '—' }}%
          </div>
        </div>
        <div class="mt-3 flex items-center justify-between">
          <span class="text-xs text-dim">更新于 {{ fmtTime(store.snapshots[p.id].fetched_at) }}</span>
          <div class="flex gap-2">
            <!-- 预热：仅智谱；二次确认后才真正发起（PLAN §5.6） -->
            <button
              v-if="p.id === 'zhipu'"
              class="btn-ghost !px-3 !py-1.5 !text-[13px]"
              :class="{ '!border-warn !text-warn': prewarmConfirming }"
              :disabled="prewarmBusy"
              @click="prewarm"
            >
              {{
                prewarmBusy ? '预热中…'
                : prewarmConfirming ? '确认发送？（模型 1 token 请求）'
                : '预热'
              }}
            </button>
            <button class="btn !px-3 !py-1.5 !text-[13px]" :disabled="refreshing === p.id" @click="refresh(p.id)">
              {{ refreshing === p.id ? '刷新中…' : '刷新' }}
            </button>
          </div>
        </div>

        <!-- 预热结果：前后额度对比（不承诺重置窗口） -->
        <div v-if="prewarmResult" class="mt-3 rounded-xl border border-line bg-bg p-3 text-xs">
          <div :class="prewarmResult.status === 'executed' ? 'text-ok' : 'text-warn'">
            {{ prewarmStatusText[prewarmResult.status] }}
            <span v-if="prewarmResult.message" class="text-dim">（{{ prewarmResult.message }}）</span>
          </div>
          <div class="mt-1.5 grid grid-cols-3 gap-1 text-dim">
            <span>5小时：{{ pctOf(prewarmResult, 'before', '5h') }}% → {{ pctOf(prewarmResult, 'after', '5h') }}%</span>
            <span>本周：{{ pctOf(prewarmResult, 'before', 'weekly') }}% → {{ pctOf(prewarmResult, 'after', 'weekly') }}%</span>
            <span v-if="prewarmResult.duration_ms != null">耗时 {{ prewarmResult.duration_ms }}ms</span>
          </div>
          <div class="mt-1 text-dim">预热只发起一次轻量请求并记录前后变化，不代表重置 5 小时窗口。</div>
        </div>
        <p v-if="prewarmError" class="mt-2 text-xs text-err">预热失败：{{ prewarmError }}</p>
      </template>
      <p v-else class="text-sm text-dim">暂无快照数据</p>

      <p v-if="store.snapshots[p.id]?.stale" class="mt-2 text-xs text-warn">
        ⚠ 最近一次刷新失败（{{ store.snapshots[p.id]?.last_error }}），以上为缓存数据
      </p>
    </div>
  </div>
</template>
