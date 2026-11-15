// Package xiaozhi — 官方 xiaozhi.me 服务的设备模拟（ADR-005 / PLAN §5.5）。
//
// Bridge 模拟一台 xiaozhi 设备：OTA 拿激活码 → 用户在 xiaozhi.me 控制台
// 绑定 → re-OTA 拿 WebSocket url/token → hello/listen PTT 回合（上行
// 16k Opus 60ms，下行 Opus 解码为 16k PCM 直通设备）。设备端零协议改动。
//
// 子文件：
//   ota.go     — OTA 注册与激活（api.tenclass.net/xiaozhi/ota/）
//   codec.go   — Opus 编解码（libopus via hraban/opus, -tags nolibopusfile）
//   session.go — WebSocket 一轮 PTT 会话
//   manager.go — 身份/凭据管理与 Manager（voice 集成入口）
package xiaozhi

// DefaultOTAURL 官方 OTA 端点（可被配置覆盖，例如自建服务端）。
const DefaultOTAURL = "https://api.tenclass.net/xiaozhi/ota/"

// 模拟设备上报的身份（固定值即可通过官方校验；不伪装具体 Waveshare
// 板型，board.type 用通用名避免与真实设备冲突）。
const (
	uaBoard   = "gadget-bridge"
	uaVersion = "0.1.0"
)
