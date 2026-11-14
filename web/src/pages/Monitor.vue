<script setup lang="ts">
// 服务器监控页：Komari 面板对接配置（设备"服务器"App 的数据源）。
import { onMounted, ref } from 'vue'

import { request } from '@/api/client'

interface MonitorCfg {
  base_url: string
  has_token: boolean
  updated_at?: string
}

const cfg = ref<MonitorCfg>({ base_url: '', has_token: false })
const form = ref({ base_url: '', token: '' })
const message = ref('')
const messageErr = ref(false)
const busy = ref(false)
const testMsg = ref('')
const testing = ref(false)

onMounted(async () => {
  try {
    cfg.value = await request<MonitorCfg>('/api/v1/monitor/config')
    form.value.base_url = cfg.value.base_url
  } catch {
    /* Bridge 未就绪时静默 */
  }
})

function say(err: boolean, text: string) {
  messageErr.value = err
  message.value = text
}

async function save() {
  if (!form.value.base_url) return
  busy.value = true
  try {
    await request('/api/v1/monitor/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(form.value),
    })
    say(false, '已保存（设备端下一次刷新生效）')
    cfg.value = await request<MonitorCfg>('/api/v1/monitor/config')
  } catch (e) {
    say(true, `保存失败：${e instanceof Error ? e.message : e}`)
  } finally {
    busy.value = false
  }
}

async function clearCfg() {
  if (!confirm('清除监控配置？将回退到环境变量 KOMARI_URL（未设则为演示数据）。')) return
  await request('/api/v1/monitor/config', { method: 'DELETE' })
  say(false, '已清除')
  cfg.value = { base_url: '', has_token: false }
  form.value = { base_url: '', token: '' }
}

/* 测试连接：保存后强制刷新一次节点列表 */
async function testConn() {
  testing.value = true
  testMsg.value = '测试中…'
  try {
    await save()
    const r = await request<{
      snapshot?: { servers?: { name: string; online: boolean }[]; source: string }
      stale?: boolean
      last_error?: string
    }>('/api/v1/komari/nodes?refresh=1')
    if (r.snapshot?.servers?.length) {
      const online = r.snapshot.servers.filter((s) => s.online).length
      testMsg.value = `✓ 连接成功：${r.snapshot.servers.length} 台服务器，${online} 台在线（${r.snapshot.source}）`
    } else if (r.snapshot?.source === 'none') {
      testMsg.value = '未配置监控源（保存地址后自动生效）'
    } else if (r.snapshot === null || r.snapshot === undefined) {
      testMsg.value = `✗ 抓取失败：${r.last_error ?? '未知错误'}`
    } else {
      testMsg.value = `✗ ${r.last_error ?? '未取到节点'}`
    }
  } catch (e) {
    testMsg.value = `✗ ${e instanceof Error ? e.message : e}`
  } finally {
    testing.value = false
  }
}
</script>

<template>
  <h1 class="page-title">服务器监控</h1>
  <p class="page-sub">
    对接自部署的 <a class="text-accent2" href="https://github.com/komari-monitor/komari" target="_blank">Komari</a>
    面板——设备端"服务器"App 展示节点列表与详情（在线/CPU/内存/网络/运行时间）。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="messageErr ? 'text-err' : 'text-ok'">{{ message }}</p>

  <div class="card max-w-xl space-y-3">
    <div class="flex items-center justify-between">
      <h3 class="font-semibold">Komari 数据源</h3>
      <span class="badge" :class="cfg.base_url ? '!bg-ok/15 !text-ok' : ''">
        {{ cfg.base_url ? (cfg.has_token ? '已配置 · 带 Token' : '已配置 · 无鉴权') : '未配置（演示数据）' }}
      </span>
    </div>
    <p v-if="cfg.updated_at" class="text-xs text-dim">
      更新于 {{ new Date(cfg.updated_at).toLocaleString('zh-CN', { hour12: false }) }}
    </p>
    <input v-model="form.base_url" class="input" placeholder="面板地址（https://komari.example.com）" />
    <input v-model="form.token" class="input" type="password" placeholder="访问 Token（面板未开鉴权可空；保存后不回显）" />
    <div class="flex gap-2">
      <button class="btn" :disabled="busy || !form.base_url" @click="save">保存</button>
      <button class="btn-ghost" :disabled="testing || !form.base_url" @click="testConn">测试连接</button>
      <button v-if="cfg.base_url" class="btn-ghost !text-err" @click="clearCfg">清除</button>
    </div>
    <p v-if="testMsg" class="text-xs" :class="testMsg.startsWith('✓') ? 'text-ok' : 'text-err'">{{ testMsg }}</p>
  </div>

  <div class="card mt-4 max-w-xl text-xs text-dim">
    说明：配置经 <code>GET {面板}/api/nodes</code> 抓取（开鉴权的部署填 Bearer Token）；
    Bridge 缓存 30 秒、失败保留旧数据。优先级：此配置 &gt; 环境变量
    <code>KOMARI_URL</code> &gt; 内置演示数据。Token 加密落库，明文不回显。
  </div>
</template>
