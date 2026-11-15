// asr_minimax.go — MiniMax 语音识别（asr-1.0）。
//
// POST {base}/v1/speech_to_text（multipart，Bearer API Key）：
//   model=asr-1.0, file=<wav>, response_format=json
//   响应: {"text": "...", "duration": ...}
// base 默认 https://api.minimax.chat（国内站；国际站 api.minimaxi.com
// 可用 MINIMAX_BASE_URL 覆盖）。单次 ≤500s / ≤50MB，PCM 裸流不支持
//（本模块统一封 WAV）。
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

const minimaxDefaultBase = "https://api.minimax.chat"

func minimaxBase() string {
	if b := os.Getenv("MINIMAX_BASE_URL"); b != "" {
		return strings.TrimRight(b, "/")
	}
	return minimaxDefaultBase
}

func minimaxTranscribe(ctx context.Context, apiKey string, pcmL16 []byte) (string, error) {
	if apiKey == "" {
		return "", fmt.Errorf("minimax api key not configured")
	}
	wav := wrapWavBytes(pcmL16)

	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	_ = mw.WriteField("model", "asr-1.0")
	_ = mw.WriteField("response_format", "json")
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
		minimaxBase()+"/v1/speech_to_text", &body)
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
		return "", fmt.Errorf("minimax asr http %d: %.200s", resp.StatusCode, string(rb))
	}
	var out struct {
		Text string `json:"text"`
	}
	if err := json.Unmarshal(rb, &out); err != nil {
		return "", fmt.Errorf("minimax asr decode: %w", err)
	}
	return strings.TrimSpace(out.Text), nil
}
