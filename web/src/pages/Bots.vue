<script setup lang="ts">
// 聊天机器人页：大肥鱼对话上游 = AstrBot 实例（后台可创建多个）。
// Token（abk_）AES-256-GCM 加密落库，只进不回；增删即时生效。
import { onMounted, ref } from 'vue'

import { request } from '@/api/client'

interface BotInfo {
  id: string
  label: string
  base_url: string
  has_token: boolean
  updated_at: string
}

const bots = ref<BotInfo[]>([])
const message = ref('')
const messageErr = ref(false)
const busy = ref(false)

const form = ref({ label: '', base_url: '', token: '' })
const probeMsg = ref('')
const probingId = ref('')

onMounted(load)

async function load() {
  try {
    const r = await request<{ bots: BotInfo[] }>('/api/v1/bots')
    bots.value = r.bots
  } catch (e) {
    message.value = `机器人列表不可用：${e instanceof Error ? e.message : e}`
    messageErr.value = true
  }
}

function say(err: boolean, text: string) {
  messageErr.value = err
  message.value = text
}

async function addBot() {
  if (!form.value.label || !form.value.base_url) return
  busy.value = true
  try {
    await request('/api/v1/bots', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(form.value),
    })
    say(false, `已创建机器人「${form.value.label}」（对话即时生效）`)
    form.value = { label: '', base_url: '', token: '' }
    await load()
  } catch (e) {
    say(true, `创建失败：${e instanceof Error ? e.message : e}`)
  } finally {
    busy.value = false
  }
}

async function removeBot(b: BotInfo) {
  if (!confirm(`删除机器人「${b.label}」？大肥鱼将不再路由到它。`)) return
  await request(`/api/v1/bots/${b.id}`, { method: 'DELETE' })
  say(false, `已删除「${b.label}」`)
  await load()
}

/* 测试连通性：对填写的地址/Token（或已保存机器人）发一条 ping */
async function probe(id?: string) {
  probingId.value = id ?? '__new'
  probeMsg.value = '测试中…'
  try {
    const body = id
      ? { id }
      : { base_url: form.value.base_url, token: form.value.token }
    const r = await request<{ ok: boolean; reply?: string; error?: string }>(
      '/api/v1/bots/probe',
      { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) },
    )
    probeMsg.value = r.ok ? `✓ 连通正常，回复：${r.reply}` : `✗ ${r.error}`
  } catch (e) {
    probeMsg.value = `✗ ${e instanceof Error ? e.message : e}`
  } finally {
    probingId.value = ''
  }
}
</script>

<template>
  <h1 class="page-title">聊天机器人</h1>
  <p class="page-sub">
    大肥鱼对话与语音的上游 = AstrBot 实例（可创建多个；设备端长按列表切换）。
    在服务器部署 <a class="text-accent2" href="https://github.com/AstrBotDevs/AstrBot" target="_blank">AstrBot</a>
    后，开启 API 并生成 <code class="text-accent2">abk_</code> Token 填入这里。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="messageErr ? 'text-err' : 'text-ok'">{{ message }}</p>

  <div class="card">
    <h3 class="mb-1 font-semibold">机器人实例</h3>
    <p class="mb-3 text-xs text-dim">对话请求经 Bridge 路由到这些实例；每个机器人的会话（session）相互隔离。未配置任何机器人时，大肥鱼走默认 Provider。</p>
    <div v-if="bots.length === 0" class="mb-2 text-sm text-dim">还没有机器人，添加一个：</div>
    <div class="space-y-2">
      <div v-for="b in bots" :key="b.id" class="flex items-center justify-between rounded border border-line px-3 py-2">
        <div class="min-w-0">
          <span class="font-medium">{{ b.label }}</span>
          <span class="ml-2 truncate text-xs text-dim">{{ b.base_url }}</span>
          <span class="ml-2 badge" :class="b.has_token ? '' : '!text-warn'">{{ b.has_token ? '已配 Token' : '无鉴权' }}</span>
        </div>
        <div class="flex shrink-0 gap-1">
          <button class="btn-ghost !px-2 !py-1" :disabled="probingId !== ''" @click="probe(b.id)">测试</button>
          <button class="btn-ghost !text-err !px-2 !py-1" @click="removeBot(b)">删除</button>
        </div>
      </div>
    </div>

    <div class="mt-3 space-y-2">
      <div class="flex flex-wrap items-end gap-2">
        <input v-model="form.label" class="input !w-40" placeholder="名称（如：客厅机器人）" />
        <input v-model="form.base_url" class="input !w-64" placeholder="AstrBot 地址（http://ip:6185）" />
        <input v-model="form.token" class="input !w-48" type="password" placeholder="abk_ Token（未开鉴权可空）" />
        <button class="btn" :disabled="busy || !form.label || !form.base_url" @click="addBot">创建机器人</button>
        <button class="btn-ghost" :disabled="!form.base_url || probingId !== ''" @click="probe()">先测一下</button>
      </div>
      <p v-if="probeMsg" class="text-xs" :class="probeMsg.startsWith('✓') ? 'text-ok' : 'text-err'">{{ probeMsg }}</p>
    </div>
  </div>

  <div class="card mt-4 text-xs text-dim">
    说明：Token 经 AES-256-GCM 加密落库（需 <code>BRIDGE_MASTER_KEY</code>），明文不回显、不入日志。
    Bridge 按 <code>POST {地址}/api/v1/chat</code> 对接（username=gadget、session 按机器人隔离）；
    添加/删除即时生效，无需重启。角色人设、模型与插件都在 AstrBot 侧配置。
  </div>
</template>
