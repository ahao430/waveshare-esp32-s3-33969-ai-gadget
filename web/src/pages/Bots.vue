<script setup lang="ts">
// 聊天机器人页：AstrBot 连接是公用配置（地址 + Token 只写不回），
// 每个机器人实例只选 名称 + AstrBot 配置文件（人设/模型套餐）。
// 增删改即时生效；Token 经 AES-256-GCM 加密落库（需 BRIDGE_MASTER_KEY）。
import { onMounted, onUnmounted, ref } from 'vue'

import { request } from '@/api/client'
import { playVoicePreview, stopVoicePreview } from '@/api/voicePreview'

interface BotInfo {
  id: string
  label: string
  config_id?: string
  provider?: string
  model?: string
  tts_provider?: string
  tts_voice?: string
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
/* 当前机器人（设备对话 App 用哪只实例；设备端点标题也能切） */
const activeBot = ref<{ id: string; label: string; fallback?: boolean }>({ id: '', label: '' })

async function loadActive() {
  try {
    activeBot.value = await request('/api/v1/chat/active')
  } catch {
    activeBot.value = { id: '', label: '' }
  }
}

async function setActive(b: BotInfo) {
  try {
    await request('/api/v1/chat/active', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: b.id }),
    })
    say(false, `已切换当前机器人为「${b.label}」（设备对话与语音即时跟随）`)
    await loadActive()
  } catch (e) {
    say(true, `切换失败：${e instanceof Error ? e.message : e}`)
  }
}

/* ---- 公用连接 ---- */
const conn = ref({ base_url: '', has_token: false })
const connForm = ref({ base_url: '', token: '' })
const connMsg = ref('')
const connSaving = ref(false)

/* ---- 实例 ---- */
const form = ref({
  id: '',
  label: '',
  config_id: '',
  provider: '',
  model: '',
  tts_provider: '',
  tts_voice: '',
})

// TTS 引擎与音色（音色来自引擎能力；自动=配了 DeepSeek Key 用 DeepSeek，否则 Edge）
const ttsEngines = [
  { id: '', label: '自动（按音色归属路由；无偏好时 DeepSeek > 火山 > Edge）' },
  { id: 'deepseek', label: 'DeepSeek TTS' },
  { id: 'volcengine', label: '火山豆包 TTS' },
  { id: 'edge', label: 'Edge-TTS（免费）' },
]
// 火山音色：v1 接口可用的大模型 1.0 音色；BV001/BV002 免费授权，
// 灿灿/炀炀需控制台"0 元下单"后可用
const volcVoices = [
  { id: '', label: '默认（BV001 通用女声）' },
  { id: 'BV001_streaming', label: 'BV001 通用女声（免费）' },
  { id: 'BV002_streaming', label: 'BV002 通用男声（免费）' },
  { id: 'BV407_streaming', label: '灿灿（女声·亲切）' },
  { id: 'BV406_streaming', label: '炀炀（男声·亲切）' },
]
const ttsVoices: Record<string, { id: string; label: string }[]> = {
  '': [
    { id: '', label: '默认音色' },
    { id: 'stella', label: 'DeepSeek · stella（女声）' },
    { id: 'tide', label: 'DeepSeek · tide（男声）' },
    { id: 'BV001_streaming', label: '火山 · BV001 通用女声（免费）' },
    { id: 'BV002_streaming', label: '火山 · BV002 通用男声（免费）' },
    { id: 'BV407_streaming', label: '火山 · 灿灿（女声）' },
    { id: 'BV406_streaming', label: '火山 · 炀炀（男声）' },
    { id: 'zh-CN-XiaoxiaoNeural', label: 'Edge · 晓晓（女声）' },
    { id: 'zh-CN-YunxiNeural', label: 'Edge · 云希（男声）' },
    { id: 'zh-CN-XiaoyiNeural', label: 'Edge · 晓伊（女声）' },
    { id: 'zh-CN-YunyangNeural', label: 'Edge · 云扬（男声）' },
  ],
  deepseek: [
    { id: '', label: '默认（stella 女声）' },
    { id: 'stella', label: 'stella（女声）' },
    { id: 'tide', label: 'tide（男声）' },
  ],
  volcengine: volcVoices,
  // Edge 免费通道实测可用（2026-11-15 真网逐个探测 25 个候选，
  // Azure 目录里的晓涵/晓秋/云泽等 12 个返回 Unsupported voice；
  // 目录与 bridge/internal/voice/voices_live_test.go 保持同步）
  edge: [
    { id: '', label: '默认（晓晓 女声）' },
    { id: 'zh-CN-XiaoxiaoNeural', label: '晓晓（女声）' },
    { id: 'zh-CN-XiaoyiNeural', label: '晓伊（女声）' },
    { id: 'zh-CN-XiaoxuanNeural', label: '晓萱（女声）' },
    { id: 'zh-CN-YunxiNeural', label: '云希（男声）' },
    { id: 'zh-CN-YunyangNeural', label: '云扬（男声）' },
    { id: 'zh-CN-YunjianNeural', label: '云健（男声）' },
    { id: 'zh-CN-YunxiaNeural', label: '云夏（少年音）' },
    { id: 'zh-CN-liaoning-XiaobeiNeural', label: '晓北（东北话·女）' },
    { id: 'zh-CN-shaanxi-XiaoniNeural', label: '晓妮（陕西话·女）' },
    { id: 'zh-HK-HiuMaanNeural', label: '曉曼（粤语·女）' },
    { id: 'zh-HK-WanLungNeural', label: '雲龍（粤语·男）' },
    { id: 'zh-TW-HsiaoChenNeural', label: '曉臻（台湾腔·女）' },
    { id: 'zh-TW-YunJheNeural', label: '雲哲（台湾腔·男）' },
  ],
}
const voiceOptions = (engine: string) => ttsVoices[engine] ?? ttsVoices['']

