<script setup lang="ts">
// 订阅额度页：Provider 账号配置（PLAN §15.3 凭据加密存储的 Web 入口）。
// Key 只进不出：保存后不回显；Bridge 侧 AES-256-GCM 落库。
import { onMounted, ref } from 'vue'

import { request } from '@/api/client'

interface CredentialInfo {
  provider_id: string
  account_label: string
  updated_at: string
}

const creds = ref<CredentialInfo[]>([])
const editKey = ref('')
const editLabel = ref('')
const editing = ref<string>('') // 正在编辑的 provider id
const message = ref('')
const saving = ref(false)

// 本页只管额度：订阅实例 + 额度查询凭据。
// 语音识别等能力 Key 在「模型 API」页（/models），两者不要混。
const quotaCreds = ['zhipu', 'kimi']
const providerTitle: Record<string, string> = {
  zhipu: '智谱 GLM Coding Plan',
  kimi: 'Kimi Coding Plan',
}
const providerHint: Record<string, string> = {
  zhipu: '仅用于查询 Coding Plan 额度（语音识别请去「模型 API」配置）',
  kimi: '仅用于查询 Coding Plan 额度',
}

onMounted(() => { load(); loadSubs() })

async function load() {
  try {
    const r = await request<{ credentials: CredentialInfo[] }>('/api/v1/credentials')
    creds.value = r.credentials
  } catch (e) {
    message.value = `凭据列表不可用：${e instanceof Error ? e.message : e}（需 BRIDGE_MASTER_KEY）`
  }
}

function infoOf(id: string): CredentialInfo | undefined {
  return creds.value.find((c) => c.provider_id === id)
}

function startEdit(id: string) {
  editing.value = id
  editKey.value = ''
  editLabel.value = infoOf(id)?.account_label ?? ''
  message.value = ''
}

