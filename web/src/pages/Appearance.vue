<script setup lang="ts">
// 个性化：设备主题色预览 + 自定义背景上传（用户 2026-11-14 需求）。
// 背景在浏览器压到 480×480 JPEG（≤256KB）再上传；Bridge 存文件，
// 设备下次启动拉取缓存。
import { onMounted, ref } from 'vue'
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
    message.value = `主题已下发（${themes[idx].name}），设备重启模拟器后生效。`
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

onMounted(() => { refresh(); loadTheme() })

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
    message.value = `已上传（${(blob.size / 1024).toFixed(0)} KB）。设备重启模拟器后生效。`
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
    message.value = '已恢复默认波浪背景（设备端重启模拟器后生效）。'
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
</script>

<template>
  <section class="space-y-6">
    <header>
      <h1 class="text-xl font-semibold">个性化</h1>
      <p class="mt-1 text-sm text-dim">
        主题色在设备端「设置 → 显示与声音」切换；背景图在这里上传，设备启动时自动拉取。
      </p>
    </header>

    <div class="card">
      <h2 class="mb-1 font-medium">主题色</h2>
      <p class="mb-3 text-xs text-dim">点击即下发到设备（重启模拟器生效）；设备端设置页也可切换。</p>
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
      <h2 class="mb-3 mt-6 font-medium">自定义背景</h2>
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
          <p class="text-xs text-dim">自动压缩到 480×480（≤256KB）。</p>
        </div>
      </div>
      <p v-if="message" class="mt-3 text-sm" :class="isError ? 'text-red-400' : 'text-emerald-400'">
        {{ message }}
      </p>
    </div>
  </section>
</template>
