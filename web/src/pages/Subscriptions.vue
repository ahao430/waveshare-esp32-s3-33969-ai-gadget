<script setup lang="ts">
// 订阅额度页：订阅实例 = 类型 + 名称 + 端点 + Key，一处配置全搞定。
// Key 只进不出：保存后不回显；Bridge AES-256-GCM 落库，并同步投影到
// 凭据表（语音 ASR / 智谱预热动态解析同 kind 的 Key——改一处两处生效）。
// 2026-11-16 重构：砍掉独立"额度查询凭据"区（用户拍板：有实例就够了，
// 此前"配了凭据没建实例 → 设备订阅页空白"的困惑即源于两层概念）。
import { onMounted, ref } from 'vue'

import { request } from '@/api/client'

/* ---- 订阅实例管理（设备端订阅页 = 这里的镜像） ---- */
interface SubInfo {
  id: string
  kind: string
  label: string
  base_url?: string
  has_token: boolean
  updated_at: string
  currency?: string // newapi：站点展示币种（usd/cny）
}
const subs = ref<SubInfo[]>([])
const subKinds = ['zhipu', 'kimi', 'deepseek', 'gpt', 'claude', 'minimax', 'mimo', 'newapi', 'opencode', 'mock']
const subKindNames: Record<string, string> = {
  mock: 'Mock（演示）',
  zhipu: '智谱 Coding Plan',
  kimi: 'Kimi Coding Plan',
  deepseek: 'DeepSeek（余额）',
  gpt: 'GPT Coding Plan（中转）',
  claude: 'Claude Coding Plan（中转）',
  minimax: 'MiniMax Coding Plan（中转）',
  mimo: 'MiMo Coding Plan（中转）',
  opencode: 'OpenCode Zen',
  newapi: 'NewAPI 中转站',
}

/* 各类型默认端点（与 Bridge provider 包常量一致；中转站类无默认必填）。
 * 选类型即自动填入 Base URL，可手改；清空 = 用默认。 */
const defaultBase: Record<string, string> = {
  zhipu: 'https://open.bigmodel.cn',
  kimi: 'https://api.kimi.com',
  deepseek: 'https://api.deepseek.com',
  opencode: 'https://opencode.ai/zen/go/v1',
}

/* 「去获取」跳转目标：中转站类有 base_url 就跳站点；否则跳官方控制台 */
const officialUrls: Record<string, string> = {
  zhipu: 'https://open.bigmodel.cn',
  kimi: 'https://www.kimi.com',
  deepseek: 'https://platform.deepseek.com',
  gpt: 'https://chatgpt.com',
  claude: 'https://claude.ai',
  minimax: 'https://platform.minimax.cn',
  mimo: 'https://mimo.mi.com',
  opencode: 'https://opencode.ai/zen',
  newapi: '',
}
function fetchUrlFor(s: SubInfo): string {
  if (s.base_url) return s.base_url
  return officialUrls[s.kind] ?? ''
}

const subForm = ref({ kind: 'zhipu', label: '', base_url: '', token: '', system_token: '', system_user_id: '', currency: 'usd' })
const editingId = ref('') // ''=新增；非空=编辑该实例（POST 同 id 覆盖）
const subAdding = ref(false)
const message = ref('')

/* ---- 排序（仿外观页主页排布：本地交换 + dirty + 保存） ---- */
const orderDirty = ref(false)
const orderBusy = ref(false)
function moveSub(i: number, delta: number) {
  const j = i + delta
  if (j < 0 || j >= subs.value.length) return
  ;[subs.value[i], subs.value[j]] = [subs.value[j], subs.value[i]]
  orderDirty.value = true
}
async function saveOrder() {
  orderBusy.value = true
  try {
    await request('/api/v1/subscriptions/order', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ order: subs.value.map((s) => s.id) }),
    })
    orderDirty.value = false
    message.value = '排序已保存'
  } catch (e) {
    message.value = `保存排序失败：${e instanceof Error ? e.message : e}`
  } finally {
    orderBusy.value = false
  }
}