/* 音色试听：用当前选择合成一句（不保存）；每行独立「试听中」状态。 */
const previewing = ref('')
onUnmounted(stopVoicePreview)

async function previewTTS(key: string, provider: string, voice: string) {
  previewing.value = key
  try {
    await playVoicePreview(provider, voice)
  } catch (e) {
    say(true, `试听失败：${e instanceof Error ? e.message : e}`)
  } finally {
    if (previewing.value === key) previewing.value = ''
  }
}

const ttsLabel = (b: BotInfo) => {
  const eng = b.tts_provider === 'deepseek' ? 'DeepSeek' : b.tts_provider === 'edge' ? 'Edge' : '自动'
  return b.tts_voice ? `${eng} · ${b.tts_voice}` : eng
}
const configs = ref<AstrBotConfig[]>([])
const cfgMsg = ref('')
const fetchingCfg = ref(false)
const probingId = ref('')

const configName = (id?: string) =>
  id ? configs.value.find((c) => c.id === id)?.name ?? id : '默认配置'

onMounted(async () => {
  await loadConn()
  await load()
  await loadActive()
  await fetchConfigs()
})

function say(err: boolean, text: string) {
  messageErr.value = err
  message.value = text
}

async function loadConn() {
  try {
    const r = await request<{ base_url: string; has_token: boolean }>('/api/v1/astrbot/config')
    conn.value = r
    connForm.value.base_url = r.base_url
    connForm.value.token = ''
  } catch (e) {
    connMsg.value = `读取连接配置失败：${e instanceof Error ? e.message : e}`
  }
}

async function saveConn() {
  if (!connForm.value.base_url) return
  connSaving.value = true
  connMsg.value = ''
  try {
    const body: Record<string, unknown> = { base_url: connForm.value.base_url }
    if (connForm.value.token !== '') body.token = connForm.value.token // 留空=不覆盖
    await request('/api/v1/astrbot/config', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    })
    connMsg.value = '✓ 已保存（所有实例共用）'
    connForm.value.token = ''
    await loadConn()
  } catch (e) {
    connMsg.value = `✗ ${e instanceof Error ? e.message : e}`
  } finally {
    connSaving.value = false
  }
}

