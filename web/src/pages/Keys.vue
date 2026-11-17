<script setup lang="ts">
// 按键页：三物理键短按/长按映射配置（POWER_MANAGEMENT §13.4）。
// PWR/BOOT 长按为硬件/固件固定行为，不可修改（页面只读展示）。
import { onMounted, ref } from 'vue'

import { request } from '@/api/client'

interface Keymap {
  pwr_short: string
  boot_short: string
  key3_short: string
  key3_long: string
}

const km = ref<Keymap>({ pwr_short: 'home', boot_short: 'back', key3_short: 'app', key3_long: 'ptt' })
const loaded = ref(false)
const message = ref('')
const messageErr = ref(false)
const busy = ref(false)

const optNames: Record<string, string> = {
  home: '回到首页',
  back: '返回（上滑同效）',
  none: '无操作',
  app: '交给当前应用',
  ptt: '语音说话（按住说、松手发）',
}

onMounted(async () => {
  try {
    const r = await request<{ keymap: Keymap }>('/api/v1/keys')
    km.value = r.keymap
    loaded.value = true
  } catch (e) {
    messageErr.value = true
    message.value = `读取失败：${e instanceof Error ? e.message : e}`
  }
})

async function save() {
  busy.value = true
  try {
    const r = await request<{ keymap: Keymap }>('/api/v1/keys', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(km.value),
    })
    km.value = r.keymap
    messageErr.value = false
    message.value = '已保存（设备开机联网后拉取生效）'
  } catch (e) {
    messageErr.value = true
    message.value = `保存失败：${e instanceof Error ? e.message : e}`
  } finally {
    busy.value = false
  }
}

const rows = [
  {
    key: 'pwr_short' as const,
    name: '电源键（PWRON）',
    options: ['home', 'none'],
    longLabel: '关机（长按：PMU 硬件行为，不可修改）',
    shortLabel: '息屏/点亮（固定，不可修改）', /* 2026-11-17 用户拍板 */
  },
  {
    key: 'boot_short' as const,
    name: 'BOOT 键',
    options: ['back', 'home', 'none'],
    longLabel: '打开配网页（固定，不可修改）',
  },
  {
    key: 'key3_short' as const,
    name: 'KEY 键（KEY3）',
    options: ['app', 'back', 'home', 'none'],
    longLabel: null, // KEY3 长按可配，单独渲染
  },
]
</script>

<template>
  <h1 class="page-title">物理按键</h1>
  <p class="page-sub">
    三个顶部按键的短按/长按行为。电源键短按（息屏/点亮）与长按（关机）
    均为固件固定功能，不可修改；BOOT 长按固定；其余可配置项保存后由
    设备联网拉取。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="messageErr ? 'text-err' : 'text-ok'">{{ message }}</p>

  <div class="grid grid-cols-1 gap-4 md:grid-cols-3">
    <div v-for="row in rows" :key="row.key" class="card space-y-3">
      <h3 class="font-semibold">{{ row.name }}</h3>
      <div v-if="row.shortLabel" class="rounded border border-dashed border-line px-3 py-2 text-xs text-dim">
        短按：<span class="text-warn">🔒 {{ row.shortLabel }}</span>
      </div>
      <label v-else class="block text-sm">
        <span class="mb-1 block text-dim">短按</span>
        <select v-model="km[row.key]" class="input" :disabled="!loaded">
          <option v-for="o in row.options" :key="o" :value="o">{{ optNames[o] }}</option>
        </select>
      </label>
      <label v-if="row.key === 'key3_short'" class="block text-sm">
        <span class="mb-1 block text-dim">长按</span>
        <select v-model="km.key3_long" class="input" :disabled="!loaded">
          <option value="ptt">{{ optNames.ptt }}</option>
          <option value="none">无操作</option>
        </select>
      </label>
      <div v-else class="rounded border border-dashed border-line px-3 py-2 text-xs text-dim">
        长按：<span class="text-warn">🔒 {{ row.longLabel }}</span>
      </div>
    </div>
  </div>

  <div class="mt-4 flex items-center gap-3">
    <button class="btn" :disabled="!loaded || busy" @click="save">保存按键配置</button>
    <span class="text-xs text-dim">默认：电源短按回首页 · BOOT 短按返回 · KEY 短按交给应用、长按语音说话</span>
  </div>

  <div class="card mt-4 text-xs text-dim">
    说明：聊天应用把「交给当前应用」实现为——短按开关连续对话、长按说话；
    其他应用未注册时「交给当前应用」等于无操作。双击 BOOT 等扩展事件后续按需开放。
  </div>
</template>