/* ---- 设备订阅自动刷新间隔（下发设备，见 Bridge device/config） ---- */
const devInterval = ref(900)
const devIntervalBusy = ref(false)
const devIntervalOptions = [
  { v: 60, label: '1 分钟' },
  { v: 120, label: '2 分钟' },
  { v: 180, label: '3 分钟' },
  { v: 300, label: '5 分钟' },
  { v: 600, label: '10 分钟' },
  { v: 900, label: '15 分钟' },
  { v: 1800, label: '30 分钟' },
  { v: 3600, label: '60 分钟' },
]
async function loadDeviceCfg() {
  try {
    const r = await request<{ sub_refresh_interval_s: number }>('/api/v1/device/config')
    devInterval.value = r.sub_refresh_interval_s
  } catch {
    /* 读不到保持默认 */
  }
}
async function saveDeviceCfg() {
  devIntervalBusy.value = true
  try {
    await request('/api/v1/device/config', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sub_refresh_interval_s: devInterval.value }),
    })
    message.value = `设备订阅自动刷新已设为 ${devIntervalOptions.find((o) => o.v === devInterval.value)?.label ?? devInterval.value + ' 秒'}（重开设备订阅页生效）`
  } catch (e) {
    message.value = `保存失败：${e instanceof Error ? e.message : e}`
  } finally {
    devIntervalBusy.value = false
  }
}

/* ---- 快照摘要（异步，不阻塞列表；设备订阅页同源数据） ---- */
interface SnapBrief {
  plan?: string
  balance?: string
  expires?: string
  error?: string
}
const snapMap = ref<Record<string, SnapBrief>>({})
async function loadSnaps() {
  try {
    const r = await request<{
      subscriptions: Array<{
        id: string
        error?: string
        snapshot?: {
          plan_name?: string
          plan_expires_at?: string
          credits?: Array<{ balance?: number; unit?: string }>
        }
      }>
    }>('/api/v1/quota/subscriptions')
    const m: Record<string, SnapBrief> = {}
    for (const e of r.subscriptions) {
      if (e.snapshot) {
        const c0 = e.snapshot.credits?.[0]
        m[e.id] = {
          plan: e.snapshot.plan_name,
          expires: e.snapshot.plan_expires_at,
          balance:
            c0 && c0.balance != null
              ? `${{ CNY: '¥', usd: '$' }[c0.unit ?? ''] ?? c0.unit}${c0.balance.toFixed(2)}`
              : undefined,
        }
      } else {
        m[e.id] = { error: e.error ?? '抓取失败' }
      }
    }
    snapMap.value = m
  } catch {
    /* 摘要拿不到不影响列表（可手动刷新） */
  }
}

// coding plan 家族（gpt/claude/minimax/mimo）与 newapi 走中转站，base_url 必填
const needsBaseKinds = ['gpt', 'claude', 'minimax', 'mimo', 'newapi']
function needsBase(kind: string) {
  return needsBaseKinds.includes(kind)
}

/* 切类型：Base URL 自动填默认（中转站类清空待填），名称空则预填类型名 */
function onKindChange() {
  subForm.value.base_url = defaultBase[subForm.value.kind] ?? ''
  if (!subForm.value.label) {
    subForm.value.label = subKindNames[subForm.value.kind]?.replace(/（.*）/, '') ?? ''
  }
}

async function loadSubs() {
  try {
    const r = await request<{ subscriptions: SubInfo[] }>('/api/v1/subscriptions')
    subs.value = r.subscriptions
    orderDirty.value = false
  } catch {
    subs.value = []
  }
  void loadSnaps() // 快照摘要异步补（实时抓取较慢，不阻塞列表）
}

function resetForm() {
  editingId.value = ''
  subForm.value = { kind: 'zhipu', label: '', base_url: '', token: '', system_token: '', system_user_id: '', currency: 'usd' }
  onKindChange()
}

/* 编辑：回填（base_url 空表示用默认 → 直接填默认值更直观；token 不回显，
 * 留空=保留原 Key） */
function editSub(s: SubInfo) {
  editingId.value = s.id
  subForm.value = {
    kind: s.kind,
    label: s.label,
    base_url: s.base_url || defaultBase[s.kind] || '',
    token: '',
    system_token: '', // 不回显（留空=保留原值，Bridge 侧合并）
    system_user_id: '',
    currency: s.currency || 'usd',
  }
  message.value = ''
}

