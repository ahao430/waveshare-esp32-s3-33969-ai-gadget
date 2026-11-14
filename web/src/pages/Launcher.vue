<script setup lang="ts">
// 主页排布页：桌面应用的显示顺序与可见性（设备一屏 4 个自动分页）。
// order=可见顺序；不在列表=隐藏（桌面不显示，可随时恢复）。
import { computed, onMounted, ref } from 'vue'

import { request } from '@/api/client'

interface CatalogApp {
  key: string
  name: string
}

const catalog = ref<CatalogApp[]>([])
const order = ref<string[]>([]) // 当前可见顺序（含未保存的编辑态）
const message = ref('')
const messageErr = ref(false)
const busy = ref(false)
const dirty = ref(false)

const nameOf = (key: string) => catalog.value.find((c) => c.key === key)?.name ?? key
const hidden = computed(() => catalog.value.map((c) => c.key).filter((k) => !order.value.includes(k)))
const pages = computed(() => Math.max(1, Math.ceil(order.value.length / 4)))
const curPage = computed(() => Array.from({ length: pages.value }, (_, i) => order.value.slice(i * 4, i * 4 + 4)))

onMounted(async () => {
  try {
    const r = await request<{ order: string[]; catalog: CatalogApp[] }>('/api/v1/launcher')
    catalog.value = r.catalog
    order.value = [...r.order]
  } catch (e) {
    messageErr.value = true
    message.value = `读取失败：${e instanceof Error ? e.message : e}`
  }
})

function markDirty() {
  dirty.value = true
  message.value = ''
}

function move(i: number, delta: -1 | 1) {
  const j = i + delta
  if (j < 0 || j >= order.value.length) return
  ;[order.value[i], order.value[j]] = [order.value[j], order.value[i]]
  markDirty()
}

function hide(key: string) {
  if (order.value.length <= 1) {
    messageErr.value = true
    message.value = '至少保留一个应用'
    return
  }
  order.value = order.value.filter((k) => k !== key)
  markDirty()
}

function restore(key: string) {
  order.value.push(key)
  markDirty()
}

function resetDefault() {
  order.value = catalog.value.map((c) => c.key)
  markDirty()
}

async function save() {
  busy.value = true
  try {
    await request('/api/v1/launcher', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ order: order.value }),
    })
    messageErr.value = false
    message.value = `已保存（${order.value.length} 个应用，${pages.value} 页）——设备重启后生效`
    dirty.value = false
  } catch (e) {
    messageErr.value = true
    message.value = `保存失败：${e instanceof Error ? e.message : e}`
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <h1 class="page-title">主页排布</h1>
  <p class="page-sub">
    设置设备桌面的应用顺序与可见性。一屏 4 个、自动分页；隐藏的应用不在
    桌面显示（设置也可隐藏，但隐藏后将无法在设备端进设置——请谨慎）。
  </p>

  <p v-if="message" class="mb-4 text-sm" :class="messageErr ? 'text-err' : 'text-ok'">{{ message }}</p>

  <div class="grid grid-cols-1 gap-4 lg:grid-cols-2">
    <div class="card">
      <div class="mb-3 flex items-center justify-between">
        <h3 class="font-semibold">显示中（{{ order.length }} 个 · {{ pages }} 页）</h3>
        <span class="text-xs text-dim">保存后设备重启生效</span>
      </div>
      <div class="space-y-1.5">
        <div
          v-for="(k, i) in order"
          :key="k"
          class="flex items-center justify-between rounded border border-line px-3 py-2"
        >
          <div class="flex items-center gap-2">
            <span class="w-6 text-center text-xs text-dim">{{ Math.floor(i / 4) + 1 }}-{{ (i % 4) + 1 }}</span>
            <span class="font-medium">{{ nameOf(k) }}</span>
            <span v-if="k === 'settings'" class="badge !text-warn">隐藏前请注意</span>
          </div>
          <div class="flex gap-1">
            <button class="btn-ghost !px-2 !py-1" :disabled="i === 0" @click="move(i, -1)">↑</button>
            <button class="btn-ghost !px-2 !py-1" :disabled="i === order.length - 1" @click="move(i, 1)">↓</button>
            <button class="btn-ghost !text-err !px-2 !py-1" @click="hide(k)">隐藏</button>
          </div>
        </div>
      </div>
      <div class="mt-3 flex gap-2">
        <button class="btn" :disabled="busy || !dirty" @click="save">保存排布</button>
        <button class="btn-ghost" @click="resetDefault">恢复默认</button>
      </div>
    </div>

    <div class="space-y-4">
      <div v-if="hidden.length" class="card">
        <h3 class="mb-3 font-semibold">已隐藏（{{ hidden.length }}）</h3>
        <div class="flex flex-wrap gap-2">
          <button
            v-for="k in hidden"
            :key="k"
            class="btn-ghost !border-line"
            @click="restore(k)"
          >
            {{ nameOf(k) }} ↩ 恢复
          </button>
        </div>
      </div>
      <div class="card">
        <h3 class="mb-3 font-semibold">分页预览（一屏 4 个）</h3>
        <div v-for="(pg, pi) in curPage" :key="pi" class="mb-2">
          <div class="mb-1 text-xs text-dim">第 {{ pi + 1 }} 页</div>
          <div class="flex gap-2">
            <div
              v-for="k in pg"
              :key="k"
              class="flex-1 rounded border border-line px-2 py-3 text-center text-sm"
            >
              {{ nameOf(k) }}
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
