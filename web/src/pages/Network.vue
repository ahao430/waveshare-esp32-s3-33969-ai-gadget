<script setup lang="ts">
// 网络与配网（2026-11-15 信息架构调整：原「二维码/首次配网」页并入本页）。
// 三块：Wi-Fi 管理 / 首次配网流程（SoftAP + 设备本地网页，PLAN §9.3）/
// 后台二维码（本管理后台地址）。
// 二维码内容只有 URL —— 不含密码/API Key/Cookie/Token（PLAN §9.3 安全规则）。
import QRCode from 'qrcode'
import { onMounted, ref } from 'vue'

import { deviceApi, type WifiNetwork } from '@/api/device'

const tab = ref<'wifi' | 'provision' | 'qr'>('wifi')

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

/* ---- 首次配网（原 /provisioning 并入） ---- */
const step = ref<1 | 2 | 3>(1)
const ssid = ref('')
const setupUrl = ref('')
const qrDataUrl = ref('')
const chosen = ref<WifiNetwork | null>(null)

async function loadProvision() {
  const info = await deviceApi.provisionInfo()
  ssid.value = info.ssid
  setupUrl.value = info.setup_url
  // 二维码只编码 URL；随机热点密码只在设备屏幕上显示，绝不进二维码
  qrDataUrl.value = await QRCode.toDataURL(info.setup_url, {
    width: 220,
    margin: 1,
    color: { dark: '#0b1020', light: '#eaf0ff' },
  })
}

async function startScan() {
  step.value = 2
  await scan()
}

function pickProv(n: WifiNetwork) {
  chosen.value = n
  password.value = ''
}

async function connectProv() {
  if (!chosen.value) return
  connecting.value = true
  connectResult.value = null
  try {
    connectResult.value = await deviceApi.wifiConnect(chosen.value.ssid, password.value)
    if (connectResult.value.ok) step.value = 3
  } finally {
    connecting.value = false
  }
}

function resetFlow() {
  step.value = 1
  chosen.value = null
  connectResult.value = null
}

/* ---- 后台二维码：本管理后台地址 ---- */
const adminUrl = ref('')
const adminQr = ref('')

onMounted(async () => {
  loadProvision().catch(() => {})
  adminUrl.value = window.location.origin
  adminQr.value = await QRCode.toDataURL(adminUrl.value, {
    width: 220,
    margin: 1,
    color: { dark: '#0b1020', light: '#eaf0ff' },
  })
})
</script>