async function submitSub() {
  if (!subForm.value.label) return
  if (needsBase(subForm.value.kind) && !subForm.value.base_url) {
    message.value = '该类型需要填 Base URL（中转站地址）'
    return
  }
  // 新增必须带 Key（Key 存实例里，没 Key 抓不了额度）；mock 演示除外。
  // 编辑留空 = 保留原 Key（Bridge 侧处理）。
  if (!editingId.value && !subForm.value.token && subForm.value.kind !== 'mock') {
    message.value = '新增订阅需要填 API Key（抓取额度用）'
    return
  }
  subAdding.value = true
  message.value = ''
  try {
    await request('/api/v1/subscriptions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(editingId.value ? { ...subForm.value, id: editingId.value } : subForm.value),
    })
    message.value = editingId.value
      ? `已更新订阅「${subForm.value.label}」`
      : `已添加订阅「${subForm.value.label}」`
    resetForm()
    await loadSubs()
  } catch (e) {
    message.value = `${editingId.value ? '更新' : '添加'}失败：${e instanceof Error ? e.message : e}`
  } finally {
    subAdding.value = false
  }
}

async function removeSub(id: string) {
  if (!confirm('删除该订阅？设备端订阅页将不再显示它。')) return
  await request(`/api/v1/subscriptions/${id}`, { method: 'DELETE' })
  if (editingId.value === id) resetForm()
  await loadSubs()
}

onMounted(() => {
  loadSubs()
  loadDeviceCfg()
  onKindChange()
})
</script>

