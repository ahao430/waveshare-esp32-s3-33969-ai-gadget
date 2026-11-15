<script setup lang="ts">
// 模型 API 配置：设备 AI 能力实际调用到的 Key（与"订阅额度查询"完全分开）。
// 目前是语音识别（ASR）；后续直连对话模型/TTS 也放这里。
// Key 经 AES-256-GCM 加密落库（需 BRIDGE_MASTER_KEY），明文不回显。
import { computed, onMounted, ref } from 'vue'

import { request } from '@/api/client'

interface CredentialInfo {
  provider_id: string
  account_label: string
  updated_at: string
}

const creds = ref<CredentialInfo[]>([])
const editing = ref('')
const editKey = ref('')
const editLabel = ref('')
const saving = ref(false)
const message = ref('')

// 语音识别：配置任一即可；Bridge 选择顺序 硅基流动 → MiniMax → MiMo →
// 智谱（可用 ASR_PROVIDER 环境变量强制指定）
const asrProviders = ['siliconflow', 'deepseek', 'minimax', 'mimo'] as const
const providerTitle: Record<string, string> = {
  siliconflow: '硅基流动 SenseVoice',
  deepseek: 'DeepSeek 语音（ASR + TTS）',
  minimax: 'MiniMax ASR (asr-1.0)',
  mimo: '小米 MiMo ASR (mimo-v2.5-asr)',
}
const providerHint: Record<string, string> = {
  siliconflow: '注册 cloud.siliconflow.cn → API 密钥；新账号有免费额度',
  deepseek: 'platform.deepseek.com → API Keys。同一个 Key 同时用于识别与合成（音色 stella 女 / tide 男）',
  minimax: 'platform.minimax.cn → 账户管理 → 接口密钥',
  mimo: 'mimo.mi.com 控制台创建 API Key',
}

/* ---- 各 App 音色（Muse / 小智 / ZCode；大肥鱼用机器人实例设置）---- */
type VoicePref = { provider?: string; voice?: string }
const appVoice = ref<Record<string, VoicePref>>({ muse: {}, xiaozhi: {}, zcode: {} })
const appVoiceMsg = ref('')
const appVoiceSaving = ref(false)
const appRows = [
  { key: 'muse', label: 'Muse' },
  { key: 'xiaozhi', label: '小智' },
  { key: 'zcode', label: 'ZCode' },
] as const

const engines = [
  { id: '', label: '自动（有 DeepSeek Key 用 DeepSeek，否则 Edge）' },
  { id: 'deepseek', label: 'DeepSeek TTS' },
  { id: 'edge', label: 'Edge-TTS（免费）' },
]
const voices: Record<string, { id: string; label: string }[]> = {
  '': [
    { id: '', label: '默认音色' },
    { id: 'stella', label: 'DeepSeek · stella（女声）' },
    { id: 'tide', label: 'DeepSeek · tide（男声）' },
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
  edge: [
    { id: '', label: '默认（晓晓 女声）' },
    { id: 'zh-CN-XiaoxiaoNeural', label: '晓晓（女声）' },
    { id: 'zh-CN-YunxiNeural', label: '云希（男声）' },
    { id: 'zh-CN-XiaoyiNeural', label: '晓伊（女声）' },
    { id: 'zh-CN-YunyangNeural', label: '云扬（男声）' },
  ],
}
const voiceOptions = (engine: string) => voices[engine] ?? voices['']

onMounted(async () => {
  await load()
  await loadAppVoice()
})

async function loadAppVoice() {
  try {
    const r = await request<{ apps: Record<string, VoicePref> }>('/api/v1/voice/apps')
    const m: Record<string, VoicePref> = { muse: {}, xiaozhi: {}, zcode: {} }
    for (const k of Object.keys(m)) {
      const got = r.apps?.[k]
      m[k] = { provider: got?.provider ?? '', voice: got?.voice ?? '' }
    }
    appVoice.value = m
  } catch {
    /* 无配置时静默 */
  }
}

async function saveAppVoice() {
  appVoiceSaving.value = true
  appVoiceMsg.value = ''
  try {
    await request('/api/v1/voice/apps', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(appVoice.value),
    })
    appVoiceMsg.value = '✓ 已保存（下一次语音请求生效）'
  } catch (e) {
    appVoiceMsg.value = `✗ ${e instanceof Error ? e.message : e}`
  } finally {
    appVoiceSaving.value = false
  }
}

async function load() {
  try {
    const r = await request<{ credentials: CredentialInfo[] }>('/api/v1/credentials')
    creds.value = r.credentials
  } catch (e) {
    message.value = `凭据列表不可用：${e instanceof Error ? e.message : e}（需 BRIDGE_MASTER_KEY）`
  }
}

const infoOf = (id: string) => creds.value.find((c) => c.provider_id === id)

/* 当前生效的 ASR（与 Bridge 选择顺序一致） */
const activeAsr = computed(
  () => asrProviders.find((p) => infoOf(p)) ?? (infoOf('zhipu') ? 'zhipu' : ''),
)
const activeAsrLabel = computed(() =>
  activeAsr.value ? (providerTitle[activeAsr.value] ?? '智谱 GLM-ASR') : '',
)

function startEdit(id: string) {
  editing.value = id
  editKey.value = ''
  editLabel.value = infoOf(id)?.account_label ?? ''
  message.value = ''
}

