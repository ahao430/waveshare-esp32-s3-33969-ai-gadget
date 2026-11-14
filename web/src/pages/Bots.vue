<script setup lang="ts">
// 聊天机器人页：大肥鱼对话上游 = AstrBot 实例（可多个）。
// Token 加密落库只进不回；支持选择 AstrBot 配置文件（人设/模型套餐）
// 与 Provider/Model 覆盖；增删改即时生效。
import { onMounted, ref } from 'vue'

import { request } from '@/api/client'

interface BotInfo {
  id: string
  label: string
  base_url: string
  has_token: boolean
  config_id?: string
  provider?: string
  model?: string
  updated_at: string
}
interface AstrBotConfig {
  id: string
  name: string
  is_default: boolean
}

const bots = ref<BotInfo[]>([])
const message = ref('')
const messageErr = ref(false)
const busy = ref(false)

const form = ref({
  id: '', // 空=新建；非空=编辑该机器人（upsert）
  label: '',
  base_url: '',
  token: '',
  config_id: '',
  provider: '',
  model: '',
})
const configs = ref<AstrBotConfig[]>([])
const probeMsg = ref('')
const probingId = ref('')
const cfgMsg = ref('')
const fetchingCfg = ref(false)

const configName = (id?: string) =>
  id ? configs.value.find((c) => c.id === id)?.name ?? id : '默认'

onMounted(load)

async function load() {
  try {
    const r = await request<{ bots: BotInfo[] }>('/api/v1/bots')
    bots.value = r.bots
  } catch (e) {
    messageErr.value = true
    message.value = `机器人列表不可用：${e instanceof Error ? e.message : e}`
  }
}

function say(err: boolean, text: string) {
  messageErr.value = err
  message.value = text
}

function resetForm() {
  form.value = {
    id: '',
    label: '',
    base_url: '',
    token: '',
    config_id: '',
    provider: '',
    model: '',
  }
  configs.value = []
  cfgMsg.value = ''
}

async function addBot() {
  if (!form.value.label || !form.value.base_url) return
  busy.value = true
  try {
    const body = { ...form.value }
    if (body.id === '') delete (body as Record<string, unknown>).id
    if (body.token === '' && form.value.id !== '') delete (body as Record<string, unknown>).token // 编辑未填 token=不覆盖
    await request('/api/v1/bots', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    })
    say(false, form.value.id ? `已更新「${form.value.label}」` : `已创建机器人「${form.value.label}」（对话即时生效）`)
    resetForm()
    await load()
  } catch (e) {
    say(true, `保存失败：${e instanceof Error ? e.message : e}`)
  } finally {
    busy.value = false
  }
}

function editBot(b: BotInfo) {
  form.value = {
    id: b.id,
    label: b.label,
    base_url: b.base_url,
    token: '',
    config_id: b.config_id ?? '',
    provider: b.provider ?? '',
    model: b.model ?? '',
  }
  fetchConfigs()
  window.scrollTo({ top: 0, behavior: 'smooth' })
}

async function removeBot(b: BotInfo) {
  if (!confirm(`删除机器人「${b.label}」？大肥鱼将不再路由到它。`)) return
  await request(`/api/v1/bots/${b.id}`, { method: 'DELETE' })
  say(false, `已删除「${b.label}」`)
  await load()
}

/* 拉取 AstrBot 配置文件列表（人设/模型套餐；Key 需 config 权限） */
async function fetchConfigs() {
  if (!form.value.base_url && !form.value.id) return
  fetchingCfg.value = true
  cfgMsg.value = '拉取中…'
  try {
    const body = form.value.id
      ? { id: form.value.id }
      : { base_url: form.value.base_url, token: form.value.token }
    const r = await request<{ ok: boolean; configs?: AstrBotConfig[]; error?: string }>(
      '/api/v1/bots/configs',
      { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) },
    )
    if (r.ok && r.configs) {
      configs.value = r.configs
      cfgMsg.value = `✓ ${r.configs.length} 个配置文件`
    } else {
      configs.value = []
      cfgMsg.value = `✗ ${r.error ?? '拉取失败'}`
    }
  } catch (e) {
    cfgMsg.value = `✗ ${e instanceof Error ? e.message : e}`
  } finally {
    fetchingCfg.value = false
  }
}

