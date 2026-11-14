package ai

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
)

// ChatMessage 是 Complete 的消息对。
type ChatMessage struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

// Complete 非流式调用（供 voice 等内部域复用，不走 HTTP handler）：
// model 含 provider 前缀（空串=默认 provider）；返回回复全文。
// mock upstream 走本地生成（mockReplyText），语义与代理路径一致。
func (r *Router) Complete(ctx context.Context, model string, messages []ChatMessage) (string, error) {
	u, modelName, err := r.resolve(model)
	if err != nil {
		return "", err
	}

	if u.ID == "mock" {
		return mockReplyText(messages), nil
	}
	if u.Kind == "astrbot" {
		return astrbotComplete(ctx, u, messages)
	}

	body, err := json.Marshal(map[string]any{
		"model":    modelName,
		"messages": messages,
		"stream":   false,
	})
	if err != nil {
		return "", err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost,
		strings.TrimRight(u.BaseURL, "/")+"/chat/completions", bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+u.APIKey)

	resp, err := r.hc.Do(req)
	if err != nil {
		return "", fmt.Errorf("upstream: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
		return "", fmt.Errorf("upstream http_status_%d", resp.StatusCode)
	}

	var out struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, maxResponseBytesAI)).Decode(&out); err != nil {
		return "", fmt.Errorf("bad_upstream_json: %v", err)
	}
	if len(out.Choices) == 0 {
		return "", fmt.Errorf("empty_choices")
	}
	return out.Choices[0].Message.Content, nil
}

// maxResponseBytesAI 与 quota 域一致的上限。
const maxResponseBytesAI = 1 << 20

// mockReplyText：mock upstream 的回复生成（serveMockChat 与 Complete 共用）。
func mockReplyText(messages []ChatMessage) string {
	last := ""
	for i := len(messages) - 1; i >= 0; i-- {
		if messages[i].Role == "user" {
			last = messages[i].Content
			break
		}
	}
	return fmt.Sprintf("[mock] 你说：%s。这是 Bridge Mock 的回复（模型路由链路验证用）。",
		truncateRunes(last, 80))
}

// toChatMessages 把 serveMockChat 的匿名结构消息转成 ChatMessage。
func toChatMessages(src []struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}) []ChatMessage {
	out := make([]ChatMessage, 0, len(src))
	for _, m := range src {
		out = append(out, ChatMessage{Role: m.Role, Content: m.Content})
	}
	return out
}
