<script setup lang="ts">
// 设备文件（2026-11-16，TF 文件管理器配套，ADR-010）：
// 浏览器上传 → Bridge 落地目录；设备端「文件」App 从这里下载到 TF。
// 设备端也可把 TF 文件上传到这里备份（文件名任意，除路径分隔符）。
import { onMounted, ref } from 'vue'

import { BRIDGE_URL, request } from '@/api/client'

interface DeviceFile {
  name: string
  size: number
  updated_at: string
}

const files = ref<DeviceFile[]>([])
const loading = ref(false)
const uploading = ref(false)
const message = ref('')
const messageErr = ref(false)

function fmtSize(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`
  return `${(n / 1024 / 1024).toFixed(2)} MB`
}

function fmtTime(s: string): string {
  const d = new Date(s)
  return isNaN(d.getTime()) ? s : d.toLocaleString()
}

async function load() {
  loading.value = true
  try {
    const out = await request<{ files: DeviceFile[] }>('/api/v1/files')
    files.value = out.files ?? []
  } catch (e) {
    message.value = `读取失败：${(e as Error).message}`
    messageErr.value = true
  } finally {
    loading.value = false
  }
}

async function onPick(ev: Event) {
  const input = ev.target as HTMLInputElement
  const f = input.files?.[0]
  input.value = '' // 允许重复选同一文件
  if (!f) return
  if (f.size > 8 * 1024 * 1024) {
    message.value = '单文件上限 8MB'
    messageErr.value = true
    return
  }
  uploading.value = true
  message.value = ''
  try {
    await request(`/api/v1/files/${encodeURIComponent(f.name)}`, { method: 'POST', body: f })
    message.value = `已上传 ${f.name}（设备端「Bridge 文件」里可下载到 TF 卡）`
    messageErr.value = false
    await load()
  } catch (e) {
    message.value = `上传失败：${(e as Error).message}`
    messageErr.value = true
  } finally {
    uploading.value = false
  }
}

async function download(f: DeviceFile) {
  // 走 fetch 带 Bearer（直链 a[href] 无法带 Authorization）
  try {
    const token = localStorage.getItem('bridge_token')
    const resp = await fetch(`${BRIDGE_URL}/api/v1/files/${encodeURIComponent(f.name)}`, {
      headers: token ? { Authorization: `Bearer ${token}` } : {},
    })
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`)
    const blob = await resp.blob()
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = f.name
    a.click()
    URL.revokeObjectURL(url)
  } catch (e) {
    message.value = `下载失败：${(e as Error).message}`
    messageErr.value = true
  }
}

async function remove(f: DeviceFile) {
  if (!confirm(`删除 Bridge 上的「${f.name}」？（设备 TF 卡上的副本不受影响）`)) return
  try {
    await request(`/api/v1/files/${encodeURIComponent(f.name)}`, { method: 'DELETE' })
    await load()
  } catch (e) {
    message.value = `删除失败：${(e as Error).message}`
    messageErr.value = true
  }
}

onMounted(load)
</script>

<template>
  <h1 class="page-title">设备文件</h1>
  <p class="page-sub">
    Bridge 上的「设备文件」目录：这里上传的文件，可在设备「文件」App 的
    <b>Bridge 文件</b> 里下载到 TF 卡；设备上传的备份也出现在这里。
  </p>

  <div class="card mb-4">
    <div class="mb-3 flex items-center justify-between">
      <h3 class="font-semibold">上传到 Bridge</h3>
      <label class="btn cursor-pointer" :class="{ 'opacity-60': uploading }">
        {{ uploading ? '上传中…' : '选择文件' }}
        <input type="file" class="hidden" :disabled="uploading" @change="onPick" />
      </label>
    </div>
    <p class="text-xs text-dim">单文件 ≤ 8MB；设备端下载上限 2MB（PSRAM 缓冲）。</p>
  </div>

  <div class="card">
    <div class="mb-3 flex items-center justify-between">
      <h3 class="font-semibold">Bridge 文件</h3>
      <button class="btn-ghost" :disabled="loading" @click="load">
        {{ loading ? '加载中…' : '刷新' }}
      </button>
    </div>

    <div v-if="files.length === 0" class="py-6 text-center text-sm text-dim">
      暂无文件
    </div>

    <div class="flex flex-col gap-1">
      <div
        v-for="f in files"
        :key="f.name"
        class="flex items-center justify-between rounded-xl border border-line bg-card px-3.5 py-2.5 text-sm"
      >
        <span class="min-w-0 truncate">{{ f.name }}</span>
        <span class="ml-3 flex shrink-0 items-center gap-3">
          <span class="badge">{{ fmtSize(f.size) }}</span>
          <span class="hidden text-xs text-dim sm:inline">{{ fmtTime(f.updated_at) }}</span>
          <button class="btn-ghost" @click="download(f)">下载</button>
          <button class="btn-ghost text-err" @click="remove(f)">删除</button>
        </span>
      </div>
    </div>
  </div>

  <p v-if="message" class="mt-3 text-sm" :class="messageErr ? 'text-err' : 'text-ok'">
    {{ message }}
  </p>
</template>