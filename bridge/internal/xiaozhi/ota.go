// ota.go — OTA 注册与激活（docs/XIAOZHI_PROTOCOL.md §5.1）。
//
//	POST <ota>/           Device-Id/Client-Id/User-Agent/Activation-Version:1
//	                      → 未绑定：activation{code,challenge} + websocket{url,token}
//	POST <ota>/activate   同头，body {} → 202 等待绑定 / 200 已绑定
//
// 无 HMAC 签名（那是 Activation-Version 2 / 硬件序列号设备的路径）。
// 官方响应可能同时带 mqtt 块——PTT 场景忽略，只走 websocket。
package xiaozhi

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

// otaRequest OTA 系统信息上报体（最小必填集，字段名对齐上游 ota.cc）。
type otaRequest struct {
	Version    int    `json:"version"` // 请求布局版本（非固件版本）
	Language   string `json:"language"`
	MacAddress string `json:"mac_address"`
	UUID       string `json:"uuid"` // = Client-Id
	Chip       struct {
		Model    int `json:"model"`
		Cores    int `json:"cores"`
		Revision int `json:"revision"`
	} `json:"chip_info"`
	Application struct {
		Name        string `json:"name"`
		Version     string `json:"version"`
		IDFVersion  string `json:"idf_version"`
		CompileTime string `json:"compile_time"`
	} `json:"application"`
	Board struct {
		Type    string `json:"type"`
		Name    string `json:"name"`
		MAC     string `json:"mac"`
		SSID    string `json:"ssid"`
		RSSI    int    `json:"rssi"`
		Channel int    `json:"channel"`
	} `json:"board"`
	Protocol struct {
		Features map[string]bool `json:"features"`
	} `json:"protocol"`
}

// otaResponse 只解析我们用的字段（服务端可能多给）。
type otaResponse struct {
	Websocket struct {
		URL    string `json:"url"`
		Token  string `json:"token"`
	} `json:"websocket"`
	Activation *struct {
		Code      string `json:"code"`
		Message   string `json:"message"`
		Challenge string `json:"challenge"`
	} `json:"activation"`
	ServerTime struct {
		Timestamp int64 `json:"timestamp"`
	} `json:"server_time"`
}

// otaHTTP 带 identity 头的 POST。
func otaHTTP(ctx context.Context, url, deviceID, clientID, body string,
	timeout time.Duration) (*http.Response, []byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url,
		bytes.NewReader([]byte(body)))
	if err != nil {
		return nil, nil, err
	}
	req.Header.Set("Device-Id", deviceID)
	req.Header.Set("Client-Id", clientID)
	req.Header.Set("User-Agent", uaBoard+"/"+uaVersion)
	req.Header.Set("Activation-Version", "1")
	req.Header.Set("Accept-Language", "zh-CN")
	req.Header.Set("Content-Type", "application/json")

	hc := &http.Client{
		Timeout: timeout,
		Transport: &http.Transport{Proxy: http.ProxyFromEnvironment},
	}
	resp, err := hc.Do(req)
	if err != nil {
		return nil, nil, err
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	return resp, raw, nil
}

// buildOTARequest 组系统信息上报体。
func buildOTARequest(deviceID, clientID string) string {
	var r otaRequest
	r.Version = 2
	r.Language = "zh-CN"
	r.MacAddress = deviceID
	r.UUID = clientID
	r.Chip.Model, r.Chip.Cores, r.Chip.Revision = 9, 2, 2 // esp32s3
	r.Application.Name = uaBoard
	r.Application.Version = uaVersion
	r.Application.IDFVersion = "v6.0.3"
	r.Application.CompileTime = "Nov 16 2026T00:00:00Z"
	r.Board.Type, r.Board.Name = uaBoard, uaBoard
	r.Board.MAC = deviceID
	r.Board.RSSI, r.Board.Channel = -50, 6
	// 不实现 MCP/UDP——别 advertise，否则服务端会下发 mcp 初始化
	r.Protocol.Features = map[string]bool{
		"mcp": false, "mcp_20260728": false,
		"udp_probe": false, "aec": false, "glyph_push": false,
	}
	b, _ := json.Marshal(r)
	return string(b)
}

// OTA 执行一次注册查询。返回 websocket url/token（无论绑定与否服务端都
// 会给）与激活码（未绑定时非空）。
func OTA(ctx context.Context, otaURL, deviceID, clientID string) (wsURL, token, activationCode string, err error) {
	if otaURL == "" {
		otaURL = DefaultOTAURL
	}
	resp, raw, err := otaHTTP(ctx, otaURL, deviceID, clientID, buildOTARequest(deviceID, clientID), 15*time.Second)
	if err != nil {
		return "", "", "", fmt.Errorf("xiaozhi ota: %w", err)
	}
	if resp.StatusCode != http.StatusOK {
		return "", "", "", fmt.Errorf("xiaozhi ota http %d: %.200s", resp.StatusCode, raw)
	}
	var out otaResponse
	if err := json.Unmarshal(raw, &out); err != nil {
		return "", "", "", fmt.Errorf("xiaozhi ota json: %w", err)
	}
	if out.Websocket.URL == "" {
		return "", "", "", fmt.Errorf("xiaozhi ota: response has no websocket.url")
	}
	if out.Activation != nil {
		activationCode = out.Activation.Code
	}
	return out.Websocket.URL, out.Websocket.Token, activationCode, nil
}

// Activate 查询绑定状态：202=等待用户在控制台输入激活码；200=已绑定。
func Activate(ctx context.Context, otaURL, deviceID, clientID string) (bound bool, err error) {
	if otaURL == "" {
		otaURL = DefaultOTAURL
	}
	resp, raw, err := otaHTTP(ctx, ensureTrailing(otaURL)+"activate", deviceID, clientID, "{}", 10*time.Second)
	if err != nil {
		return false, fmt.Errorf("xiaozhi activate: %w", err)
	}
	switch resp.StatusCode {
	case http.StatusOK:
		return true, nil
	case http.StatusAccepted:
		return false, nil
	default:
		return false, fmt.Errorf("xiaozhi activate http %d: %.200s", resp.StatusCode, raw)
	}
}

func ensureTrailing(u string) string {
	if len(u) > 0 && u[len(u)-1] != '/' {
		return u + "/"
	}
	return u
}

// NewDeviceID 生成本地管理位 MAC（02:xx…，单播），作为稳定设备身份。
func NewDeviceID() string {
	var b [5]byte
	_, _ = rand.Read(b[:])
	s := hex.EncodeToString(b[:])
	return "02:" + s[0:2] + ":" + s[2:4] + ":" + s[4:6] + ":" + s[6:8] + ":" + s[8:10]
}
