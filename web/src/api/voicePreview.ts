// 音色试听：POST /api/v1/voice/preview → WAV，用 <audio> 直接播放。
// 只读不落库：不保存任何配置；同一时刻只播一个（切换音色会自动停掉
// 上一个）。返回的 Promise 在播放结束/被打断时 resolve，失败时 reject
// （message 用 Bridge 回传的真实原因，如"没配 Key"）。
import { BRIDGE_URL } from './client'

let current: { audio: HTMLAudioElement; url: string } | null = null
let resolveCurrent: (() => void) | null = null

function stopCurrent() {
  if (current) {
    const { audio, url } = current
    current = null
    audio.onended = null
    audio.onerror = null
    try {
      audio.pause()
    } catch {
      /* 已结束 */
    }
    URL.revokeObjectURL(url)
  }
  resolveCurrent?.()
  resolveCurrent = null
}

export function stopVoicePreview() {
  stopCurrent()
}

export async function playVoicePreview(provider: string, voice: string): Promise<void> {
  stopCurrent()

  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    Accept: 'audio/wav',
  }
  // 与 api/client 相同的 Bearer 认证（Token 只存 localStorage）
  const token = localStorage.getItem('bridge_token')
  if (token) headers.Authorization = `Bearer ${token}`

  const resp = await fetch(`${BRIDGE_URL}/api/v1/voice/preview`, {
    method: 'POST',
    headers,
    body: JSON.stringify({ provider, voice }),
  })
  if (!resp.ok) {
    const body = (await resp.json().catch(() => null)) as
      | { error?: string; message?: string }
      | null
    throw new Error(body?.message ?? body?.error ?? resp.statusText)
  }
  const blob = await resp.blob()
  const url = URL.createObjectURL(blob)
  const audio = new Audio(url)
  current = { audio, url }
  const ended = new Promise<void>((resolve) => {
    resolveCurrent = resolve
  })
  audio.onended = stopCurrent
  audio.onerror = stopCurrent
  try {
    await audio.play()
  } catch {
    stopCurrent()
    throw new Error('浏览器拦截了播放，请再点一次试听')
  }
  await ended
}
