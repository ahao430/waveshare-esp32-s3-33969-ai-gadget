// Bridge /api/v1/events WebSocket 客户端：自动重连 + Bearer/查询 Token。
// 事件只是"该刷新了"的信号与增量载荷；丢消息可接受（HTTP 拉全量兜底）。
export type EventsListener = (topic: string, data: unknown) => void

export interface EventsConnection {
  close(): void
}

export function connectEvents(
  onMessage: EventsListener,
  onStatus?: (status: 'open' | 'closed' | 'connecting') => void,
): EventsConnection {
  let closed = false
  let retryMs = 1000
  let ws: WebSocket | null = null

  const base = import.meta.env.VITE_BRIDGE_URL ?? '' // 开发态空串 = 同源走 vite 代理
  const proto = base.startsWith('https') ? 'wss' : 'ws'
  const origin = base ? base.replace(/^http/, '') : location.host
  const token = localStorage.getItem('bridge_token')
  const url = `${proto}://${origin}/api/v1/events${token ? `?token=${encodeURIComponent(token)}` : ''}`

  function dial() {
    if (closed) return
    onStatus?.('connecting')
    ws = new WebSocket(url)

    ws.onopen = () => {
      retryMs = 1000
      onStatus?.('open')
    }

    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data) as { topic: string; data?: unknown }
        onMessage(msg.topic, msg.data)
      } catch {
        // 忽略非 JSON 帧
      }
    }

    ws.onclose = () => {
      onStatus?.('closed')
      if (!closed) {
        setTimeout(dial, retryMs)
        retryMs = Math.min(retryMs * 2, 8000)
      }
    }
  }

  dial()

  return {
    close() {
      closed = true
      ws?.close()
    },
  }
}
