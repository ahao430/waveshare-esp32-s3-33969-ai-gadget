// Komari 监控 API 客户端（Bridge /api/v1/komari/nodes）。
import { request } from './client'

export interface KomariServer {
  uuid: string
  name: string
  online: boolean
  cpu_pct?: number
  mem_pct?: number
  disk_pct?: number
  net_up_bps?: number
  net_down_bps?: number
  uptime_s?: number
  last_report: string
}

export interface KomariView {
  snapshot?: { fetched_at: string; source: string; servers: KomariServer[] }
  stale: boolean
  last_error?: string
  fetched_at: string
}

export const komariApi = {
  nodes: (force = false) =>
    request<KomariView>(`/api/v1/komari/nodes${force ? '?refresh=1' : ''}`),
}

export function fmtUptime(s?: number): string {
  if (!s) return '—'
  const d = Math.floor(s / 86400)
  const h = Math.floor((s % 86400) / 3600)
  if (d > 0) return `${d}天${h}时`
  const m = Math.floor((s % 3600) / 60)
  return `${h}时${m}分`
}

export function fmtBps(bps?: number): string {
  if (bps == null) return '—'
  if (bps >= 1_000_000) return (bps / 1_000_000).toFixed(1) + ' MB/s'
  if (bps >= 1000) return (bps / 1000).toFixed(0) + ' KB/s'
  return bps + ' B/s'
}
