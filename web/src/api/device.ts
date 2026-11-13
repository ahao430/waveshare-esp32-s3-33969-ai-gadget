// 设备本地 Web API 客户端（PLAN §9.4）。
//
// ⚠️ Phase 2 为 Mock 实现（web/src/api/device.mock.ts）：
// 设备 HTTP Server 属固件 Phase 1+/6 范围，尚未存在。
// 接口签名按 PLAN §9.4 预定，届时仅替换 useDeviceApi() 返回的实现，
// 页面代码不动。
import { useDeviceApi } from './device.mock'

export interface DeviceStatus {
  model: string
  firmware: string
  uptime_s: number
  wifi_ssid: string | null
  ip: string | null
  battery_pct: number
  display_brightness: number
}

export interface DeviceConfig {
  display_timeout_s: number
  brightness: number
  volume: number
}

export interface WifiNetwork {
  ssid: string
  rssi: number
  auth: 'open' | 'wpa2' | 'wpa3'
}

export type WifiScanState = 'idle' | 'scanning' | 'done' | 'error'

export interface ProxyConfig {
  mode: 'direct' | 'http_connect'
  host: string
  port: number
  username: string
  password: string // 仅写入不回显；mock 层同样不回显
}

export type ProxyTestResult =
  | { state: 'ok'; latency_ms: number }
  | { state: 'fail'; reason: string }

export interface DeviceApi {
  status(): Promise<DeviceStatus>
  config(): Promise<DeviceConfig>
  updateConfig(patch: Partial<DeviceConfig>): Promise<DeviceConfig>
  wifiScan(): Promise<WifiNetwork[]>
  wifiConnect(ssid: string, password: string): Promise<{ ok: boolean; ip?: string; error?: string }>
  proxy(): Promise<ProxyConfig>
  updateProxy(cfg: ProxyConfig): Promise<ProxyConfig>
  proxyTest(): Promise<ProxyTestResult>
  /** SoftAP 配网入口（二维码内容：仅 URL，不含任何秘密，PLAN §9.3） */
  provisionInfo(): Promise<{ ssid: string; setup_url: string }>
}

export const deviceApi: DeviceApi = useDeviceApi()