async function probeConn() {
  probingId.value = '__conn'
  connMsg.value = '测试中…'
  try {
    const r = await request<{ ok: boolean; reply?: string; error?: string }>(
      '/api/v1/astrbot/probe',
      { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' },
    )
    connMsg.value = r.ok ? `✓ 连通正常，回复：${r.reply}` : `✗ ${r.error}`
  } catch (e) {
    connMsg.value = `✗ ${e instanceof Error ? e.message : e}`
  } finally {
    probingId.value = ''
  }
}

async function load() {
  try {
    const r = await request<{ bots: BotInfo[] }>('/api/v1/bots')
    bots.value = r.bots
  } catch (e) {
    say(true, `机器人列表不可用：${e instanceof Error ? e.message : e}`)
  }
}

async function fetchConfigs() {
  fetchingCfg.value = true
  cfgMsg.value = '拉取中…'
  try {
    const r = await request<{ ok: boolean; configs?: AstrBotConfig[]; error?: string }>(
      '/api/v1/astrbot/configs',
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

function resetForm() {
  form.value = {
    id: '',
    label: '',
    config_id: '',
    provider: '',
    model: '',
    tts_provider: '',
    tts_voice: '',
  }
}

async function addBot() {
  if (!form.value.label) return
  busy.value = true
  try {
    const body: Record<string, unknown> = {
      label: form.value.label,
      config_id: form.value.config_id,
      provider: form.value.provider,
      model: form.value.model,
      tts_provider: form.value.tts_provider,
      tts_voice: form.value.tts_voice,
    }
    if (form.value.id) body.id = form.value.id
    await request('/api/v1/bots', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    })
    say(false, form.value.id ? `已更新「${form.value.label}」` : `已创建「${form.value.label}」（对话即时生效）`)
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
    config_id: b.config_id ?? '',
    provider: b.provider ?? '',
    model: b.model ?? '',
    tts_provider: b.tts_provider ?? '',
    tts_voice: b.tts_voice ?? '',
  }
  window.scrollTo({ top: 0, behavior: 'smooth' })
}

async function removeBot(b: BotInfo) {
  if (!confirm(`删除机器人「${b.label}」？设备端将不再路由到它。`)) return
  await request(`/api/v1/bots/${b.id}`, { method: 'DELETE' })
  say(false, `已删除「${b.label}」`)
  await load()
}

async function probe(id: string) {
  probingId.value = id
  try {
    const r = await request<{ ok: boolean; reply?: string; error?: string }>(
      '/api/v1/bots/probe',
      { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ id }) },
    )
    say(!r.ok, r.ok ? `✓ 连通正常，回复：${r.reply}` : `✗ ${r.error}`)
  } catch (e) {
    say(true, `✗ ${e instanceof Error ? e.message : e}`)
  } finally {
    probingId.value = ''
  }
}

/* 新对话：轮换该实例的 username+session（服务端会话与用户记忆一并遗忘），
 * 设备端聊天记录下次进入时自行清空。 */
async function freshChat(b: BotInfo) {
  if (!confirm(`和「${b.label}」开启新对话？该机器人的服务端记忆将被清除。`)) return
  try {
    await request(`/api/v1/chat/reset?bot=${encodeURIComponent(b.id)}`, { method: 'POST' })
    say(false, `已开启新对话（「${b.label}」不再记得之前的内容）`)
  } catch (e) {
    say(true, `✗ ${e instanceof Error ? e.message : e}`)
  }
}
</script>

<template>
  <h1 class="page-title">AstrBot</h1>
  <p class="page-sub">
    设备端 AstrBot 应用（对话与语音）的上游。AstrBot 侧无需创建机器人——走
    HTTP OpenAPI：WebUI <b>设置 → OpenAPI → 创建 API Key</b>（对话勾
    <code class="text-accent2">chat</code>；拉配置文件再勾 <code class="text-accent2">config</code>）。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="messageErr ? 'text-err' : 'text-ok'">{{ message }}</p>

  <!-- 公用连接 -->
  <div class="card">
    <h3 class="mb-1 font-semibold">AstrBot 连接（公用）</h3>
    <p class="mb-3 text-xs text-dim">所有机器人实例共用这个地址与 Token。</p>
    <div class="flex flex-wrap items-end gap-2">
      <input v-model="connForm.base_url" class="input !w-72" placeholder="AstrBot 地址（https://bot.example.com）" />
      <input v-model="connForm.token" class="input !w-56" type="password"
             :placeholder="conn.has_token ? 'Token 已保存（留空=不修改）' : 'abk_ Token（未开鉴权可空）'" />
      <button class="btn" :disabled="connSaving || !connForm.base_url" @click="saveConn">保存连接</button>
      <button class="btn-ghost" :disabled="probingId !== '' || !conn.base_url" @click="probeConn">测试连接</button>
      <span class="badge" :class="conn.has_token ? '!bg-ok/15 !text-ok' : ''">
        {{ conn.has_token ? 'Token 已配置' : '无 Token' }}
      </span>
    </div>
    <p v-if="connMsg" class="mt-2 text-xs" :class="connMsg.startsWith('✓') ? 'text-ok' : 'text-err'">{{ connMsg }}</p>
  </div>

  <!-- 实例 -->
  <div class="card mt-4">
    <h3 class="mb-1 font-semibold">{{ form.id ? '编辑机器人' : '新建机器人' }}</h3>
    <div class="mb-4 flex flex-wrap items-end gap-2">
      <input v-model="form.label" class="input !w-44" placeholder="名称（如：大肥鱼）" />
      <select v-model="form.config_id" class="input !w-64" :disabled="!configs.length">
        <option value="">默认配置（default）</option>
        <option v-for="c in configs" :key="c.id" :value="c.id">{{ c.name }}</option>
      </select>
      <button class="btn-ghost" :disabled="fetchingCfg || !conn.base_url" @click="fetchConfigs">
        刷新配置文件
      </button>
      <button class="btn" :disabled="busy || !form.label" @click="addBot">
        {{ form.id ? '保存修改' : '创建机器人' }}
      </button>
      <button v-if="form.id" class="btn-ghost" @click="resetForm">取消编辑</button>
    </div>
    <p v-if="cfgMsg" class="mb-2 text-xs" :class="cfgMsg.startsWith('✓') ? 'text-ok' : 'text-err'">{{ cfgMsg }}</p>
    <div class="mb-3 flex flex-wrap items-end gap-2">
      <label class="text-xs text-dim">语音合成</label>
      <select
        v-model="form.tts_provider"
        class="input !w-64"
        @change="form.tts_voice = ''"
      >
        <option v-for="e in ttsEngines" :key="e.id" :value="e.id">{{ e.label }}</option>
      </select>
      <select v-model="form.tts_voice" class="input !w-56">
        <option v-for="v in voiceOptions(form.tts_provider)" :key="v.id" :value="v.id">
          {{ v.label }}
        </option>
      </select>
      <button
        class="btn-ghost !px-2 !py-1"
        :disabled="previewing === 'form'"
        title="用当前选择的引擎/音色合成一句试听（不保存）"
        @click="previewTTS('form', form.tts_provider, form.tts_voice)"
      >
        {{ previewing === 'form' ? '试听中…' : '▶ 试听' }}
      </button>
      <span class="text-xs text-dim">语音链路使用「当前」实例的设置</span>
    </div>
    <details class="mb-2 text-xs text-dim">
      <summary class="cursor-pointer">高级：provider / 模型覆盖（通常留空）</summary>
      <div class="mt-2 flex flex-wrap gap-2">
        <input v-model="form.provider" class="input !w-44" placeholder="Provider 覆盖（可选）" />
        <input v-model="form.model" class="input !w-44" placeholder="模型覆盖（可选）" />
      </div>
    </details>

    <h3 class="mb-1 font-semibold">机器人实例</h3>
    <p class="mb-2 text-xs text-dim">
      当前设备对话使用
      <span class="badge !bg-ok/15 !text-ok">{{ activeBot.label || '未配置实例' }}</span>
      <template v-if="activeBot.fallback"> （未显式设置，默认第一个）</template>
      。设备 AstrBot 页点标题也能切到下一个实例；语音音色跟随当前实例设置。
    </p>
    <div v-if="bots.length === 0" class="mb-2 text-sm text-dim">还没有机器人，创建一个。</div>
    <div class="space-y-2">
      <div v-for="b in bots" :key="b.id" class="flex items-center justify-between rounded border border-line px-3 py-2">
        <div class="min-w-0">
          <span class="font-medium">{{ b.label }}</span>
          <span v-if="b.id === activeBot.id" class="ml-2 badge !bg-ok/15 !text-ok">当前</span>
          <span class="ml-2 badge !bg-accent/15 !text-accent">{{ configName(b.config_id) }}</span>
          <span v-if="b.model" class="ml-1 badge">{{ b.model }}</span>
          <span class="ml-1 badge">{{ ttsLabel(b) }}</span>
        </div>
        <div class="flex shrink-0 gap-1">
          <button
            v-if="b.id !== activeBot.id"
            class="btn-ghost !px-2 !py-1"
            @click="setActive(b)"
          >设为当前</button>
          <button
            class="btn-ghost !px-2 !py-1"
            :disabled="previewing === b.id"
            title="按该实例保存的引擎/音色试听"
            @click="previewTTS(b.id, b.tts_provider ?? '', b.tts_voice ?? '')"
          >{{ previewing === b.id ? '试听中…' : '试听' }}</button>
          <button class="btn-ghost !px-2 !py-1" :disabled="probingId !== ''" @click="probe(b.id)">测试</button>
          <button class="btn-ghost !px-2 !py-1" title="清除该机器人的服务端对话记忆" @click="freshChat(b)">新对话</button>
          <button class="btn-ghost !px-2 !py-1" @click="editBot(b)">编辑</button>
          <button class="btn-ghost !text-err !px-2 !py-1" @click="removeBot(b)">删除</button>
        </div>
      </div>
    </div>
  </div>

  <div class="card mt-4 text-xs text-dim">
    说明：Token 经 AES-256-GCM 加密落库（需 <code>BRIDGE_MASTER_KEY</code>），明文不回显。
    Bridge 按 <code>POST {地址}/api/v1/chat</code> 对接（username=gadget，每实例独立会话）；
    config_id 每次请求都携带，后台改动即时生效。人设细节（系统提示词、开场白）、
    服务商与插件都在 AstrBot 侧维护。
  </div>
</template>
