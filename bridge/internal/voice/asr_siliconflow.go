// asr_siliconflow.go — 硅基流动（SiliconFlow）语音转写（PLAN §5.3 ASR）。
//
// POST {base}/audio/transcriptions（multipart，Bearer Key）：
//
//	file=<WAV 16k/16bit/mono>, model=FunAudioLLM/SenseVoiceSmall
//	响应: {"text": "..."}
//
// 免注册模型、新账号有免费额度；作为智谱 GLM-ASR 余额不足时的替代
// 引擎（Key 存 Bridge 凭据库 provider=siliconflow）。
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

const siliconflowDefaultBase = "https://api.siliconflow.cn/v1"

func siliconflowModel() string {
	if m := os.Getenv("ASR_MODEL"); m != "" {
		return m
	}
	return "FunAudioLLM/SenseVoiceSmall" // 官方默认 ASR 模型
}

// siliconflowTranscribe PCM L16 → 文本。
func siliconflowTranscribe(ctx context.Context, apiKey, baseURL string, pcmL16 []byte) (string, error) {
	if apiKey == "" {
		return "", fmt.Errorf("siliconflow api key not configured")
	}
	base := strings.TrimRight(baseURL, "/")
	if base == "" {
		base = siliconflowDefaultBase
	}
	wav := wrapWavBytes(pcmL16)

	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	_ = mw.WriteField("model", siliconflowModel())
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
		base+"/audio/transcriptions", &body)
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
		return "", fmt.Errorf("siliconflow asr http %d: %.200s", resp.StatusCode, string(rb))
	}
	var out struct {
		Text string `json:"text"`
	}
	if err := json.Unmarshal(rb, &out); err != nil {
		return "", fmt.Errorf("siliconflow asr decode: %w", err)
	}
	return strings.TrimSpace(out.Text), nil
}