async function save(id: string) {
  if (!editKey.value) return
  saving.value = true
  try {
    await request(`/api/v1/credentials/${id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ account_label: editLabel.value, api_key: editKey.value }),
    })
    message.value = `已保存 ${providerTitle[id] ?? id}（下一次语音请求即生效）`
    editing.value = ''
    editKey.value = ''
    await load()
  } catch (e) {
    message.value = `保存失败：${e instanceof Error ? e.message : e}`
  } finally {
    saving.value = false
  }
}

async function remove(id: string) {
  if (!confirm(`删除 ${providerTitle[id] ?? id} 的 Key？`)) return
  await request(`/api/v1/credentials/${id}`, { method: 'DELETE' })
  await load()
}
</script>

<template>
  <h1 class="page-title">模型 API</h1>
  <p class="page-sub">
    设备 AI 能力实际调用的 Key（语音识别等）。与「订阅中心」的额度查询凭据是两件事：
    这里决定<b>能不能听写语音</b>，订阅中心决定<b>设备订阅页显示多少额度</b>。
    聊天回复由「聊天机器人」页的 AstrBot/人设决定。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="message.includes('失败') || message.includes('不可用') ? 'text-err' : 'text-ok'">
    {{ message }}
  </p>

  <div class="card">
    <div class="mb-3 flex flex-wrap items-center justify-between gap-2">
      <h3 class="font-semibold">语音识别（ASR）</h3>
      <span class="badge" :class="activeAsr ? '!bg-ok/15 !text-ok' : '!text-warn'">
        {{ activeAsr ? `当前生效：${activeAsrLabel}` : '未配置（语音会失败）' }}
      </span>
    </div>
    <p class="mb-3 text-xs text-dim">
      链路：按住说话 → 设备录音 → <b>这里配的 Key 转写</b> → AstrBot/DeepSeek 回答 →
      Edge-TTS 合成语音 → 设备播放（TTS 免费、无需 Key）。
      配任意一家即可；Bridge 选择顺序 硅基流动 → DeepSeek → MiniMax → MiMo → 智谱。
      语音合成默认 Edge-TTS（免费）；配了 DeepSeek Key 会自动优先用 DeepSeek TTS
      （可用 <code>TTS_PROVIDER=edge</code> 强制回免费 Edge）。
    </p>

    <div class="grid grid-cols-1 gap-4 md:grid-cols-3">
      <div v-for="p in asrProviders" :key="p" class="rounded border border-line p-3">
        <div class="mb-2 flex items-center justify-between">
          <span class="text-sm font-semibold">{{ providerTitle[p] }}</span>
          <span class="badge" :class="infoOf(p) ? '!bg-ok/15 !text-ok' : ''">
            {{ infoOf(p) ? '已配置' : '未配置' }}
          </span>
        </div>
        <p class="mb-2 text-xs text-dim">{{ providerHint[p] }}</p>

        <template v-if="editing === p">
          <div class="flex flex-col gap-2">
            <input v-model="editLabel" class="input" placeholder="标签（可选）" />
            <input v-model="editKey" class="input" type="password"
                   :placeholder="infoOf(p) ? '输入新 Key 覆盖' : 'API Key'" />
            <div class="flex gap-2">
              <button class="btn" :disabled="saving || !editKey" @click="save(p)">保存</button>
              <button class="btn-ghost" @click="editing = ''">取消</button>
            </div>
          </div>
        </template>
        <template v-else>
          <div class="flex gap-2">
            <button class="btn-ghost !px-2 !py-1" @click="startEdit(p)">
              {{ infoOf(p) ? '更换 Key' : '配置 Key' }}
            </button>
            <button v-if="infoOf(p)" class="btn-ghost !text-err !px-2 !py-1" @click="remove(p)">
              删除
            </button>
          </div>
        </template>
      </div>
    </div>
  </div>

  <div class="card mt-4">
    <h3 class="mb-1 font-semibold">各 App 音色</h3>
    <p class="mb-3 text-xs text-dim">
      每个对话 App 可以有自己的引擎/音色；<b>大肥鱼</b>用「聊天机器人」页里实例的设置，
      这里配置 Muse / 小智 / ZCode。未配置时回退默认实例。
    </p>
    <div class="space-y-2">
      <div v-for="row in appRows" :key="row.key" class="flex flex-wrap items-center gap-2">
        <span class="w-16 text-sm">{{ row.label }}</span>
        <select
          :value="appVoice[row.key]?.provider ?? ''"
          class="input !w-64"
          @change="appVoice[row.key] = { provider: ($event.target as HTMLSelectElement).value, voice: '' }"
        >
          <option v-for="e in engines" :key="e.id" :value="e.id">{{ e.label }}</option>
        </select>
        <select
          :value="appVoice[row.key]?.voice ?? ''"
          class="input !w-56"
          @change="appVoice[row.key] = { ...appVoice[row.key], voice: ($event.target as HTMLSelectElement).value }"
        >
          <option v-for="v in voiceOptions(appVoice[row.key]?.provider ?? '')" :key="v.id" :value="v.id">
            {{ v.label }}
          </option>
        </select>
      </div>
    </div>
    <div class="mt-3 flex items-center gap-3">
      <button class="btn" :disabled="appVoiceSaving" @click="saveAppVoice">保存音色</button>
      <span v-if="appVoiceMsg" class="text-xs" :class="appVoiceMsg.startsWith('✓') ? 'text-ok' : 'text-err'">
        {{ appVoiceMsg }}
      </span>
    </div>
  </div>

  <div class="card mt-4 text-xs text-dim">
    说明：Key 加密落库、不回显不入日志；保存即生效（无需重启 Bridge）。
    智谱 Coding Plan 的 Key 不含语音识别能力，不要用它做 ASR。
  </div>
</template>
