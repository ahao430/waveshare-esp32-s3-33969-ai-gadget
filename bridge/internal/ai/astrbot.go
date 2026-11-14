// astrbot.go — AstrBot 机器人实例适配（大肥鱼对话对接，2026-11-14）。
// AstrBot（github.com/AstrBotDevs/AstrBot）部署在服务器（WebUI :6185），
// 对话端点 POST {base}/api/v1/chat（Bearer abk_ key，SSE 响应，body 需
// username）。Bridge 做格式适配：设备端 OpenAI chat/completions 请求 →
// AstrBot；AstrBot SSE → 设备端 OpenAI delta 格式——设备端解析器零改动。
package ai

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"
)

// AstrBot 对话请求参数（2026-11-14 v1）：
// username 固定 gadget 身份（stable app-controlled username，OpenAPI 要求）；
// session_id 每实例稳定（机器人侧保持上下文记忆）。
const (
	astrbotUsername  = "gadget"
	astrbotTimeout   = 120 * time.Second
	astrbotMaxChunks = 16 << 20
)

// serveAstrBotChat 把 OpenAI chat/completions 请求转 AstrBot 对话，
// SSE 流式回传（OpenAI delta 格式）。
func serveAstrBotChat(w http.ResponseWriter, body []byte, u Upstream, clientGone <-chan struct{}) error {
	// 1) OpenAI messages → AstrBot message：取最后一条 user 内容
	//    （多轮上下文由 AstrBot session 记忆维护，设备端历史仅展示）
	msg := lastUserContent(body)
	if msg == "" {
		msg = "你好"
	}

	// 2) 构造 AstrBot 请求
	payload, err := json.Marshal(map[string]any{
		"message":          msg,
		"username":         astrbotUsername,
		"session_id":       "gadget-bigfish-" + u.ID,
		"enable_streaming": true,
	})
	if err != nil {
		return err
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() {
		select {
		case <-clientGone:
			cancel()
		case <-ctx.Done():
		}
	}()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost,
		strings.TrimRight(u.BaseURL, "/")+"/api/v1/chat", bytesReader(payload))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "text/event-stream")
	req.Header.Set("Authorization", "Bearer "+u.APIKey)

	hc := &http.Client{Timeout: astrbotTimeout}
	resp, err := hc.Do(req)
	if err != nil {
		return fmt.Errorf("astrbot: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("astrbot_http_%d", resp.StatusCode)
	}

	// 3) SSE 透传响应头
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	flusher, _ := w.(http.Flusher)

	// 4) AstrBot SSE → OpenAI delta 转换（宽容解析：提取事件内文本）
	emit := func(text string) {
		if text == "" {
			return
		}
		chunk := map[string]any{
			"choices": []map[string]any{{"delta": map[string]any{"content": text}}},
		}
		b, _ := json.Marshal(chunk)
		fmt.Fprintf(w, "data: %s\n\n", b)
		if flusher != nil {
			flusher.Flush()
		}
	}

	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 64*1024), astrbotMaxChunks)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		payload := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if payload == "" || payload == "[DONE]" {
			continue
		}
		emit(extractAstrBotText(payload))
	}
	fmt.Fprint(w, "data: [DONE]\n\n")
	if flusher != nil {
		flusher.Flush()
	}
	return nil
}

// extractAstrBotText 宽容提取 SSE 事件里的文本：JSON 时依次找
// content/text/delta/result 字段（含嵌套 data 包裹）；纯文本直通。
func extractAstrBotText(payload string) string {
	payload = strings.TrimSpace(payload)
	if !strings.HasPrefix(payload, "{") {
		return payload // 非 JSON：纯文本事件直通
	}
	var probe map[string]any
	if err := json.Unmarshal([]byte(payload), &probe); err != nil {
		return payload
	}
	// data: {"type":"...","data":{"content":...}} 与扁平字段都兼容
	if inner, ok := probe["data"].(map[string]any); ok {
		for _, k := range []string{"content", "text", "result"} {
			if v, ok := inner[k].(string); ok && v != "" {
				return v
			}
		}
	}
	for _, k := range []string{"content", "text", "result", "delta"} {
		if v, ok := probe[k].(string); ok && v != "" {
			return v
		}
	}
	return ""
}

// lastUserContent 从 OpenAI messages 里取最后一条 user 的 content
// （字符串或 content parts 数组）；AstrBot 自己维护会话上下文。
func lastUserContent(body []byte) string {
	var req struct {
		Messages []struct {
			Role    string          `json:"role"`
			Content json.RawMessage `json:"content"`
		} `json:"messages"`
	}
	if err := json.Unmarshal(body, &req); err != nil {
		return ""
	}
	for i := len(req.Messages) - 1; i >= 0; i-- {
		if req.Messages[i].Role != "user" {
			continue
		}
		var s string
		if json.Unmarshal(req.Messages[i].Content, &s) == nil {
			return s
		}
		var parts []struct {
			Type string `json:"type"`
			Text string `json:"text"`
		}
		if json.Unmarshal(req.Messages[i].Content, &parts) == nil {
			var b strings.Builder
			for _, p := range parts {
				if p.Type == "text" {
					b.WriteString(p.Text)
				}
			}
			return b.String()
		}
	}
	return ""
}

// astrbotComplete 非流式：收齐 SSE 全文后返回（voice 链路用）。
func astrbotComplete(ctx context.Context, u Upstream, messages []ChatMessage) (string, error) {
	msg := ""
	for i := len(messages) - 1; i >= 0; i-- {
		if messages[i].Role == "user" {
			msg = messages[i].Content
			break
		}
	}
	if msg == "" {
		msg = "你好"
	}
	payload, err := json.Marshal(map[string]any{
		"message":          msg,
		"username":         astrbotUsername,
		"session_id":       "gadget-bigfish-" + u.ID,
		"enable_streaming": true,
	})
	if err != nil {
		return "", err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost,
		strings.TrimRight(u.BaseURL, "/")+"/api/v1/chat", bytes.NewReader(payload))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+u.APIKey)

	resp, err := (&http.Client{Timeout: astrbotTimeout}).Do(req)
	if err != nil {
		return "", fmt.Errorf("astrbot: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", fmt.Errorf("astrbot_http_%d", resp.StatusCode)
	}

	var b strings.Builder
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 64*1024), astrbotMaxChunks)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		p := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if p == "" || p == "[DONE]" {
			continue
		}
		b.WriteString(extractAstrBotText(p))
	}
	if b.Len() == 0 {
		return "", fmt.Errorf("astrbot_empty_response")
	}
	return b.String(), nil
}

// ProbeAstrBot 连通性测试（Web 后台"测试"按钮）：发一条 ping 并取回
// 首段回复。只返回截断文本与脱敏错误码，不带 URL/token。
func ProbeAstrBot(ctx context.Context, id, baseURL, token string) (string, error) {
	reply, err := astrbotComplete(ctx, Upstream{
		ID: id, BaseURL: baseURL, APIKey: token, Kind: "astrbot",
	}, []ChatMessage{{Role: "user", Content: "ping（连通性测试，请回复 pong）"}})
	if err != nil {
		return "", err
	}
	r := []rune(reply)
	if len(r) > 60 {
		r = r[:60]
	}
	return string(r), nil
}
