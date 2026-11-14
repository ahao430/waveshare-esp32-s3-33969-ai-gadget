<script setup lang="ts">
// ZCode 远程页：远程编码会话源配置（设备 ZCode App，PLAN §5.7）。
// 未配置时设备端显示演示会话；配置后 Bridge 透传远端会话列表。
import { onMounted, ref } from 'vue'

import { request } from '@/api/client'

interface ZcodeCfg {
  base_url: string
  has_token: boolean
  updated_at?: string
}

const cfg = ref<ZcodeCfg>({ base_url: '', has_token: false })
const form = ref({ base_url: '', token: '' })
const message = ref('')
const messageErr = ref(false)
const busy = ref(false)

onMounted(async () => {
  try {
    cfg.value = await request<ZcodeCfg>('/api/v1/zcode/config')
    form.value.base_url = cfg.value.base_url
  } catch {
    /* 静默 */
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
    await request('/api/v1/zcode/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(form.value),
    })
    say(false, '已保存')
    cfg.value = await request<ZcodeCfg>('/api/v1/zcode/config')
  } catch (e) {
    say(true, `保存失败：${e instanceof Error ? e.message : e}`)
  } finally {
    busy.value = false
  }
}

async function clearCfg() {
  if (!confirm('清除 ZCode 远程配置？设备端将回退为演示会话。')) return
  await request('/api/v1/zcode/config', { method: 'DELETE' })
  say(false, '已清除')
  cfg.value = { base_url: '', has_token: false }
  form.value = { base_url: '', token: '' }
}
</script>

<template>
  <h1 class="page-title">ZCode 远程</h1>
  <p class="page-sub">
    设备端 ZCode App 的远程会话源：查看远端 Agent 会话列表（运行状态、
    等待确认、最近活动），为后续远程确认操作打基础（PLAN §5.7）。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="messageErr ? 'text-err' : 'text-ok'">{{ message }}</p>

  <div class="card max-w-xl space-y-3">
    <div class="flex items-center justify-between">
      <h3 class="font-semibold">会话源</h3>
      <span class="badge" :class="cfg.base_url ? '!bg-ok/15 !text-ok' : ''">
        {{ cfg.base_url ? (cfg.has_token ? '已配置 · 带 Token' : '已配置 · 无鉴权') : '未配置（演示会话）' }}
      </span>
    </div>
    <p v-if="cfg.updated_at" class="text-xs text-dim">
      更新于 {{ new Date(cfg.updated_at).toLocaleString('zh-CN', { hour12: false }) }}
    </p>
    <input v-model="form.base_url" class="input" placeholder="服务地址（如 http://10.0.0.5:8787）" />
    <input v-model="form.token" class="input" type="password" placeholder="API Token（可选；保存后不回显）" />
    <div class="flex gap-2">
      <button class="btn" :disabled="busy || !form.base_url" @click="save">保存</button>
      <button v-if="cfg.base_url" class="btn-ghost !text-err" @click="clearCfg">清除</button>
    </div>
  </div>

  <div class="card mt-4 max-w-xl text-xs text-dim">
    对接契约：<code>GET {地址}/api/v1/sessions</code>（Bearer Token），返回
    <code>{ "sessions": [{ id, agent, project, status, pending_confirm, completed, failed, last_active }] }</code>。
    Token 加密落库、明文不回显。未配置时 Bridge 返回演示会话（source=mock）。
  </div>
</template>
