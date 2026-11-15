// asr_zhipu.go — 智谱 GLM-ASR 语音转写（PLAN §5.3 ASR 引擎）。
//
// POST {base}/paas/v4/audio/transcriptions（Bearer 认证，与智谱其他 API
// 同一 Key 体系——env ZHIPU_API_KEY 或 Web 配置的加密凭据）：
//
//	multipart: model=glm-asr-2512, stream=false, file=<wav>
//	响应: {"text": "..."}
//
// 限制：音频 ≤30s（设备采集上限 8s）、文件 ≤25MB，均在预算内。
package voice

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"strings"
	"time"
)

const zhipuASRModel = "glm-asr-2512"

// zhipuTranscribe 把 16k/16bit/mono PCM L16 字节流转写为文本。
// baseURL 空 = 默认国内站；传入时应为去掉尾部 /paas/v4 的根（与
// zhipu provider 的 ZHIPU_BASE_URL 语义一致）。
func zhipuTranscribe(ctx context.Context, apiKey, baseURL string, pcmL16 []byte) (string, error) {
	if apiKey == "" {
		return "", fmt.Errorf("zhipu api key not configured")
	}
	base := strings.TrimSuffix(baseURL, "/")
	if base == "" {
		base = "https://open.bigmodel.cn/api"
	}
	url := base + "/paas/v4/audio/transcriptions"

	wav := wrapWavBytes(pcmL16)

	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	_ = mw.WriteField("model", zhipuASRModel)
	_ = mw.WriteField("stream", "false")
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

	cctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(cctx, http.MethodPost, url, &body)
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
		return "", fmt.Errorf("zhipu asr http %d: %.200s", resp.StatusCode, string(rb))
	}

	var out struct {
		Text string `json:"text"`
	}
	if err := json.Unmarshal(rb, &out); err != nil {
		return "", fmt.Errorf("zhipu asr decode: %w", err)
	}
	return out.Text, nil
}

// wrapWavBytes PCM L16 → WAV（16k/16bit/mono，44B 标准头）。
// voice.go 的 wrapWav 接收 []int16；这里直接按字节打包避免一次转换。
func wrapWavBytes(pcmL16 []byte) []byte {
	n := len(pcmL16) &^ 1 // 奇数字节防御性对齐
	buf := make([]byte, 44+n)
	copy(buf[0:], "RIFF")
	putLE32(buf[4:], uint32(36+n))
	copy(buf[8:], "WAVE")
	copy(buf[12:], "fmt ")
	putLE32(buf[16:], 16) // fmt chunk 大小
	buf[20] = 1           // PCM
	buf[22] = 1           // mono
	putLE32(buf[24:], 16000)
	putLE32(buf[28:], 32000) // byte/s = 16000×2
	buf[32] = 2              // block align
	buf[34] = 16             // bits
	copy(buf[36:], "data")
	putLE32(buf[40:], uint32(n))
	copy(buf[44:], pcmL16[:n])
	return buf
}

func putLE32(b []byte, v uint32) {
	b[0] = byte(v)
	b[1] = byte(v >> 8)
	b[2] = byte(v >> 16)
	b[3] = byte(v >> 24)
}
