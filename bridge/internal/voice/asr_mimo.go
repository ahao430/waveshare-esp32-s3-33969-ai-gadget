// asr_mimo.go — 小米 MiMo 语音识别（mimo-v2.5-asr）。
//
// OpenAI 兼容接口：POST {base}/chat/completions
//   model=mimo-v2.5-asr
//   messages[0].content = [{type:input_audio, input_audio:{data:"data:audio/wav;base64,..."}}]
//   extra: asr_options.language = auto|zh|en
// base 默认 https://api.xiaomimimo.com/v1（MIMO_BASE_URL 可覆盖）。
// 限制：仅 wav/mp3，Base64 后 ≤10MB（设备语音 ≤8s 远低于上限）。
package voice

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

const mimoDefaultBase = "https://api.xiaomimimo.com/v1"

func mimoBase() string {
	if b := os.Getenv("MIMO_BASE_URL"); b != "" {
		return strings.TrimRight(b, "/")
	}
	return mimoDefaultBase
}

func mimoTranscribe(ctx context.Context, apiKey string, pcmL16 []byte) (string, error) {
	if apiKey == "" {
		return "", fmt.Errorf("mimo api key not configured")
	}
	wav := wrapWavBytes(pcmL16)
	dataURL := "data:audio/wav;base64," + base64.StdEncoding.EncodeToString(wav)
	lang := os.Getenv("MIMO_ASR_LANGUAGE")
	if lang == "" {
		lang = "auto"
	}

	payload := map[string]any{
		"model": "mimo-v2.5-asr",
		"messages": []map[string]any{{
			"role": "user",
			"content": []map[string]any{{
				"type":        "input_audio",
				"input_audio": map[string]any{"data": dataURL},
			}},
		}},
		"asr_options": map[string]any{"language": lang},
	}
	body, err := json.Marshal(payload)
	if err != nil {
		return "", err
	}

	cctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(cctx, http.MethodPost,
		mimoBase()+"/chat/completions", bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	req.Header.Set("Authorization", "Bearer "+apiKey)
	req.Header.Set("Content-Type", "application/json")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	rb, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("mimo asr http %d: %.200s", resp.StatusCode, string(rb))
	}

	// OpenAI chat.completion 结构：choices[0].message.content
	var out struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
	}
	if err := json.Unmarshal(rb, &out); err != nil {
		return "", fmt.Errorf("mimo asr decode: %w", err)
	}
	if len(out.Choices) == 0 {
		return "", fmt.Errorf("mimo asr: empty choices")
	}
	return strings.TrimSpace(out.Choices[0].Message.Content), nil
}