<template>
  <h1 class="page-title">网络与配网</h1>
  <p class="page-sub">
    Wi-Fi 管理、首次配网与后台入口。设备刷好固件后：手机连设备热点即可在
    <b>局域网</b>完成配网，之后用同一网络的浏览器打开本后台即可改设置并生效。
  </p>

  <div class="mb-4 flex gap-2">
    <button :class="tab === 'wifi' ? 'btn' : 'btn-ghost'" @click="tab = 'wifi'">Wi-Fi</button>
    <button :class="tab === 'provision' ? 'btn' : 'btn-ghost'" @click="tab = 'provision'">首次配网</button>
    <button :class="tab === 'qr' ? 'btn' : 'btn-ghost'" @click="tab = 'qr'">后台二维码</button>
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

  <!-- 首次配网 -->
  <div v-if="tab === 'provision'">
    <p class="mb-4 text-xs text-dim">SoftAP + 设备本地网页流程（PLAN §9.3）· 当前为原型演示，模拟手机视角</p>

    <div class="mb-5 flex items-center gap-2 text-sm">
      <template v-for="s in [1, 2, 3]" :key="s">
        <span
          class="flex h-7 w-7 items-center justify-center rounded-full"
          :class="step === s ? 'bg-accent text-white' : step > s ? 'bg-accent/30 text-txt' : 'bg-cardhi text-dim'"
        >{{ s }}</span>
        <span v-if="s < 3" class="h-px w-8 bg-line"></span>
      </template>
      <span class="ml-2 text-dim">{{ ['连接热点', '选择家庭 Wi-Fi', '完成'][step - 1] }}</span>
    </div>

    <!-- Step 1: 热点 + 二维码 -->
    <div v-if="step === 1" class="grid grid-cols-1 gap-4 md:grid-cols-2">
      <div class="card flex flex-col items-center justify-center gap-3 py-8">
        <img v-if="qrDataUrl" :src="qrDataUrl" alt="配网二维码" class="rounded-xl" />
        <div class="text-sm text-dim">扫码直接打开配网页</div>
      </div>
      <div class="card flex flex-col justify-center gap-4">
        <div>
          <div class="text-xs text-dim">设备热点（屏幕上同时显示，密码仅显示在设备屏幕）</div>
          <div class="mt-1 font-mono text-lg font-semibold">{{ ssid || '…' }}</div>
        </div>
        <ol class="list-decimal space-y-1.5 pl-5 text-sm text-dim">
          <li>手机连接上方热点（密码见设备屏幕）</li>
          <li>自动弹出或手动打开 <span class="font-mono text-txt">{{ setupUrl }}</span></li>
          <li>选择家庭 Wi-Fi 并输入密码</li>
        </ol>
        <button class="btn w-fit" @click="startScan">开始扫描 Wi-Fi</button>
      </div>
    </div>

    <!-- Step 2: 选择 Wi-Fi -->
    <div v-if="step === 2" class="card">
      <div class="mb-3 flex items-center justify-between">
        <h3 class="font-semibold">选择家庭 Wi-Fi</h3>
        <button class="btn-ghost" :disabled="scanning" @click="startScan">
          {{ scanning ? '扫描中…' : '重新扫描' }}
        </button>
      </div>

      <div class="flex flex-col gap-1">
        <button
          v-for="n in networks"
          :key="n.ssid"
          class="flex items-center justify-between rounded-xl border px-3.5 py-2.5 text-left text-sm transition-colors"
          :class="chosen?.ssid === n.ssid ? 'border-accent bg-cardhi' : 'border-line bg-card hover:bg-cardhi/60'"
          @click="pickProv(n)"
        >
          <span>{{ n.ssid }}</span>
          <span class="badge">{{ n.rssi }}dBm</span>
        </button>
      </div>

      <div v-if="chosen" class="mt-4 flex flex-col gap-3 sm:flex-row sm:items-end">
        <div class="flex-1">
          <div class="mb-1 text-xs text-dim">「{{ chosen.ssid }}」的密码</div>
          <input v-model="password" class="input" type="password" placeholder="Wi-Fi 密码" />
        </div>
        <button class="btn" :disabled="connecting" @click="connectProv">
          {{ connecting ? '连接中…' : '连接并关闭热点' }}
        </button>
      </div>
      <p v-if="connectResult && !connectResult.ok" class="mt-3 text-sm text-err">
        连接失败（{{ connectResult.error }}）
      </p>
    </div>

    <!-- Step 3: 完成 -->
    <div v-if="step === 3" class="card flex flex-col items-center gap-4 py-10">
      <div class="flex h-16 w-16 items-center justify-center rounded-full bg-ok/15 text-3xl text-ok">✓</div>
      <div class="text-center">
        <div class="font-semibold">配网完成，SoftAP 已关闭</div>
        <div class="mt-1 text-sm text-dim">
          局域网管理入口：<span class="font-mono text-txt">http://{{ connectResult?.ip }}</span>
          （设备屏幕会显示同地址的二维码）
        </div>
      </div>
      <button class="btn-ghost" @click="resetFlow">重新演示</button>
    </div>
  </div>

  <!-- 后台二维码 -->
  <div v-if="tab === 'qr'" class="grid grid-cols-1 gap-4 md:grid-cols-2">
    <div class="card flex flex-col items-center justify-center gap-3 py-8">
      <img v-if="adminQr" :src="adminQr" alt="后台二维码" class="rounded-xl" />
      <div class="font-mono text-sm text-dim">{{ adminUrl }}</div>
    </div>
    <div class="card space-y-3">
      <h3 class="font-semibold">管理后台入口</h3>
      <p class="text-sm text-dim">
        二维码内容是<b>当前打开本页的地址</b>。手机与设备在同一局域网时，扫码
        （或直接输入地址）即可打开这个配置后台——改主题、排布、订阅、语音都即时生效。
      </p>
      <p class="text-xs text-dim">
        现阶段后台跑在开发机的 Bridge/Web 服务上（本页地址即服务地址）；
        后续固件内置设备端服务后，设备屏幕会直接显示本后台的局域网地址二维码，
        刷好固件 → 局域网配网 → 局域网浏览器打开后台设置，全程无需开发机。
      </p>
      <p class="text-xs text-dim">二维码只编码 URL，不含任何密码 / Token（安全规则同配网码）。</p>
    </div>
  </div>
</template>
