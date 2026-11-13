<script setup lang="ts">
import { onMounted, ref } from 'vue'

import { deviceApi, type ProxyConfig, type WifiNetwork } from '@/api/device'

const tab = ref<'wifi' | 'proxy'>('wifi')

/* ---- Wi-Fi ---- */
const scanning = ref(false)
const networks = ref<WifiNetwork[]>([])
const selected = ref<WifiNetwork | null>(null)
const password = ref('')
const connecting = ref(false)
const connectResult = ref<{ ok: boolean; ip?: string; error?: string } | null>(null)

async function scan() {
  scanning.value = true
  connectResult.value = null
  try {
    networks.value = await deviceApi.wifiScan()
  } finally {
    scanning.value = false
  }
}

function pick(n: WifiNetwork) {
  selected.value = n
  password.value = ''
  connectResult.value = null
}

async function connect() {
  if (!selected.value) return
  connecting.value = true
  connectResult.value = null
  try {
    connectResult.value = await deviceApi.wifiConnect(selected.value.ssid, password.value)
  } finally {
    connecting.value = false
  }
}

function rssiIcon(rssi: number): string {
  if (rssi > -50) return '▂▄▆█'
  if (rssi > -65) return '▂▄▆ '
  return '▂▄  '
}

/* ---- HTTP 代理 ---- */
const proxy = ref<ProxyConfig | null>(null)
const proxyTesting = ref(false)
const proxyResult = ref<string>('')
const proxySaving = ref(false)

onMounted(async () => {
  proxy.value = await deviceApi.proxy()
})

async function saveProxy() {
  if (!proxy.value) return
  proxySaving.value = true
  try {
    proxy.value = await deviceApi.updateProxy(proxy.value)
    proxyResult.value = '已保存'
  } finally {
    proxySaving.value = false
  }
}

async function testProxy() {
  proxyTesting.value = true
  proxyResult.value = ''
  try {
    const r = await deviceApi.proxyTest()
    proxyResult.value = r.state === 'ok' ? `连通 · ${r.latency_ms}ms` : `失败 · ${r.reason}`
  } finally {
    proxyTesting.value = false
  }
}
</script>

<template>
  <h1 class="page-title">网络</h1>
  <p class="page-sub">Wi-Fi 与 HTTP 代理（第一版仅 Direct / HTTP CONNECT，PLAN §10.2）· 当前为 Mock</p>

  <div class="mb-4 flex gap-2">
    <button :class="tab === 'wifi' ? 'btn' : 'btn-ghost'" @click="tab = 'wifi'">Wi-Fi</button>
    <button :class="tab === 'proxy' ? 'btn' : 'btn-ghost'" @click = "tab = 'proxy'">HTTP 代理</button>
  </div>

  <!-- Wi-Fi -->
  <div v-if="tab === 'wifi'">
    <div class="card">
      <div class="mb-3 flex items-center justify-between">
        <h3 class="font-semibold">可用网络</h3>
        <button class="btn-ghost" :disabled="scanning" @click="scan">
          {{ scanning ? '扫描中…' : '扫描' }}
        </button>
      </div>

      <div v-if="networks.length === 0" class="py-6 text-center text-sm text-dim">
        点击「扫描」查找附近 Wi-Fi
      </div>

      <div class="flex flex-col gap-1">
        <button
          v-for="n in networks"
          :key="n.ssid"
          class="flex items-center justify-between rounded-xl border px-3.5 py-2.5 text-left text-sm transition-colors"
          :class="selected?.ssid === n.ssid ? 'border-accent bg-cardhi' : 'border-line bg-card hover:bg-cardhi/60'"
          @click="pick(n)"
        >
          <span class="flex items-center gap-3">
            <span class="font-mono text-xs text-accent2">{{ rssiIcon(n.rssi) }}</span>
            {{ n.ssid }}
          </span>
          <span class="badge">{{ n.auth === 'open' ? '开放' : n.auth.toUpperCase() }} · {{ n.rssi }}dBm</span>
        </button>
      </div>
    </div>

    <div v-if="selected" class="card mt-4">
      <h3 class="mb-3 font-semibold">连接到「{{ selected.ssid }}」</h3>
      <div class="flex flex-col gap-3 sm:flex-row sm:items-end">
        <div class="flex-1">
          <div class="mb-1 text-xs text-dim">密码（开放网络无需填写）</div>
          <input
            v-model="password"
            class="input"
            type="password"
            :disabled="selected.auth === 'open'"
            placeholder="≥ 8 位"
          />
        </div>
        <button class="btn" :disabled="connecting" @click="connect">
          {{ connecting ? '连接中…' : '连接' }}
        </button>
      </div>
      <p v-if="connectResult" class="mt-3 text-sm" :class="connectResult.ok ? 'text-ok' : 'text-err'">
        {{ connectResult.ok ? `已连接 · IP ${connectResult.ip}` : `连接失败（${connectResult.error}）` }}
      </p>
    </div>
  </div>

  <!-- HTTP 代理 -->
  <div v-if="tab === 'proxy' && proxy">
    <div class="card">
      <h3 class="mb-3 font-semibold">Muse 出站代理（只作用于 Muse，不改全局流量）</h3>

      <div class="mb-4 flex gap-4 text-sm">
        <label class="flex items-center gap-2">
          <input v-model="proxy.mode" type="radio" value="direct" /> 直连（默认）
        </label>
        <label class="flex items-center gap-2">
          <input v-model="proxy.mode" type="radio" value="http_connect" /> HTTP CONNECT
        </label>
      </div>

      <div v-if="proxy.mode === 'http_connect'" class="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <div>
          <div class="mb-1 text-xs text-dim">代理服务器</div>
          <input v-model="proxy.host" class="input" placeholder="proxy.example.com" />
        </div>
        <div>
          <div class="mb-1 text-xs text-dim">端口</div>
          <input v-model.number="proxy.port" class="input" type="number" placeholder="8080" />
        </div>
        <div>
          <div class="mb-1 text-xs text-dim">Basic 认证（可选）</div>
          <input v-model="proxy.username" class="input" placeholder="用户名" />
        </div>
        <div class="sm:col-span-3">
          <div class="mb-1 text-xs text-dim">密码（留空 = 保留已存密码，不回显）</div>
          <input v-model="proxy.password" class="input" type="password" placeholder="••••••" />
        </div>
      </div>

      <div class="mt-4 flex items-center gap-3">
        <button class="btn" :disabled="proxySaving" @click="saveProxy">
          {{ proxySaving ? '保存中…' : '保存' }}
        </button>
        <button class="btn-ghost" :disabled="proxyTesting" @click="testProxy">
          {{ proxyTesting ? '测试中…' : '连通性测试' }}
        </button>
        <span v-if="proxyResult" class="text-sm" :class="proxyResult.includes('失败') ? 'text-err' : 'text-ok'">
          {{ proxyResult }}
        </span>
      </div>
    </div>
  </div>
</template>
