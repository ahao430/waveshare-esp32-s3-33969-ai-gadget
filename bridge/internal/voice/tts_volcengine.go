// tts_volcengine.go — 火山引擎（豆包）语音合成，v1 HTTP 一次性接口。
//
// POST https://openspeech.bytedance.com/api/v1/tts（官方标注"不推荐、
// 优先 v3"，但 v1 仍在线且最简单——JSON 进、整段 base64 音频出；
// 小智 ESP32 等设备生态广泛使用）。设备按句合成，无需 v3 流式）：
//
//	Authorization: "Bearer;{token}"   ← 注意分号分隔，与常规 Bearer 不同
//	{
//	  "app":     {"appid": "...", "token": "access_token", "cluster": "volcano_tts"},
//	  "user":    {"uid": "gadget-bridge"},
//	  "audio":   {"voice_type": "BV001_streaming", "encoding": "pcm",
//	              "rate": 16000, "speed_ratio": 1.0},
//	  "request": {"reqid": "<uuid>", "text": "...", "operation": "query"}
//	}
//	响应 {"code": 3000, "data": "<base64 PCM 16k/16bit/mono>", ...}
//
// encoding=pcm 支持 rate 8000/16000（默认 24000）——直接要 16k，
// 零转码匹配设备约定。文本上限 1024 字节，按句调用天然满足。
//
// 凭据：火山"语音技术"控制台应用的 AppID + AccessToken，与 ASR 共用，
// 存 Bridge 凭据库 provider=volcengine，格式 "AppID:AccessToken"。
package voice

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

const (
	volcTTSURL  = "https://openspeech.bytedance.com/api/v1/tts"
	volcDefault = "BV001_streaming" // 通用女声（控制台免费授权音色之一）
)

// volcTTSBase 测试注入用（VOLC_TTS_BASE_URL 覆盖端点）。
func volcTTSBase() string {
	if b := os.Getenv("VOLC_TTS_BASE_URL"); b != "" {
		return strings.TrimRight(b, "/") + "/api/v1/tts"
	}
	return volcTTSURL
}

// volcReqID 生成请求唯一 ID（官方仅要求每次唯一，格式不限）。
func volcReqID() string {
	var b [8]byte
	_, _ = rand.Read(b[:])
	return hex.EncodeToString(b[:])
}

// volcCreds 拆 "AppID:AccessToken"（首个冒号分割；AppID 纯数字）。
func volcCreds(key string) (appID, accessToken string, err error) {
	appID, accessToken, ok := strings.Cut(strings.TrimSpace(key), ":")
	if !ok || appID == "" || accessToken == "" {
		return "", "", fmt.Errorf("volcengine key 格式应为 AppID:AccessToken")
	}
	return appID, accessToken, nil
}

// volcengineTTS 合成 text 为 16k/16bit/mono PCM。
// voice 空=BV001_streaming（免费音色；灿灿/炀炀等需控制台 0 元下单后可用）。
func volcengineTTS(ctx context.Context, apiKey, text, voiceType string) ([]int16, error) {
	appID, token, err := volcCreds(apiKey)
	if err != nil {
		return nil, err
	}
	if voiceType == "" {
		voiceType = volcDefault
	}
	payload, err := json.Marshal(map[string]any{
		"app":  map[string]any{"appid": appID, "token": token, "cluster": "volcano_tts"},
		"user": map[string]any{"uid": "gadget-bridge"},
		"audio": map[string]any{
			"voice_type":  voiceType,
			"encoding":    "pcm",
			"rate":        16000,
			"speed_ratio": 1.0,
		},
		"request": map[string]any{
			"reqid":     volcReqID(), // 每次合成必须唯一
			"text":      text,
			"operation": "query",
		},
	})
	if err != nil {
		return nil, err
	}

	cctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(cctx, http.MethodPost, volcTTSBase(), bytes.NewReader(payload))
	if err != nil {
		return nil, err
	}
	// 官方规定 "Bearer;" 分号分隔（token 同时放 app.token；鉴权以 Header 为准）
	req.Header.Set("Authorization", "Bearer;"+token)
	req.Header.Set("Content-Type", "application/json")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	rb, _ := io.ReadAll(io.LimitReader(resp.Body, 16<<20))
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("volc tts http %d: %.200s", resp.StatusCode, string(rb))
	}
	var out struct {
		Code    int    `json:"code"`
		Message string `json:"message"`
		Data    string `json:"data"`
	}
	if err := json.Unmarshal(rb, &out); err != nil {
		return nil, fmt.Errorf("volc tts decode: %w", err)
	}
	if out.Code != 3000 { // 3000 = success（官方错误码）
		return nil, fmt.Errorf("volc tts code %d: %.200s", out.Code, out.Message)
	}
	pcm, err := base64.StdEncoding.DecodeString(out.Data)
	if err != nil {
		return nil, fmt.Errorf("volc tts base64: %w", err)
	}
	if len(pcm) < 2 {
		return nil, fmt.Errorf("volc tts: empty audio")
	}
	n := len(pcm) / 2
	pcm16 := make([]int16, n)
	for i := 0; i < n; i++ {
		pcm16[i] = int16(binary.LittleEndian.Uint16(pcm[i*2:]))
	}
	return pcm16, nil // 已请求 rate=16000，直接返回
}