<template>
  <h1 class="page-title">订阅额度</h1>
  <p class="page-sub">
    一个订阅实例 = 类型 + 名称 + 端点 + API Key，设备端订阅页显示这里配置的实例。
    Key 经 AES-256-GCM 加密落库（需 <code class="text-accent2">BRIDGE_MASTER_KEY</code>），
    明文不回显、不入日志；保存后会同步给语音识别等复用同类型 Key 的功能。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="message.includes('失败') ? 'text-err' : 'text-ok'">
    {{ message }}
  </p>

  <div class="card">
    <div class="mb-1 flex flex-wrap items-center justify-between gap-2">
      <h3 class="font-semibold">{{ editingId ? '编辑订阅' : '订阅实例' }}</h3>
      <!-- 排序保存：调整后高亮（原来藏在列表下方且默认 disabled，几乎看不见） -->
      <div v-if="subs.length > 1" class="flex items-center gap-2">
        <span v-if="orderDirty" class="text-xs text-warn">顺序已调整（设备订阅页按此顺序显示）</span>
        <button class="btn !px-3 !py-1 text-xs" :disabled="orderBusy || !orderDirty" @click="saveOrder">
          {{ orderBusy ? '保存中…' : '保存排序' }}
        </button>
      </div>
    </div>
    <p class="mb-3 text-xs text-dim">
      设备端订阅页显示这里的配置；无任何配置时设备显示"请添加订阅"。
      中转站类型（GPT/Claude/MiniMax/MiMo/NewAPI）需填站点 Base URL，其余留空走官方端点。
    </p>
    <div v-if="subs.length === 0" class="mb-2 text-sm text-dim">还没有订阅，添加一个：</div>
    <div class="space-y-2">
      <div v-for="(s, i) in subs" :key="s.id" class="rounded border border-line px-3 py-2"
           :class="editingId === s.id ? 'border-accent/50' : ''">
        <div class="flex items-center justify-between">
          <div class="min-w-0">
            <span class="font-medium">{{ s.label }}</span>
            <span class="ml-2 text-xs text-dim">{{ subKindNames[s.kind] ?? s.kind }}</span>
            <span class="ml-2 badge" :class="s.has_token ? '' : '!text-warn'">{{ s.has_token ? '已配 Key' : '无 Key' }}</span>
          </div>
          <div class="flex shrink-0 gap-1">
            <button class="btn-ghost !px-2 !py-1" :disabled="i === 0" @click="moveSub(i, -1)">↑</button>
            <button class="btn-ghost !px-2 !py-1" :disabled="i === subs.length - 1" @click="moveSub(i, 1)">↓</button>
            <a
              v-if="fetchUrlFor(s)"
              class="btn-ghost !px-2 !py-1"
              :href="fetchUrlFor(s)"
              target="_blank"
              rel="noopener"
            >去获取 ↗</a>
            <button class="btn-ghost !px-2 !py-1" @click="editSub(s)">编辑</button>
            <button class="btn-ghost !text-err !px-2 !py-1" @click="removeSub(s.id)">删除</button>
          </div>
        </div>
        <!-- 快照摘要（异步；设备订阅页同源） -->
        <div v-if="snapMap[s.id]" class="mt-1 text-xs" :class="snapMap[s.id].error ? 'text-dim' : 'text-dim'">
          <template v-if="snapMap[s.id].error">状态：{{ snapMap[s.id].error }}</template>
          <template v-else>
            <span v-if="snapMap[s.id].plan">{{ snapMap[s.id].plan }}</span>
            <span v-if="snapMap[s.id].expires" class="ml-2">
              到期 {{ new Date(snapMap[s.id].expires!).toLocaleDateString('zh-CN') }}
            </span>
            <span v-if="snapMap[s.id].balance" class="ml-2">余额 {{ snapMap[s.id].balance }}</span>
          </template>
        </div>
      </div>
    </div>
        <div class="mt-3 flex flex-wrap items-end gap-2">
      <select v-model="subForm.kind" class="input !w-auto" :disabled="!!editingId" @change="onKindChange">
        <option v-for="k in subKinds" :key="k" :value="k">{{ subKindNames[k] }}</option>
      </select>
      <input v-model="subForm.label" class="input !w-40" placeholder="名称（如：智谱主号）" />
      <input v-model="subForm.base_url" class="input !w-56"
             :placeholder="needsBase(subForm.kind) ? 'Base URL（必填：中转站地址）' : 'Base URL（默认官方端点）'" />
      <input v-model="subForm.token" class="input !w-48" type="password"
             :placeholder="editingId ? 'API Key（留空=保留原值）' : 'API Key（必填）'" />
      <button class="btn" :disabled="subAdding || !subForm.label" @click="submitSub">
        {{ subAdding ? '保存中…' : editingId ? '保存修改' : '添加订阅' }}
      </button>
      <button v-if="editingId" class="btn-ghost" @click="resetForm">取消</button>
    </div>
    <div v-if="subForm.kind === 'newapi'" class="mt-2 flex flex-wrap items-end gap-2">
      <input v-model="subForm.system_token" class="input !w-64" type="password"
             placeholder="系统访问令牌（留空=保留原值）" />
      <input v-model="subForm.system_user_id" class="input !w-32"
             placeholder="用户 ID" />
      <select v-model="subForm.currency" class="input !w-36">
        <option value="cny">余额币种：人民币（元）</option>
        <option value="usd">余额币种：美元（$）</option>
      </select>
      <span class="text-xs text-dim">
        查余额用「系统访问令牌」：站点控制台「个人设置」生成（不是 sk- API Key）；
        用户 ID 在个人设置页可见（新版站点必需）。币种按站点控制台展示选。
      </span>
    </div>
  </div>

  <div class="card mt-4">
    <h3 class="mb-1 font-semibold">设备订阅自动刷新</h3>
    <p class="mb-3 text-xs text-dim">
      设备订阅页按此间隔自动拉取额度（1~60 分钟；查询本身不消耗额度，仅「预热」按钮会消耗约 1%——但高频查询可能触发上游接口限频，按需选择）。注意：配置存在设备所连的那台 Bridge 上——改完重开设备订阅页（或等一次自动刷新）即生效，无需重启设备。
    </p>
    <div class="flex flex-wrap items-center gap-2">
      <select v-model.number="devInterval" class="input !w-40">
        <option v-for="o in devIntervalOptions" :key="o.v" :value="o.v">{{ o.label }}</option>
      </select>
      <button class="btn !px-3 !py-1" :disabled="devIntervalBusy" @click="saveDeviceCfg">
        {{ devIntervalBusy ? '保存中…' : '保存' }}
      </button>
    </div>
  </div>

  <div class="card mt-6 text-xs text-dim">
    说明：实例里的 Key 同时供语音识别（DeepSeek/智谱等）与额度抓取使用；
    抓取账号解析顺序 = 环境变量 → 实例/凭据库。保存后无需重启 Bridge，
    下一次刷新即生效。硅基流动等仅语音用的 Key 在
    <RouterLink to="/voice" class="text-accent2">语音设置</RouterLink> 页配置。
  </div>
</template>
