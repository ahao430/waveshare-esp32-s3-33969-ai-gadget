<script setup lang="ts">
// 小智官方服务接入（ADR-005：Bridge 模拟设备）。
// 流程：连接（OTA）→ 未绑定显示 6 位激活码 → 用户在 xiaozhi.me 控制台
// 绑定 → 「我已绑定」轮询 → 已连接后设备小智 App 走官方链路（人设/
// 音色在官方控制台配），失败自动回落本机链路。
import { onMounted, onUnmounted, ref } from 'vue'

import { request } from '@/api/client'

interface Status {
  configured: boolean
  bound: boolean
  ready: boolean
  device_id: string
  ota_url: string
  last_error: string
  ws_host: string
}

const status = ref<Status | null>(null)
const busy = ref(false)
const code = ref('')
const message = ref('')
const messageErr = ref(false)
let pollTimer: number | undefined

async function load() {
  try {
    status.value = await request<Status>('/api/v1/xiaozhi/status')
  } catch {
    status.value = null
  }
}

async function connect() {
  busy.value = true
  message.value = '连接中…'
  messageErr.value = false
  try {
    const r = await request<{ bound: boolean; activation_code?: string; message?: string }>(
      '/api/v1/xiaozhi/connect',
      { method: 'POST' },
    )
    code.value = r.activation_code ?? ''
    message.value = r.bound ? '✓ 已连接官方服务' : r.message ?? '未绑定'
    messageErr.value = !r.bound
    await load()
    if (!r.bound && code.value) startPolling()
  } catch (e) {
    message.value = `✗ ${e instanceof Error ? e.message : e}`
    messageErr.value = true
  } finally {
    busy.value = false
  }
}

/* 绑定轮询：3s 一次，绑定成功即停（Bridge 内部会 re-OTA 刷新 token） */
function startPolling() {
  stopPolling()
  pollTimer = window.setInterval(async () => {
    try {
      const r = await request<{ bound: boolean }>('/api/v1/xiaozhi/activate', { method: 'POST' })
      if (r.bound) {
        stopPolling()
        code.value = ''
        message.value = '✓ 绑定成功，小智 App 已走官方服务'
        messageErr.value = false
        await load()
      }
    } catch {
      /* 轮询失败不打扰 */
    }
  }, 3000)
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = undefined
  }
}

async function clearConfig() {
  if (!confirm('删除小智官方服务身份？（设备小智 App 回落本机链路；重新连接会生成新设备）')) return
  stopPolling()
  busy.value = true
  try {
    await request('/api/v1/xiaozhi/config', { method: 'DELETE' })
    code.value = ''
    message.value = '已删除'
    messageErr.value = false
    await load()
  } finally {
    busy.value = false
  }
}

onMounted(load)
onUnmounted(stopPolling)
</script>

<template>
  <h1 class="page-title">小智</h1>
  <p class="page-sub">
    官方 xiaozhi.me 服务：Bridge 模拟一台小智设备，设备端小智 App 的识别、
    回复与音色全部来自官方（人设/音色在官方控制台配置）。未连接或失败时
    自动回落本机语音链路（语音设置页的音色在回落时生效）。
  </p>

  <div class="card">
    <div class="mb-3 flex flex-wrap items-center justify-between gap-2">
      <h3 class="font-semibold">官方服务</h3>
      <span class="badge" :class="status?.ready ? '!bg-ok/15 !text-ok' : status?.configured ? '!text-warn' : ''">
        {{
          status?.ready
            ? `已连接${status.ws_host ? '（' + status.ws_host + '）' : ''}`
            : status?.configured
              ? status.bound
                ? '已绑定（连接异常）'
                : '待绑定'
              : '未连接（走本机链路）'
        }}
      </span>
    </div>

    <!-- 激活码展示：大号等宽 + 步骤说明 -->
    <div v-if="code" class="mb-4 rounded border border-accent/40 bg-accent/10 p-4 text-center">
      <p class="text-xs text-dim">请在 xiaozhi.me 控制台「设备管理 → 添加设备」输入激活码</p>
      <p class="my-2 font-mono text-4xl font-bold tracking-[0.4em] text-accent">{{ code }}</p>
      <p class="text-xs text-dim">绑定后本页自动完成（无需刷新）</p>
    </div>

    <ol class="mb-4 list-decimal space-y-1 pl-5 text-sm text-dim">
      <li>点「连接官方服务」获取激活码（首次会自动注册设备身份）。</li>
      <li>打开 <a class="text-accent2" href="https://xiaozhi.me/console" target="_blank" rel="noopener">xiaozhi.me 控制台</a>，设备管理里输入激活码。</li>
      <li>绑定后这里自动变为「已连接」，拿设备小智 App 按住说话试试。</li>
    </ol>

    <div class="flex flex-wrap items-center gap-3">
      <button class="btn" :disabled="busy" @click="connect">
        {{ status?.ready ? '重新检测' : '连接官方服务' }}
      </button>
      <button v-if="code" class="btn-ghost" :disabled="busy" @click="startPolling">我已输入激活码</button>
      <a class="btn-ghost" href="https://xiaozhi.me/console" target="_blank" rel="noopener">打开控制台 ↗</a>
      <button v-if="status?.configured" class="btn-ghost !text-err" :disabled="busy" @click="clearConfig">
        删除设备身份
      </button>
    </div>
    <p v-if="message" class="mt-2 text-xs" :class="messageErr ? 'text-err' : 'text-ok'">{{ message }}</p>
    <p v-if="status?.last_error" class="mt-1 text-xs text-warn">最近错误：{{ status.last_error }}</p>
    <p v-if="status?.device_id" class="mt-1 text-xs text-dim">
      设备身份 {{ status.device_id }}<template v-if="status.ota_url"> · 端点 {{ status.ota_url }}</template>
    </p>
  </div>

  <div class="card mt-4 text-xs text-dim">
    说明：设备身份与凭据经 AES-256-GCM 加密落库；一个身份对应官方控制台里的
    一台设备。免费额度以官方为准；官方服务不可用时设备自动回落本机
    ASR/LLM/TTS，不会变哑巴。
  </div>
</template>
