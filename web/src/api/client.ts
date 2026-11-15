// Bridge API 客户端。开发环境走 Vite 代理（/api → 127.0.0.1:8081），
// 生产环境可设 VITE_BRIDGE_URL 指向局域网 Bridge。
export const BRIDGE_URL = import.meta.env.VITE_BRIDGE_URL ?? ''

export class ApiError extends Error {
  constructor(
    public status: number,
    public kind: string,
    message: string,
  ) {
    super(message)
  }
}

export async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const headers: Record<string, string> = { Accept: 'application/json' }
  // Bridge 启用 BRIDGE_TOKEN 后需要 Bearer 认证；Token 只存 localStorage，
  // 永不写入 URL/日志（PLAN §19）。
  const token = localStorage.getItem('bridge_token')
  if (token) headers.Authorization = `Bearer ${token}`

  const resp = await fetch(`${BRIDGE_URL}${path}`, { headers, ...init })
  const body = (await resp.json().catch(() => null)) as
    | (T & { error?: string; message?: string })
    | null
  if (!resp.ok) {
    throw new ApiError(resp.status, body?.error ?? 'http_error', body?.message ?? resp.statusText)
  }
  return body as T
}

// ---- 类型与 Bridge internal/quota 模型保持一致 ----

export interface ProviderCapabilities {
  trust: 'A' | 'B' | 'C' | 'D'
  can_refresh: boolean
  can_prewarm: boolean
  experimental: boolean
}

export interface ProviderInfo {
  id: string
  display_name: string
  capabilities: ProviderCapabilities
}

export interface QuotaWindow {
  id: string
  label: string
  window_seconds?: number
  used_percent?: number
  remaining_pct?: number
  reset_at?: string
  unit?: string
  source_field?: string
}

export interface QuotaSnapshot {
  provider_id: string
  account_id: string
  plan_name?: string
  fetched_at: string
  status: string
  windows: QuotaWindow[]
}

export interface StoredSnapshot {
  snapshot?: QuotaSnapshot
  stale: boolean
  last_error?: string
  fetched_at: string
}

export interface PrewarmResult {
  idempotency_key: string
  status: 'executed' | 'failed' | 'skipped_low_quota' | 'skipped_cooldown' | 'duplicate'
  message?: string
  model?: string
  light_http_status?: number
  duration_ms?: number
  cooldown_remain_s?: number
  before?: QuotaSnapshot
  after?: QuotaSnapshot
  created_at: string
}

export const api = {
  providers: () => request<{ providers: ProviderInfo[] }>('/api/v1/quota/providers'),
  snapshot: (id: string) => request<StoredSnapshot>(`/api/v1/quota/${id}/snapshot`),
  refresh: (id: string) =>
    request<StoredSnapshot>(`/api/v1/quota/${id}/refresh`, { method: 'POST' }),
  deviceStatus: () => request<Record<string, unknown>>('/api/v1/device/status'),
}
