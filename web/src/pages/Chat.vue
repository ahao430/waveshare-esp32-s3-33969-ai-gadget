<script setup lang="ts">
// Bigfish Chat 调试页：验证 Bridge Model Router 链路（SSE 流式）。
// 会话仅存内存；正式设备端 UI 在模拟器/固件侧。
import { nextTick, ref } from 'vue'

import { request } from '@/api/client'

interface ChatMessage {
  role: 'user' | 'assistant'
  content: string
}

const messages = ref<ChatMessage[]>([])
const input = ref('')
const busy = ref(false)
const provider = ref('mock/dummy')
const providers = ref<string[]>([])

// provider 列表（下拉选项 = <id>/<model> 简化）
request<{ providers: string[] }>('/api/v1/chat/providers')
  .then((r) => {
    providers.value = r.providers
    if (r.providers.length && r.providers[0] !== 'mock') {
      provider.value = `${r.providers[0]}/default-model`
    }
  })
  .catch(() => {})

const listEl = ref<HTMLElement | null>(null)
async function scrollBottom() {
  await nextTick()
  listEl.value?.scrollTo({ top: listEl.value.scrollHeight })
}

async function send() {
  const text = input.value.trim()
  if (!text || busy.value) return
  input.value = ''
  messages.value.push({ role: 'user', content: text })
  scrollBottom()

  busy.value = true
  try {
    // 流式：fetch ReadableStream 手动解析 SSE
    const headers: Record<string, string> = { 'Content-Type': 'application/json' }
    const token = localStorage.getItem('bridge_token')
    if (token) headers.Authorization = `Bearer ${token}`

    const resp = await fetch('/api/v1/chat/completions', {
      method: 'POST',
      headers,
      body: JSON.stringify({
        model: provider.value,
        stream: true,
        messages: [...messages.value],
      }),
    })
    if (!resp.ok || !resp.body) {
      const errBody = await resp.json().catch(() => null)
      throw new Error(errBody?.message ?? `HTTP ${resp.status}`)
    }

    // 先建一个空的 assistant 气泡，逐字追加
    const aiMsg = ref({ role: 'assistant', content: '' } as ChatMessage)
    messages.value.push(aiMsg.value)

    const reader = resp.body.getReader()
    const decoder = new TextDecoder()
    let buf = ''
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      buf += decoder.decode(value, { stream: true })
      // SSE 帧以空行分隔
      const frames = buf.split('\n\n')
      buf = frames.pop() ?? ''
      for (const frame of frames) {
        const line = frame.trim()
        if (!line.startsWith('data: ')) continue
        const payload = line.slice(6)
        if (payload === '[DONE]') continue
        try {
          const chunk = JSON.parse(payload) as {
            choices?: { delta?: { content?: string } }[]
          }
          const delta = chunk.choices?.[0]?.delta?.content
          if (delta) {
            aiMsg.value.content += delta
            messages.value[messages.value.length - 1] = { ...aiMsg.value }
            scrollBottom()
          }
        } catch {
          // 忽略残帧
        }
      }
    }
  } catch (e) {
    messages.value.push({
      role: 'assistant',
      content: `⚠ 请求失败：${e instanceof Error ? e.message : String(e)}`,
    })
  } finally {
    busy.value = false
    scrollBottom()
  }
}
</script>

<template>
  <h1 class="page-title">Bigfish Chat</h1>
  <p class="page-sub">Bridge Model Router 链路调试（当前 upstream：
    <select v-model="provider" class="input !w-auto inline-block !py-1">
      <option v-for="p in providers" :key="p" :value="`${p}/`">{{ p }}（默认模型）</option>
      <option value="mock/">mock</option>
    </select>）
  </p>

  <div ref="listEl" class="card mb-4 h-96 overflow-y-auto">
    <div v-if="messages.length === 0" class="py-10 text-center text-sm text-dim">
      输入消息测试 Bridge 聊天链路（当前为 Mock upstream，回复为 echo 风格）
    </div>
    <div
      v-for="(m, i) in messages"
      :key="i"
      class="mb-3 flex"
      :class="m.role === 'user' ? 'justify-end' : 'justify-start'"
    >
      <div
        class="max-w-[80%] whitespace-pre-wrap rounded-2xl px-3.5 py-2 text-sm"
        :class="m.role === 'user' ? 'bg-accent text-white' : 'bg-cardhi text-txt'"
      >
        {{ m.content }}
      </div>
    </div>
  </div>

  <div class="flex gap-2">
    <input
      v-model="input"
      class="input flex-1"
      placeholder="输入消息…"
      :disabled="busy"
      @keydown.enter="send"
    />
    <button class="btn" :disabled="busy || !input.trim()" @click="send">
      {{ busy ? '生成中…' : '发送' }}
    </button>
  </div>
</template>
