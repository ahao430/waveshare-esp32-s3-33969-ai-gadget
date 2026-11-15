<script setup lang="ts">
// Muse 设置（2026-11-15 从「语音设置」独立成单独 tab）：
// 云端连接凭据（配对导出）加密落库 + 连通性测试。
// Muse 页的语音请求经 Bridge 连到用户的 Muse VM（Noise 加密通道，
// ADR-004），回复文本由本地 TTS 朗读。未配置时设备 Muse 页回落常规链路。
import { onMounted, ref } from 'vue'

import { request } from '@/api/client'

type MuseCfg = {
  node_id: string
  noise_host: string
  api_url: string
  proxy_url: string
  has_creds: boolean
}
const muse = ref<MuseCfg>({ node_id: '', noise_host: '', api_url: '', proxy_url: '', has_creds: false })
const museToken = ref('')
const museRefresh = ref('')
const museMsg = ref('')
const museProbe = ref('')
const museBusy = ref(false)

async function loadMuse() {
  try {
    const r = await request<MuseCfg>('/api/v1/muse/config')
    muse.value = {
      node_id: r.node_id ?? '',
      noise_host: r.noise_host ?? 'hatch.metaaivm.com',
      api_url: r.api_url ?? 'https://api.muse.ai',
      proxy_url: r.proxy_url ?? '',
      has_creds: !!r.has_creds,
    }
  } catch {
    /* 未配置 */
  }
}

async function saveMuse() {
  museBusy.value = true
  museMsg.value = ''
  try {
    const body: Record<string, unknown> = {
      node_id: muse.value.node_id,
      noise_host: muse.value.noise_host,
      api_url: muse.value.api_url,
      proxy_url: muse.value.proxy_url,
    }
    if (museToken.value) {
      body.creds = { access_token: museToken.value, refresh_token: museRefresh.value }
    }
    await request('/api/v1/muse/config', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    })
    museToken.value = ''
    museRefresh.value = ''
    museMsg.value = '✓ 已保存（下一次语音请求生效）'
    await loadMuse()
  } catch (e) {
    museMsg.value = `✗ ${e instanceof Error ? e.message : e}`
  } finally {
    museBusy.value = false
  }
}

async function probeMuse() {
  museBusy.value = true
  museProbe.value = '测试中…'
  try {
    const r = await request<{ ok: boolean; step?: string; error?: string; vm_id?: string }>(
      '/api/v1/muse/probe',
      { method: 'POST' },
    )
    museProbe.value = r.ok ? `✓ 连接正常（VM ${r.vm_id?.slice(0, 8)}…）` : `✗ ${r.step}: ${r.error}`
  } catch (e) {
    museProbe.value = `✗ ${e instanceof Error ? e.message : e}`
  } finally {
    museBusy.value = false
  }
}

async function clearMuse() {
  if (!confirm('删除 Muse 凭据？（Muse App 会回落到常规语音链路）')) return
  await request('/api/v1/muse/config', { method: 'DELETE' })
  muse.value.has_creds = false
  museMsg.value = '已删除'
}

onMounted(loadMuse)
</script>

