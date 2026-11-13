package ai

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"
)

// RegisterMockHandlers 在桥内起一个 mock OpenAI 兼容 upstream。
// 由于 Router 直接代理任意 BaseURL，mock 用 Go 默认 mux 不可行；
// 改为 Router.ChatCompletions 特判 mock:// scheme，本文件提供
// mock 响应生成器，由 API 层在 handler 中调用（见 api/server.go）。
//
// 方案说明：mock 不走真实 HTTP upstream，而是 ChatCompletions 检测到
// mock provider 时直接返回生成的响应（流式或非流式），语义与真实
// upstream 一致。

// mockStreamChunk 生成一条 SSE data 行。
func mockStreamChunk(id string, delta string, finish bool) string {
	payload := map[string]any{
		"id":      id,
		"object":  "chat.completion.chunk",
		"created": time.Now().Unix(),
		"model":   "mock/dummy",
		"choices": []map[string]any{{
			"index":         0,
			"delta":         map[string]string{"content": delta},
			"finish_reason": nil,
		}},
	}
	if finish {
		payload["choices"].([]map[string]any)[0]["finish_reason"] = "stop"
		payload["choices"].([]map[string]any)[0]["delta"] = map[string]string{}
	}
	b, _ := json.Marshal(payload)
	return "data: " + string(b) + "\n\n"
}

// serveMockChat 生成 mock 回复（echo 风格，按请求 messages 拼接）。
// stream=true 时以 30ms/块推 SSE；false 时返回完整 JSON。
func serveMockChat(w http.ResponseWriter, body []byte) error {
	var req struct {
		Stream   bool `json:"stream"`
		Messages []struct {
			Role    string `json:"role"`
			Content string `json:"content"`
		} `json:"messages"`
	}
	if err := json.Unmarshal(body, &req); err != nil {
		http.Error(w, `{"error":"bad_request"}`, http.StatusBadRequest)
		return nil
	}

	// 回复生成与 Complete 共用 mockReplyText
	reply := mockReplyText(toChatMessages(req.Messages))

	if !req.Stream {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		return json.NewEncoder(w).Encode(map[string]any{
			"id":     "mock-1",
			"object": "chat.completion",
			"model":  "mock/dummy",
			"choices": []map[string]any{{
				"index":         0,
				"message":       map[string]string{"role": "assistant", "content": reply},
				"finish_reason": "stop",
			}},
		})
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(http.StatusOK)
	flusher, ok := w.(http.Flusher)
	if !ok {
		return fmt.Errorf("streaming unsupported")
	}

	// 按小块流式推送
	const chunkSize = 6
	id := fmt.Sprintf("mock-%d", time.Now().UnixNano())
	for _, r := range reply {
		if _, err := w.Write([]byte(mockStreamChunk(id, string(r), false))); err != nil {
			return nil
		}
		flusher.Flush()
		time.Sleep(30 * time.Millisecond)
	}
	_, _ = w.Write([]byte(mockStreamChunk(id, "", true)))
	_, _ = w.Write([]byte("data: [DONE]\n\n"))
	flusher.Flush()
	return nil
}

func truncateRunes(s string, n int) string {
	r := []rune(strings.TrimSpace(s))
	if len(r) > n {
		r = r[:n]
	}
	return string(r)
}
