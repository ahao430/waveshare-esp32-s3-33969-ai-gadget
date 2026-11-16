<script setup lang="ts">
// 个性化：主题色 + 自定义背景 + 主页排布（2026-11-15 后台信息架构调整：
// 主页排布并入外观；网络/二维码合并；代理/语音/Muse/小智独立成页）。
// 背景在浏览器压到 480×480 JPEG（≤256KB）再上传；Bridge 存文件，
// 设备下次启动拉取缓存。
import { computed, onMounted, ref } from 'vue'
import { ApiError, request } from '@/api/client'

const themes = [
  { name: '星云紫', accent: '#7C5CFF', accent2: '#4F8CFF' },
  { name: '极光青', accent: '#2EC4B6', accent2: '#4F8CFF' },
  { name: '落日橙', accent: '#FF8A3D', accent2: '#FF5D7A' },
  { name: '蔷薇粉', accent: '#FF6FA5', accent2: '#B28DFF' },
]
const currentTheme = ref(-1)

async function loadTheme() {
  try {
    const r = await request<{ theme: number }>('/api/v1/appearance/theme')
    currentTheme.value = r.theme
  } catch {
    currentTheme.value = -1
  }
}

async function pickTheme(idx: number) {
  try {
    await request('/api/v1/appearance/theme', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ theme: idx }),
    })
    currentTheme.value = idx
    message.value = `主题已下发（${themes[idx].name}），设备联网后自动生效。`
    isError.value = false
  } catch (err) {
    message.value = err instanceof Error ? err.message : String(err)
    isError.value = true
  }
}

const hasBackground = ref(false)
const previewUrl = ref<string | null>(null)
const uploading = ref(false)
const message = ref('')
const isError = ref(false)

async function refresh() {
  try {
    const info = await request<{ background: boolean }>('/api/v1/appearance')
    hasBackground.value = info.background
    previewUrl.value = info.background
      ? `${import.meta.env.VITE_BRIDGE_URL ?? ''}/api/v1/appearance/background?_=${Date.now()}`
      : null
  } catch {
    hasBackground.value = false
  }
}

onMounted(() => { refresh(); loadTheme(); loadLayout() })

async function onFile(e: Event) {
  const file = (e.target as HTMLInputElement).files?.[0]
  if (!file) return
  uploading.value = true
  message.value = ''
  try {
    const blob = await compressTo480(file)
    const resp = await fetch(
      `${import.meta.env.VITE_BRIDGE_URL ?? ''}/api/v1/appearance/background`,
      {
        method: 'POST',
        headers: buildAuthHeaders({ 'Content-Type': 'image/jpeg' }),
        body: blob,
      },
    )
    if (!resp.ok) {
      const body = (await resp.json().catch(() => null)) as { message?: string } | null
      throw new Error(body?.message ?? `HTTP ${resp.status}`)
    }
    message.value = `已上传（${(blob.size / 1024).toFixed(0)} KB）。设备重启后生效。`
    isError.value = false
    await refresh()
  } catch (err) {
    message.value = err instanceof Error ? err.message : String(err)
    isError.value = true
  } finally {
    uploading.value = false
    ;(e.target as HTMLInputElement).value = ''
  }
}

async function removeBackground() {
  try {
    await request('/api/v1/appearance/background', { method: 'DELETE' })
    message.value = '已恢复默认波浪背景（设备端重启后生效）。'
    isError.value = false
    await refresh()
  } catch (err) {
    message.value = err instanceof ApiError ? err.message : String(err)
    isError.value = true
  }
}

/** 压到 480×480（cover 裁切）JPEG，循环降质到 ≤256KB */
async function compressTo480(file: File): Promise<Blob> {
  const img = await loadImage(URL.createObjectURL(file))
  const size = 480
  const canvas = document.createElement('canvas')
  canvas.width = size
  canvas.height = size
  const ctx = canvas.getContext('2d')!
  const scale = Math.max(size / img.width, size / img.height)
  const w = img.width * scale
  const h = img.height * scale
  ctx.drawImage(img, (size - w) / 2, (size - h) / 2, w, h)
  URL.revokeObjectURL(img.src)

  for (const q of [0.85, 0.7, 0.5, 0.35]) {
    const blob = await new Promise<Blob>((resolve) =>
      canvas.toBlob((b) => resolve(b!), 'image/jpeg', q),
    )
    if (blob.size <= 256 * 1024) return blob
  }
  throw new Error('压缩后仍超过 256KB，请换一张图')
}

function loadImage(src: string): Promise<HTMLImageElement> {
  return new Promise((resolve, reject) => {
    const img = new Image()
    img.onload = () => resolve(img)
    img.onerror = () => reject(new Error('图片解析失败'))
    img.src = src
  })
}

function buildAuthHeaders(extra: Record<string, string>): Record<string, string> {
  const headers = { ...extra }
  const token = localStorage.getItem('bridge_token')
  if (token) headers.Authorization = `Bearer ${token}`
  return headers
}

/* ---- 主页排布（自 /launcher 并入）：设备一屏 4 个自动分页 ---- */
interface CatalogApp {
  key: string
  name: string
}

const catalog = ref<CatalogApp[]>([])
const order = ref<string[]>([]) // 当前可见顺序（含未保存的编辑态）
const layMsg = ref('')
const layMsgErr = ref(false)
const layBusy = ref(false)
const layDirty = ref(false)

