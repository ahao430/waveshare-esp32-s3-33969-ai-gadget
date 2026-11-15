// tts_deepseek.go — DeepSeek 语音合成（全家桶 API，2026）。
//
// POST {base}/v1/tools/tts（JSON，Bearer Key）：
//   {"text": "...", "voice_id": "stella|tide", "format": "pcm",
//    "filter_markdown": true}
//   响应体为流式音频：format=pcm → 24kHz/16bit/单声道裸 PCM。
// 设备约定 16k/16bit/mono：本文件把 24k 降采样到 16k 后返回。
// 限制：单次 ≤10000 计费字符、≤30 分钟音频；不支持语速调节。
package voice

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

const deepseekTTSMaxBytes = 8 << 20 // 30s 语音 ≈ 1.4MB，8MB 足够且防异常

// deepseekTTS 合成 text 为 16k/16bit/mono PCM。
// voice 空=stella（默认女声）；tide=男声。
func deepseekTTS(ctx context.Context, apiKey, text, voiceID string) ([]int16, error) {
	if apiKey == "" {
		return nil, fmt.Errorf("deepseek api key not configured")
	}
	if voiceID == "" {
		voiceID = "stella"
	}
	payload, err := json.Marshal(map[string]any{
		"text":            text,
		"voice_id":        voiceID,
		"format":          "pcm", // 24kHz 裸 PCM，Bridge 侧重采样
		"filter_markdown": true,
	})
	if err != nil {
		return nil, err
	}

	cctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(cctx, http.MethodPost,
		deepseekBase()+"/v1/tools/tts", bytes.NewReader(payload))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+apiKey)
	req.Header.Set("Content-Type", "application/json")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		rb, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return nil, fmt.Errorf("deepseek tts http %d: %.200s", resp.StatusCode, string(rb))
	}

	raw, err := io.ReadAll(io.LimitReader(resp.Body, deepseekTTSMaxBytes))
	if err != nil {
		// 官方说明：音频开始返回后失败会中途断开——视为失败
		return nil, fmt.Errorf("deepseek tts read: %w", err)
	}
	if len(raw) < 2 {
		return nil, fmt.Errorf("deepseek tts: empty audio")
	}
	n := len(raw) / 2
	src := make([]int16, n)
	for i := 0; i < n; i++ {
		src[i] = int16(binary.LittleEndian.Uint16(raw[i*2:]))
	}
	return resampleTo16k(src, 24000), nil
}

// deepseekVoiceID 读取 DEEPSEEK_TTS_VOICE（stella/tide），默认 stella。
func deepseekVoiceID() string {
	if v := os.Getenv("DEEPSEEK_TTS_VOICE"); v != "" {
		return strings.TrimSpace(v)
	}
	return "stella"
}
