<script setup lang="ts">
// 首次配网流程原型（PLAN §9.3）。
// 本页模拟"手机连接设备 SoftAP 后打开 192.168.4.1"的设备本地页面视角。
// 二维码内容只有 URL —— 不含密码/API Key/Cookie/Token（PLAN §9.3 安全规则）。
import QRCode from 'qrcode'
import { onMounted, ref } from 'vue'

import { deviceApi, type WifiNetwork } from '@/api/device'

const step = ref<1 | 2 | 3>(1)
const ssid = ref('')
const setupUrl = ref('')
const qrDataUrl = ref('')
const networks = ref<WifiNetwork[]>([])
const scanning = ref(false)
const chosen = ref<WifiNetwork | null>(null)
const password = ref('')
const connecting = ref(false)
const result = ref<{ ok: boolean; ip?: string; error?: string } | null>(null)

onMounted(async () => {
  const info = await deviceApi.provisionInfo()
  ssid.value = info.ssid
  setupUrl.value = info.setup_url
  // 二维码只编码 URL；随机热点密码只在设备屏幕上显示，绝不进二维码
  qrDataUrl.value = await QRCode.toDataURL(info.setup_url, {
    width: 220,
    margin: 1,
    color: { dark: '#0b1020', light: '#eaf0ff' },
  })
})

async function startScan() {
  step.value = 2
  scanning.value = true
  try {
    networks.value = await deviceApi.wifiScan()
  } finally {
    scanning.value = false
  }
}

function pick(n: WifiNetwork) {
  chosen.value = n
  password.value = ''
}

async function connect() {
  if (!chosen.value) return
  connecting.value = true
  result.value = null
  try {
    result.value = await deviceApi.wifiConnect(chosen.value.ssid, password.value)
    if (result.value.ok) step.value = 3
  } finally {
    connecting.value = false
  }
}

function reset() {
  step.value = 1
  chosen.value = null
  result.value = null
}
</script>

<template>
  <h1 class="page-title">首次配网</h1>
  <p class="page-sub">SoftAP + 设备本地网页流程原型（PLAN §9.3）· 当前为 Mock，模拟手机视角</p>

  <!-- 步骤指示 -->
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
        @click="pick(n)"
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
      <button class="btn" :disabled="connecting" @click="connect">
        {{ connecting ? '连接中…' : '连接并关闭热点' }}
      </button>
    </div>
    <p v-if="result && !result.ok" class="mt-3 text-sm text-err">连接失败（{{ result.error }}）</p>
  </div>

  <!-- Step 3: 完成 -->
  <div v-if="step === 3" class="card flex flex-col items-center gap-4 py-10">
    <div class="flex h-16 w-16 items-center justify-center rounded-full bg-ok/15 text-3xl text-ok">✓</div>
    <div class="text-center">
      <div class="font-semibold">配网完成，SoftAP 已关闭</div>
      <div class="mt-1 text-sm text-dim">
        局域网管理入口：<span class="font-mono text-txt">http://{{ result?.ip }}</span>
        （设备屏幕会显示同地址的二维码）
      </div>
    </div>
    <button class="btn-ghost" @click="reset">重新演示</button>
  </div>
</template>