/* 连通性测试（携带已填地址/Token/配置，验证整条链路含 config_id 合法性） */
async function probe(id?: string) {
  probingId.value = id ?? '__new'
  probeMsg.value = '测试中…'
  try {
    const body = id
      ? { id }
      : {
          base_url: form.value.base_url,
          token: form.value.token,
          config_id: form.value.config_id || undefined,
        }
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
    大肥鱼对话与语音的上游 = AstrBot 实例（可创建多个；设备端切换）。
    AstrBot 侧<b>无需创建机器人/选择平台类型</b>——走 HTTP OpenAPI：
    WebUI <b>设置 → OpenAPI → 创建 API Key</b>（对话勾 <code class="text-accent2">chat</code>；
    要在下面拉配置文件再勾 <code class="text-accent2">config</code>）。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="messageErr ? 'text-err' : 'text-ok'">{{ message }}</p>

  <div class="card">
    <h3 class="mb-1 font-semibold">{{ form.id ? '编辑机器人' : '创建机器人' }}</h3>
    <div class="mb-4 space-y-2">
      <div class="flex flex-wrap items-end gap-2">
        <input v-model="form.label" class="input !w-40" placeholder="名称（如：客厅机器人）" />
        <input v-model="form.base_url" class="input !w-64" placeholder="AstrBot 地址（http://ip:6185）" />
        <input v-model="form.token" class="input !w-48" type="password"
               :placeholder="form.id ? 'Token（留空=不修改）' : 'abk_ Token（未开鉴权可空）'" />
        <button class="btn-ghost" :disabled="!form.base_url && !form.id || probingId !== ''" @click="probe()">先测一下</button>
      </div>
      <div class="flex flex-wrap items-end gap-2">
        <select v-model="form.config_id" class="input !w-56" :disabled="!configs.length">
          <option value="">默认配置（default）</option>
          <option v-for="c in configs" :key="c.id" :value="c.id">{{ c.name }}（{{ c.id }}）</option>
        </select>
        <button class="btn-ghost" :disabled="fetchingCfg || (!form.base_url && !form.id)" @click="fetchConfigs">
          拉取配置文件
        </button>
        <input v-model="form.provider" class="input !w-40" placeholder="Provider 覆盖（可选）" />
        <input v-model="form.model" class="input !w-40" placeholder="模型覆盖（可选）" />
        <button class="btn" :disabled="busy || !form.label || !form.base_url" @click="addBot">
          {{ form.id ? '保存修改' : '创建机器人' }}
        </button>
        <button v-if="form.id" class="btn-ghost" @click="resetForm">取消编辑</button>
      </div>
      <p v-if="cfgMsg" class="text-xs" :class="cfgMsg.startsWith('✓') ? 'text-ok' : 'text-err'">{{ cfgMsg }}</p>
      <p v-if="probeMsg" class="text-xs" :class="probeMsg.startsWith('✓') ? 'text-ok' : 'text-err'">{{ probeMsg }}</p>
      <p class="text-xs text-dim">
        「配置文件」= AstrBot 里的人设/模型套餐（含人格情境+服务商）；
        Provider/模型覆盖为单次请求级高级项，通常留空即可。
      </p>
    </div>

    <h3 class="mb-1 font-semibold">机器人实例</h3>
    <div v-if="bots.length === 0" class="mb-2 text-sm text-dim">还没有机器人，添加一个：</div>
    <div class="space-y-2">
      <div v-for="b in bots" :key="b.id" class="flex items-center justify-between rounded border border-line px-3 py-2">
        <div class="min-w-0">
          <span class="font-medium">{{ b.label }}</span>
          <span class="ml-2 truncate text-xs text-dim">{{ b.base_url }}</span>
          <span class="ml-2 badge" :class="b.has_token ? '' : '!text-warn'">{{ b.has_token ? '已配 Token' : '无鉴权' }}</span>
          <span v-if="b.config_id" class="ml-1 badge !bg-accent/15 !text-accent">{{ b.config_id }}</span>
          <span v-if="b.model" class="ml-1 badge">{{ b.model }}</span>
        </div>
        <div class="flex shrink-0 gap-1">
          <button class="btn-ghost !px-2 !py-1" :disabled="probingId !== ''" @click="probe(b.id)">测试</button>
          <button class="btn-ghost !px-2 !py-1" @click="editBot(b)">编辑</button>
          <button class="btn-ghost !text-err !px-2 !py-1" @click="removeBot(b)">删除</button>
        </div>
      </div>
    </div>
  </div>

  <div class="card mt-4 text-xs text-dim">
    说明：Token 经 AES-256-GCM 加密落库（需 <code>BRIDGE_MASTER_KEY</code>），明文不回显。
    Bridge 按 <code>POST {地址}/api/v1/chat</code> 对接（username=gadget、session_id=gadget-bigfish-&lt;id&gt;
    每台机器人独立会话）；config_id 每次请求都携带，后台改动即时生效。
    人设细节（系统提示词、开场白）、服务商与插件都在 AstrBot 侧维护。
  </div>
</template>
