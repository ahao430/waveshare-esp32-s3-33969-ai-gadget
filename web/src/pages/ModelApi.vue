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
const asrProviders = ['siliconflow', 'minimax', 'mimo'] as const
const providerTitle: Record<string, string> = {
  siliconflow: '硅基流动 SenseVoice',
  minimax: 'MiniMax ASR (asr-1.0)',
  mimo: '小米 MiMo ASR (mimo-v2.5-asr)',
}
const providerHint: Record<string, string> = {
  siliconflow: '注册 cloud.siliconflow.cn → API 密钥；新账号有免费额度',
  minimax: 'platform.minimax.cn → 账户管理 → 接口密钥',
  mimo: 'mimo.mi.com 控制台创建 API Key',
}

onMounted(load)

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
      配任意一家即可；Bridge 选择顺序 硅基流动 → MiniMax → MiMo → 智谱。
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

  <div class="card mt-4 text-xs text-dim">
    说明：Key 加密落库、不回显不入日志；保存即生效（无需重启 Bridge）。
    智谱 Coding Plan 的 Key 不含语音识别能力，不要用它做 ASR。
  </div>
</template>
