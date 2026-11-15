// stream.go — Router 的流式调用（语音链路用：边生成边合成）。
//
// 与 Complete 同样的 provider 解析，但把增量文本逐段回调出去。三种
// upstream 的分支：
//
//	mock      直接吐整段（本地生成，无网络）
//	astrbot   AstrBot OpenAPI 的 SSE（plain 增量事件；见 astrbot.go）
//	openai    OpenAI 兼容 /chat/completions + stream=true
package ai

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
)

// CompleteStream 流式调用：每收到一段增量文本调用 onDelta。
// onDelta 返回错误表示调用方不再需要后续内容（例如语音已够长），
// 此时立即返回该错误以外的 nil（视为"提前结束"）。
func (r *Router) CompleteStream(ctx context.Context, model string,
	messages []ChatMessage, onDelta func(string) error) error {
	u, modelName, err := r.resolve(model)
	if err != nil {
		return err
	}
	if u.ID == "mock" {
		return onDelta(mockReplyText(messages))
	}
	if u.Kind == "astrbot" {
		return astrbotStream(ctx, u, messages, onDelta)
	}
	return r.openAIStream(ctx, u, modelName, messages, onDelta)
}

// ErrStreamDone 由 onDelta 返回，表示"够了，停止读取（不是错误）"。
var ErrStreamDone = fmt.Errorf("stream_done")

// IsStreamDone 供调用方判断是否提前结束（不是错误）。
func IsStreamDone(err error) bool { return err == ErrStreamDone }

func (r *Router) openAIStream(ctx context.Context, u Upstream, modelName string,
	messages []ChatMessage, onDelta func(string) error) error {
	body, err := json.Marshal(map[string]any{
		"model":    modelName,
		"messages": messages,
		"stream":   true,
	})
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost,
		strings.TrimRight(u.BaseURL, "/")+"/chat/completions", bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+u.APIKey)

	resp, err := r.hc.Do(req)
	if err != nil {
		return fmt.Errorf("upstream: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("upstream http_status_%d", resp.StatusCode)
	}

	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 64*1024), maxResponseBytesAI*4)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		p := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if p == "" || p == "[DONE]" {
			continue
		}
		var chunk struct {
			Choices []struct {
				Delta struct {
					Content string `json:"content"`
				} `json:"delta"`
			} `json:"choices"`
		}
		if json.Unmarshal([]byte(p), &chunk) != nil || len(chunk.Choices) == 0 {
			continue
		}
		if d := chunk.Choices[0].Delta.Content; d != "" {
			if err := onDelta(d); err != nil {
				return err
			}
		}
	}
	return sc.Err()
}