const nameOf = (key: string) => catalog.value.find((c) => c.key === key)?.name ?? key
const pages = computed(() => Math.max(1, Math.ceil(order.value.length / 4)))
const curPage = computed(() => Array.from({ length: pages.value }, (_, i) => order.value.slice(i * 4, i * 4 + 4)))

async function loadLayout() {
  try {
    const r = await request<{ order: string[]; catalog: CatalogApp[] }>('/api/v1/launcher')
    catalog.value = r.catalog
    order.value = [...r.order]
  } catch (e) {
    layMsgErr.value = true
    layMsg.value = `读取失败：${e instanceof Error ? e.message : e}`
  }
}

function markDirty() {
  layDirty.value = true
  layMsg.value = ''
}

function move(i: number, delta: -1 | 1) {
  const j = i + delta
  if (j < 0 || j >= order.value.length) return
  ;[order.value[i], order.value[j]] = [order.value[j], order.value[i]]
  markDirty()
}

function resetDefault() {
  order.value = catalog.value.map((c) => c.key)
  markDirty()
}

async function saveLayout() {
  layBusy.value = true
  try {
    await request('/api/v1/launcher', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ order: order.value }),
    })
    layMsgErr.value = false
    layMsg.value = `已保存（${order.value.length} 个应用，${pages.value} 页）——设备重启后生效（开机联网时自动应用新排布）`
    layDirty.value = false
  } catch (e) {
    layMsgErr.value = true
    layMsg.value = `保存失败：${e instanceof Error ? e.message : e}`
  } finally {
    layBusy.value = false
  }
}
</script>

<template>
  <section class="space-y-6">
    <header>
      <h1 class="text-xl font-semibold">外观</h1>
      <p class="mt-1 text-sm text-dim">
        主题色、自定义背景与桌面主页排布。主题色也可在设备端「设置 → 显示与声音」切换。
      </p>
    </header>

    <div class="card">
      <h2 class="mb-1 font-medium">主题色</h2>
      <p class="mb-3 text-xs text-dim">点击即下发到设备（联网后生效）；设备端设置页也可切换。</p>
      <div class="grid grid-cols-2 gap-3 md:grid-cols-4">
        <button
          v-for="(t, i) in themes"
          :key="t.name"
          class="rounded-lg border p-3 text-left transition"
          :class="currentTheme === i ? 'border-accent' : 'border-line'"
          @click="pickTheme(i)"
        >
          <div
            class="h-10 rounded-md"
            :style="{ background: `linear-gradient(135deg, ${t.accent}, ${t.accent2})` }"
          />
          <p class="mt-2 text-sm">
            {{ t.name }}
            <span v-if="currentTheme === i" class="ml-1 badge">当前</span>
          </p>
        </button>
      </div>
    </div>

    <div class="card">
      <h2 class="mb-3 font-medium">自定义背景</h2>
      <div class="flex flex-wrap items-center gap-4">
        <div
          class="h-32 w-32 shrink-0 rounded-lg border border-line bg-black/40 bg-cover bg-center"
          :style="previewUrl ? { backgroundImage: `url(${previewUrl})` } : {}"
        >
          <span v-if="!previewUrl" class="flex h-full items-center justify-center text-xs text-dim">
            默认波浪
          </span>
        </div>
        <div class="space-y-2">
          <label class="btn-primary cursor-pointer inline-block">
            {{ uploading ? '上传中…' : '选择图片上传' }}
            <input type="file" accept="image/*" class="hidden" @change="onFile" />
          </label>
          <button v-if="hasBackground" class="btn-ghost" @click="removeBackground">
            恢复默认
          </button>
          <p class="text-xs text-dim">自动压缩到 480×480（≤256KB），设备启动时自动拉取。</p>
        </div>
      </div>
      <p v-if="message" class="mt-3 text-sm" :class="isError ? 'text-red-400' : 'text-emerald-400'">
        {{ message }}
      </p>
    </div>

    <!-- 主页排布 -->
    <h2 class="text-sm font-semibold text-dim">主页排布</h2>
    <p v-if="layMsg" class="mb-2 text-sm" :class="layMsgErr ? 'text-err' : 'text-ok'">{{ layMsg }}</p>

    <div class="grid grid-cols-1 gap-4 lg:grid-cols-2">
      <div class="card">
        <div class="mb-3 flex items-center justify-between">
          <h3 class="font-semibold">排布（{{ order.length }} 个 · {{ pages }} 页）</h3>
          <span class="text-xs text-dim">一屏 4 个、自动分页</span>
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
            </div>
            <div class="flex gap-1">
              <button class="btn-ghost !px-2 !py-1" :disabled="i === 0" @click="move(i, -1)">↑</button>
              <button class="btn-ghost !px-2 !py-1" :disabled="i === order.length - 1" @click="move(i, 1)">↓</button>
            </div>
          </div>
        </div>
        <div class="mt-3 flex gap-2">
          <button class="btn" :disabled="layBusy || !layDirty" @click="saveLayout">保存排布</button>
          <button class="btn-ghost" @click="resetDefault">恢复默认</button>
        </div>
      </div>

      <div class="space-y-4">
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

    <div class="card text-xs text-dim">
      说明：固件内置的 App 始终显示，排布只决定先后顺序（一屏 4 个自动分页）。
      排布保存后，设备重启生效；开机联网时设备会自动拉取最新排布（无需再重启一次）。
    </div>
  </section>
</template>
