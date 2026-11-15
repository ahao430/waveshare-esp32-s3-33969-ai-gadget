// asr_deepseek.go — DeepSeek 语音识别（全家桶 API，2026）。
//
// POST {base}/v1/tools/asr（multipart，Bearer Key）：
//   file=<16bit PCM WAV 或 Ogg Opus，内容嗅探>
//   响应: {"request_id":"...","text":"...","duration_ms":...}
// 约束：≤8MiB、≤120s；采样率 8k~48k 均可；无语言参数（自动识别）。
package voice

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"strings"
	"time"
)

const deepseekDefaultBase = "https://api.deepseek.com"

func deepseekBase() string {
	if b := os.Getenv("DEEPSEEK_BASE_URL"); b != "" {
		return strings.TrimRight(b, "/")
	}
	return deepseekDefaultBase
}

func deepseekTranscribe(ctx context.Context, apiKey string, pcmL16 []byte) (string, error) {
	if apiKey == "" {
		return "", fmt.Errorf("deepseek api key not configured")
	}
	wav := wrapWavBytes(pcmL16)

	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	fw, err := mw.CreateFormFile("file", "audio.wav")
	if err != nil {
		return "", err
	}
	if _, err := fw.Write(wav); err != nil {
		return "", err
	}
	if err := mw.Close(); err != nil {
		return "", err
	}

	cctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(cctx, http.MethodPost,
		deepseekBase()+"/v1/tools/asr", &body)
	if err != nil {
		return "", err
	}
	req.Header.Set("Authorization", "Bearer "+apiKey)
	req.Header.Set("Content-Type", mw.FormDataContentType())

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	rb, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("deepseek asr http %d: %.200s", resp.StatusCode, string(rb))
	}
	var out struct {
		Text string `json:"text"`
	}
	if err := json.Unmarshal(rb, &out); err != nil {
		return "", fmt.Errorf("deepseek asr decode: %w", err)
	}
	return strings.TrimSpace(out.Text), nil
}