<template>
  <h1 class="page-title">Muse</h1>
  <p class="page-sub">
    Muse App 的云端连接（ADR-004：Noise 会话跑在 Bridge，设备保持瘦客户端）。
    朗读音色在「语音设置」页按 App 配置；未配置凭据时 Muse 页走常规语音链路。
  </p>

  <div class="card mb-4">
    <h3 class="mb-2 font-semibold">首次使用前提：先刷官方 SDK 配对授权</h3>
    <ol class="list-decimal space-y-1 pl-5 text-sm text-dim">
      <li>确认原厂全片备份存在，刷官方 Muse Gadget SDK 固件
        （<code>sdkconfig.muse-waveshare-s3-216</code>，ESP-IDF v6.0.3）。</li>
      <li>手机 Muse App 配对设备并完成授权（屏上出现 <code>setup=done ws=up</code>）。</li>
      <li>从设备 NVS（<code>0x11000, 0xC000</code>）导出
        <code>access_token</code> / <code>refresh_token</code> / <code>node_id</code> / <code>noise_host</code>。</li>
      <li>刷回本项目固件，并重烧伴宠资源分区（官方固件的 files 会覆盖 <code>assets</code>）。</li>
      <li>回到本页填写凭据 → 保存 → 连通性测试。</li>
    </ol>
    <p class="mt-2 text-xs text-warn">
      当前固件无法发起 Muse 配对；凭据被 revoke/过期时需要重做上述 1–3 步。
      详细记录见 docs/HARDWARE.md §2.9 与 ADR-004。
    </p>
  </div>

  <div class="card">
    <div class="mb-3 flex flex-wrap items-center justify-between gap-2">
      <h3 class="font-semibold">云端连接</h3>
      <span class="badge" :class="muse.has_creds ? '!bg-ok/15 !text-ok' : '!text-warn'">
        {{ muse.has_creds ? '已配置（Muse 页走 Muse 云端）' : '未配置（Muse 页走常规链路）' }}
      </span>
    </div>
    <p class="mb-3 text-xs text-dim">
      凭据来自一次性配对：官方固件配对后从设备 NVS 导出
      <code>access_token</code> / <code>refresh_token</code> / <code>node_id</code>。
      四项里 <b>node_id + access_token 必填</b>；改完保存后用「连通性测试」验证。
    </p>
    <div class="grid grid-cols-1 gap-3 md:grid-cols-2">
      <label class="text-xs text-dim">node_id
        <input v-model="muse.node_id" class="input mt-1" placeholder="homelink-xxxxxx" />
      </label>
      <label class="text-xs text-dim">noise_host
        <input v-model="muse.noise_host" class="input mt-1" placeholder="hatch.metaaivm.com" />
      </label>
      <label class="text-xs text-dim">api_url
        <input v-model="muse.api_url" class="input mt-1" placeholder="https://api.muse.ai" />
      </label>
      <label class="text-xs text-dim">proxy_url（可选，VM 需代理时；详见「代理」页）
        <input v-model="muse.proxy_url" class="input mt-1" placeholder="http://127.0.0.1:7897" />
      </label>
      <label class="text-xs text-dim">access_token
        <input v-model="museToken" class="input mt-1" type="password"
               :placeholder="muse.has_creds ? '已配置（留空=不改）' : 'muse-secret-…'" />
      </label>
      <label class="text-xs text-dim">refresh_token（可选）
        <input v-model="museRefresh" class="input mt-1" type="password"
               :placeholder="muse.has_creds ? '已配置（留空=不改）' : 'ABRA.…'" />
      </label>
    </div>
    <div class="mt-3 flex flex-wrap items-center gap-3">
      <button class="btn" :disabled="museBusy" @click="saveMuse">保存</button>
      <button class="btn-ghost" :disabled="museBusy" @click="probeMuse">连通性测试</button>
      <button v-if="muse.has_creds" class="btn-ghost !text-err" :disabled="museBusy" @click="clearMuse">删除凭据</button>
      <span v-if="museMsg" class="text-xs" :class="museMsg.startsWith('✓') || museMsg === '已删除' ? 'text-ok' : 'text-err'">{{ museMsg }}</span>
      <span v-if="museProbe" class="text-xs" :class="museProbe.startsWith('✓') ? 'text-ok' : 'text-err'">{{ museProbe }}</span>
    </div>
  </div>

  <div class="card mt-4 text-xs text-dim">
    说明：凭据经 AES-256-GCM 加密落库、明文不回显不入日志；保存即生效（会话按凭据
    指纹自动重建）。配对流程与 NVS 导出步骤见 <code>docs/HARDWARE.md §2.9</code>。
  </div>
</template>
