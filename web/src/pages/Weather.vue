<script setup lang="ts">
// 天气页：定位方式 + 自动刷新间隔配置。数据源 Open-Meteo 免费无需 Key；
// 自动刷新可关闭（0=关闭，Bridge 仅请求时按需刷新）。
import { onMounted, ref } from 'vue'

import { request } from '@/api/client'

const mode = ref<'auto' | 'manual'>('auto')
const city = ref('')
const location = ref('') // 当前实际定位（缓存）
const message = ref('')
const messageErr = ref(false)
const busy = ref(false)
const preview = ref('')
// 自动刷新间隔（分钟；0=关闭）
const autoRefresh = ref(10)
const refreshOptions = [
  { v: 0, label: '关闭（按需刷新）' },
  { v: 10, label: '10 分钟' },
  { v: 30, label: '30 分钟' },
  { v: 60, label: '60 分钟' },
  { v: 120, label: '120 分钟' },
]

interface WeatherView {
  snapshot?: {
    location: string
    current: { temp_c: number; desc: string; humidity: number; wind_kmh: number }
  }
  stale?: boolean
  last_error?: string
}

onMounted(async () => {
  try {
    const c = await request<{ mode: string; city?: string; location?: string; auto_refresh_min?: number }>(
      '/api/v1/weather/config',
    )
    mode.value = c.mode === 'manual' ? 'manual' : 'auto'
    city.value = c.city ?? ''
    location.value = c.location ?? ''
    autoRefresh.value = c.auto_refresh_min ?? 10
  } catch {
    /* 静默 */
  }
  loadPreview()
})

async function loadPreview() {
  try {
    const v = await request<WeatherView>('/api/v1/weather')
    if (v.snapshot) {
      const c = v.snapshot.current
      preview.value = `${v.snapshot.location} · ${c.desc} ${c.temp_c}°C（湿度 ${c.humidity}% 风 ${c.wind_kmh}km/h）`
    } else if (v.last_error) {
      preview.value = `暂无数据：${v.last_error}`
    }
  } catch {
    preview.value = ''
  }
}

async function save() {
  if (mode.value === 'manual' && !city.value.trim()) return
  busy.value = true
  try {
    await request('/api/v1/weather/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        mode: mode.value,
        city: city.value.trim(),
        auto_refresh_min: autoRefresh.value,
      }),
    })
    messageErr.value = false
    message.value = '已保存（Bridge 正按新配置刷新…）'
    setTimeout(loadPreview, 1500)
  } catch (e) {
    messageErr.value = true
    message.value = `保存失败：${e instanceof Error ? e.message : e}`
  } finally {
    busy.value = false
  }
}

async function refreshNow() {
  busy.value = true
  try {
    await request('/api/v1/weather?refresh=1')
    await loadPreview()
    messageErr.value = false
    message.value = '已刷新'
  } catch (e) {
    messageErr.value = true
    message.value = `刷新失败：${e instanceof Error ? e.message : e}`
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <h1 class="page-title">天气</h1>
  <p class="page-sub">
    数据源 <a class="text-accent2" href="https://open-meteo.com" target="_blank">Open-Meteo</a>
    ——免费、无需注册 API Key。Bridge 按下方间隔自动刷新（默认 10 分钟），设备端即开即看。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="messageErr ? 'text-err' : 'text-ok'">{{ message }}</p>

  <div class="card max-w-xl space-y-4">
    <h3 class="font-semibold">定位方式</h3>
    <label class="flex cursor-pointer items-center gap-2 text-sm">
      <input v-model="mode" type="radio" value="auto" class="accent-[var(--accent)]" />
      <span>自动（按 Bridge 出口 IP 定位，适合家里/办公室固定网络）</span>
    </label>
    <label class="flex cursor-pointer items-center gap-2 text-sm">
      <input v-model="mode" type="radio" value="manual" class="accent-[var(--accent)]" />
      <span>手动指定城市</span>
    </label>
    <div v-if="mode === 'manual'" class="pl-6">
      <input v-model="city" class="input" placeholder="城市名（如：上海 / 杭州 / Shenzhen）" />
      <p class="mt-1 text-xs text-dim">支持中文/拼音/英文，经 Open-Meteo 地理编码解析。</p>
    </div>
    <div>
      <h3 class="mb-2 font-semibold">自动刷新</h3>
      <select v-model.number="autoRefresh" class="input !w-56">
        <option v-for="o in refreshOptions" :key="o.v" :value="o.v">{{ o.label }}</option>
      </select>
      <p class="mt-1 text-xs text-dim">
        关闭后 Bridge 不后台保温，设备/页面请求时按需刷新（有缓存仍先回缓存）。
      </p>
    </div>
    <div class="flex gap-2">
      <button class="btn" :disabled="busy || (mode === 'manual' && !city.trim())" @click="save">保存</button>
      <button class="btn-ghost" :disabled="busy" @click="refreshNow">立即刷新</button>
    </div>
    <p v-if="location || preview" class="text-xs text-dim">
      当前定位：<span class="text-txt">{{ location || '（待刷新）' }}</span>
      <template v-if="preview"> · {{ preview }}</template>
    </p>
  </div>

  <div class="card mt-4 max-w-xl text-xs text-dim">
    说明：自动模式定位的是 Bridge 所在网络的位置（设备不直连外网）；
    服务器部署或使用代理时建议切换手动城市。后续如需接入和风天气等
    需要 Key 的服务，可在此页扩展 Provider 配置。
  </div>
</template>
