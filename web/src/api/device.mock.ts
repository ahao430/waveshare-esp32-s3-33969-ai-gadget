// 设备 API 的 Mock 实现：内存状态 + 模拟延迟。
// 与真实实现的差异只在网络层；接口与语义一致（见 device.ts）。
import type {
  DeviceApi,
  DeviceConfig,
  DeviceStatus,
  ProxyConfig,
  ProxyTestResult,
  WifiNetwork,
} from './device'

const delay = (ms: number) => new Promise((r) => setTimeout(r, ms))

const state = {
  status: {
    model: 'Waveshare ESP32-S3-Touch-AMOLED-2.16 (33969)',
    firmware: '0.1.0-phase2-mock',
    uptime_s: 3720,
    wifi_ssid: 'Home-WiFi-5G',
    ip: '192.168.1.42',
    battery_pct: 82,
    display_brightness: 80,
  } as DeviceStatus,
  config: {
    display_timeout_s: 60,
    brightness: 80,
    volume: 55,
  } as DeviceConfig,
  wifi: [
    { ssid: 'Home-WiFi-5G', rssi: -41, auth: 'wpa2' },
    { ssid: 'Home-WiFi', rssi: -58, auth: 'wpa2' },
    { ssid: 'Office-Guest', rssi: -66, auth: 'open' },
    { ssid: 'Neighbor_2.4G', rssi: -78, auth: 'wpa2' },
  ] as WifiNetwork[],
  proxy: {
    mode: 'direct',
    host: '',
    port: 0,
    username: '',
    password: '',
  } as ProxyConfig,
}

export function useDeviceApi(): DeviceApi {
  return {
    async status() {
      await delay(150)
      state.status.uptime_s += 30
      return { ...state.status }
    },

    async config() {
      await delay(100)
      return { ...state.config }
    },

    async updateConfig(patch) {
      await delay(200)
      state.config = { ...state.config, ...patch }
      return { ...state.config }
    },

    async wifiScan() {
      await delay(1200) // ESP32 扫描实际耗时约 1~2s
      return state.wifi.map((w) => ({ ...w })).sort((a, b) => b.rssi - a.rssi)
    },

    async wifiConnect(ssid, password) {
      await delay(1800)
      const known = state.wifi.find((w) => w.ssid === ssid)
      if (!known) return { ok: false, error: 'not_found' }
      if (known.auth !== 'open' && password.length < 8) {
        return { ok: false, error: 'bad_password' } // mock 校验：≥8 位
      }
      state.status.wifi_ssid = ssid
      state.status.ip = '192.168.1.' + (40 + Math.floor(Math.random() * 20))
      return { ok: true, ip: state.status.ip }
    },

    async proxy() {
      await delay(100)
      const { password: _pw, ...rest } = state.proxy // 密码不回显
      return { ...rest, password: '' } as ProxyConfig
    },

    async updateProxy(cfg) {
      await delay(200)
      // 空密码 = 保留旧密码（与设备侧语义一致）
      state.proxy = { ...state.proxy, ...cfg, password: cfg.password || state.proxy.password }
      return this.proxy()
    },

    async proxyTest(): Promise<ProxyTestResult> {
      await delay(1500)
      if (state.proxy.mode === 'direct') {
        return { state: 'ok', latency_ms: 42 }
      }
      if (!state.proxy.host || !state.proxy.port) {
        return { state: 'fail', reason: 'empty_host_or_port' }
      }
      return Math.random() < 0.85
        ? { state: 'ok', latency_ms: 120 + Math.floor(Math.random() * 200) }
        : { state: 'fail', reason: 'connect_timeout' }
    },

    async provisionInfo() {
      await delay(100)
      return { ssid: 'AI-Gadget-Setup', setup_url: 'http://192.168.4.1' }
    },
  }
}
