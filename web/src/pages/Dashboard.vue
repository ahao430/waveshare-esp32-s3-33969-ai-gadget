<script setup lang="ts">
// 配置总览：公共配置 + 各应用配置入口（Web=配置后台，不做查看）。
import { onMounted, ref } from 'vue'
import { RouterLink } from 'vue-router'

import { request } from '@/api/client'

const bridgeOk = ref<boolean | null>(null)
const bridgeVersion = ref('')
const themeName = ref('')

const adminUrl = typeof window !== 'undefined' ? window.location.origin : ''

const themes = ['星云紫', '极光青', '落日橙', '蔷薇粉']

onMounted(async () => {
  try {
    const r = await request<{ version?: string }>('/api/v1/device/status')
    bridgeOk.value = true
    bridgeVersion.value = r.version ?? ''
  } catch {
    bridgeOk.value = false
  }
  try {
    const r = await request<{ theme: number }>('/api/v1/appearance/theme')
    themeName.value = themes[r.theme] ?? ''
  } catch {
    themeName.value = ''
  }
})

const groups = [
  {
    title: '公共配置',
    items: [
      { to: '/appearance', label: '外观', desc: '主题色、自定义背景、主页排布（一屏 4 个自动分页）' },
      { to: '/network', label: '网络与配网', desc: 'Wi-Fi 管理、首次配网流程、后台二维码' },
      { to: '/proxy', label: '代理', desc: '出站 HTTP 代理（Direct / HTTP CONNECT）' },
      { to: '/keys', label: '物理按键', desc: '三键短按/长按映射（长按电源/BOOT 固定）' },
    ],
  },
  {
    title: '应用配置',
    items: [
      { to: '/subscriptions', label: '订阅中心', desc: '额度订阅实例（智谱/Kimi/DeepSeek/Coding Plan/NewAPI）' },
      { to: '/bots', label: 'AstrBot', desc: '机器人实例与连接（设备端对话对接，可多个）' },
      { to: '/voice', label: '语音设置', desc: '语音识别 Key（可跳转官方控制台获取）与各 App 音色' },
      { to: '/muse', label: 'Muse', desc: '云端连接凭据（配对导出）与连通性测试' },
      { to: '/xiaozhi', label: '小智', desc: '连接官方 xiaozhi.me（激活码绑定）；未绑定走本机语音链路' },
      { to: '/weather', label: '天气', desc: 'Open-Meteo 免 Key；自动 IP 定位或手动城市' },
      { to: '/files', label: '设备文件', desc: '与设备 TF 卡双向同步（设备「文件」App）' },
    ],
  },
]
</script>

<template>
  <section class="space-y-6">
    <header>
      <h1 class="text-xl font-semibold">配置总览</h1>
      <p class="mt-1 text-sm text-dim">
        Web 是设备的配置后台——公共配置与各应用配置在这里完成；数据查看
        （额度/监控/对话）在设备端体验。
        Bridge 状态：
        <span :class="bridgeOk ? 'text-ok' : 'text-err'">
          {{ bridgeOk === null ? '检查中…' : bridgeOk ? '已连接' : '未连接（:8081）' }}
        </span>
        <template v-if="themeName"> · 当前主题：{{ themeName }}</template>
        <template v-if="bridgeVersion"> · <span class="text-dim">v{{ bridgeVersion }}</span></template>
      </p>
    </header>

    <div class="card">
      <h3 class="mb-2 font-semibold">Bridge（自部署服务）</h3>
      <p class="mb-2 text-sm text-dim">
        设备的所有 AI 能力都由 <b>Bridge</b> 提供：聊天/语音转发 AstrBot、TTS 合成、
        天气与订阅额度抓取、配置下发。API Key 只在 Bridge 加密存储，设备端不含任何密钥。
      </p>
      <ul class="space-y-1 text-sm text-dim">
        <li>当前后台：<span class="text-accent2">{{ adminUrl }}</span>（Web 由 Bridge 托管，一个地址）</li>
        <li>
          <b>多环境切换</b>：设备「设置 → 网络与连接 → Bridge 地址」是地址簿——
          开发调试连本机电脑、日常使用切远程服务器，点一下即生效（无需重刷）。
        </li>
        <li>
          <b>部署</b>：开发用本机（<code>bridge/./run.sh</code>）；正式部署到常驻
          服务器/NAS——一个进程托管 API 与后台，步骤见仓库 <code>docs/DEPLOY.md</code>。
        </li>
        <li>
          <b>AstrBot 也要自部署</b>：聊天与语音回复的后端。部署后在
          <RouterLink to="/bots" class="text-accent2">AstrBot</RouterLink> 页填地址与 Token。
        </li>
        <li>
          拔 USB：Bridge 在服务器时设备完全独立；还在本机时 Mac 睡眠/关机则对话失败
          （设备地址簿可随时切换）。
        </li>
        <li>
          安全：仅限可信网络；公网部署请设 <code>BRIDGE_TOKEN</code> 并配 TLS。
        </li>
      </ul>
    </div>

    <div class="card">
      <h3 class="mb-2 font-semibold">新用户上手</h3>
      <ol class="list-decimal space-y-1 pl-5 text-sm text-dim">
        <li>
          （可选）<b>Muse 授权</b>：想用 Muse App 才需要——先刷官方 Muse SDK 配对一次，
          导出配对凭据后刷回本固件；凭据在
          <RouterLink to="/muse" class="text-accent2">Muse</RouterLink> 页填。
        </li>
        <li><b>部署 Bridge + AstrBot</b>（服务器或本机，步骤见 <code>docs/DEPLOY.md</code>）。</li>
        <li><b>刷固件</b>（首次务必先做原厂全片备份）→ 设备「设置 → 网络与连接」配 Wi-Fi 与 Bridge 地址。</li>
        <li>
          <b>在本后台完成配置</b>：
          <RouterLink to="/bots" class="text-accent2">AstrBot</RouterLink> 实例 ·
          <RouterLink to="/voice" class="text-accent2">语音设置</RouterLink> Key/音色 ·
          <RouterLink to="/subscriptions" class="text-accent2">订阅中心</RouterLink> ·
          <RouterLink to="/xiaozhi" class="text-accent2">小智</RouterLink> ·
          <RouterLink to="/appearance" class="text-accent2">外观排布</RouterLink>。
        </li>
      </ol>
    </div>

    <div v-for="g in groups" :key="g.title" class="space-y-3">
      <h2 class="text-sm font-medium text-dim">{{ g.title }}</h2>
      <div class="grid grid-cols-1 gap-4 md:grid-cols-2">
        <RouterLink
          v-for="it in g.items"
          :key="it.label"
          :to="it.to"
          class="card block transition hover:border-accent"
        >
          <h3 class="font-semibold">{{ it.label }}</h3>
          <p class="mt-1 text-sm text-dim">{{ it.desc }}</p>
        </RouterLink>
      </div>
    </div>

    <div class="card text-xs text-dim">
      说明：机器人（AstrBot）、订阅与按键映射均由 Bridge
      加密落库并即时生效；设备端开机联网后拉取最新配置。开发用本机 Bridge、
      正式用服务器 Bridge，可在设备地址簿里一键切换。
    </div>
  </section>
</template>
