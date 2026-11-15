<script setup lang="ts">
// 代理页（2026-11-15 信息架构调整：从「网络」独立成单独 tab）。
// 第一版仅 Direct / HTTP CONNECT（PLAN §10.2），只作用于 Muse 出站流量。
import { onMounted, ref } from 'vue'

import { deviceApi, type ProxyConfig } from '@/api/device'

const proxy = ref<ProxyConfig | null>(null)
const proxyTesting = ref(false)
const proxyResult = ref('')
const proxySaving = ref(false)

onMounted(async () => {
  proxy.value = await deviceApi.proxy()
})

async function saveProxy() {
  if (!proxy.value) return
  proxySaving.value = true
  proxyResult.value = ''
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
  <h1 class="page-title">代理</h1>
  <p class="page-sub">
    设备出站 HTTP 代理（第一版仅 Direct / HTTP CONNECT，PLAN §10.2）。
    只作用于 Muse 等需要代理的上游，不改全局流量。
  </p>

  <div v-if="proxy" class="card">
    <h3 class="mb-3 font-semibold">HTTP 代理</h3>

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

  <div class="card mt-4 text-xs text-dim">
    说明：Muse 的云端连接跑在 Bridge（本机），Bridge 自身代理走环境变量
    HTTP(S)_PROXY / NO_PROXY；Muse 配置里的 proxy_url 优先级更高（见「Muse」页）。
  </div>
</template>