async function save(id: string) {
  if (!editKey.value) return
  saving.value = true
  message.value = ''
  try {
    await request(`/api/v1/credentials/${id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ account_label: editLabel.value, api_key: editKey.value }),
    })
    message.value = `已保存 ${id}（下一次刷新即生效）`
    editing.value = ''
    editKey.value = ''
    await load()
  } catch (e) {
    message.value = `保存失败：${e instanceof Error ? e.message : e}`
  } finally {
    saving.value = false
  }
}


/* ---- 订阅实例管理（动态多订阅；设备端聚合展示） ---- */
interface SubInfo {
  id: string
  kind: string
  label: string
  base_url?: string
  has_token: boolean
  updated_at: string
}
const subs = ref<SubInfo[]>([])
const subKinds = ['zhipu', 'kimi', 'opencode', 'newapi', 'mock']
const subKindNames: Record<string, string> = {
  mock: 'Mock（演示）',
  zhipu: '智谱 Coding Plan',
  kimi: 'Kimi Coding Plan',
  opencode: 'OpenCode Zen',
  newapi: 'NewAPI 中转站',
}
const subForm = ref({ kind: 'zhipu', label: '', base_url: '', token: '' })
const subAdding = ref(false)

async function loadSubs() {
  try {
    const r = await request<{ subscriptions: SubInfo[] }>('/api/v1/subscriptions')
    subs.value = r.subscriptions
  } catch {
    subs.value = []
  }
}

async function addSub() {
  if (!subForm.value.label) return
  subAdding.value = true
  message.value = ''
  try {
    await request('/api/v1/subscriptions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(subForm.value),
    })
    message.value = `已添加订阅「${subForm.value.label}」`
    subForm.value = { kind: subForm.value.kind, label: '', base_url: '', token: '' }
    await loadSubs()
  } catch (e) {
    message.value = `添加失败：${e instanceof Error ? e.message : e}`
  } finally {
    subAdding.value = false
  }
}

async function removeSub(id: string) {
  if (!confirm('删除该订阅？设备端订阅页将不再显示它。')) return
  await request(`/api/v1/subscriptions/${id}`, { method: 'DELETE' })
  await loadSubs()
}

async function remove(id: string) {
  if (!confirm(`删除 ${id} 的凭据？设备下一次刷新将无法抓取该 Provider。`)) return
  await request(`/api/v1/credentials/${id}`, { method: 'DELETE' })
  await load()
}
</script>

<template>
  <h1 class="page-title">订阅额度</h1>
  <p class="page-sub">
    本页只负责<b>额度</b>：订阅实例（设备订阅页显示什么）+ 额度查询凭据（抓额度用的 Key）。
    语音识别等能力 Key 在 <RouterLink to="/models" class="text-accent2">模型 API</RouterLink> 页配置。
    Key 经 AES-256-GCM 加密落库（需 <code class="text-accent2">BRIDGE_MASTER_KEY</code>），明文不回显、不入日志。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="message.includes('失败') || message.includes('不可用') ? 'text-err' : 'text-ok'">
    {{ message }}
  </p>

  <div class="card">
    <h3 class="mb-1 font-semibold">订阅实例</h3>
    <p class="mb-3 text-xs text-dim">设备端订阅页显示这里的配置；无任何配置时设备显示"请添加订阅"。</p>
    <div v-if="subs.length === 0" class="mb-2 text-sm text-dim">还没有订阅，添加一个：</div>
    <div class="space-y-2">
      <div v-for="s in subs" :key="s.id" class="flex items-center justify-between rounded border border-line px-3 py-2">
        <div>
          <span class="font-medium">{{ s.label }}</span>
          <span class="ml-2 text-xs text-dim">{{ subKindNames[s.kind] ?? s.kind }}</span>
          <span class="ml-2 badge" :class="s.has_token ? '' : '!text-warn'">{{ s.has_token ? '已配 Key' : '无 Key' }}</span>
        </div>
        <button class="btn-ghost !text-err !px-2 !py-1" @click="removeSub(s.id)">删除</button>
      </div>
    </div>
    <div class="mt-3 flex flex-wrap items-end gap-2">
      <select v-model="subForm.kind" class="input !w-auto">
        <option v-for="k in subKinds" :key="k" :value="k">{{ subKindNames[k] }}</option>
      </select>
      <input v-model="subForm.label" class="input !w-40" placeholder="名称（如：智谱主号）" />
      <input v-model="subForm.base_url" class="input !w-56" placeholder="Base URL（可选）" />
      <input v-model="subForm.token" class="input !w-48" type="password" placeholder="API Key / Token（可选）" />
      <button class="btn" :disabled="subAdding || !subForm.label" @click="addSub">添加订阅</button>
    </div>
  </div>

  <h2 class="mt-6 mb-2 text-sm font-semibold text-dim">额度查询凭据（订阅页取数）</h2>

  <div class="grid grid-cols-1 gap-4 md:grid-cols-2">
    <div v-for="p in quotaCreds" :key="p" class="card">
      <div class="mb-3 flex items-center justify-between">
        <h3 class="font-semibold">{{ providerTitle[p] ?? p }}</h3>
        <span class="badge" :class="infoOf(p) ? '!bg-ok/15 !text-ok' : ''">
          {{ infoOf(p) ? `已配置 · ${infoOf(p)!.account_label || '未命名'}` : '未配置' }}
        </span>
      </div>

      <p class="mb-2 text-xs text-dim">{{ providerHint[p] }}</p>
      <div v-if="infoOf(p)" class="mb-3 text-xs text-dim">
        更新于 {{ new Date(infoOf(p)!.updated_at).toLocaleString('zh-CN', { hour12: false }) }}
      </div>

      <template v-if="editing === p">
        <div class="flex flex-col gap-3">
          <input v-model="editLabel" class="input" placeholder="账号标签（可选）" />
          <input
            v-model="editKey"
            class="input"
            type="password"
            :placeholder="infoOf(p) ? '输入新 Key 覆盖' : 'API Key'"
          />
          <div class="flex gap-2">
            <button class="btn" :disabled="saving || !editKey" @click="save(p)">
              {{ saving ? '保存中…' : '保存' }}
            </button>
            <button class="btn-ghost" @click="editing = ''">取消</button>
          </div>
        </div>
      </template>
      <template v-else>
        <div class="flex gap-2">
          <button class="btn-ghost" @click="startEdit(p)">
            {{ infoOf(p) ? '更换 Key' : '配置 Key' }}
          </button>
          <button v-if="infoOf(p)" class="btn-ghost !text-err" @click="remove(p)">删除</button>
        </div>
      </template>
    </div>
  </div>

  <div class="card mt-6 text-xs text-dim">
    说明：额度抓取账号解析顺序 = 环境变量（ZHIPU_API_KEY 等）→ 此处配置的加密凭据。
    保存后无需重启 Bridge，下一次「刷新」即生效。
  </div>
</template>
