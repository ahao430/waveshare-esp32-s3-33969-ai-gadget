// astrbot_reasoning_test.go — 锁 2026-11-15 实测事件协议：思考与正文
// 都是 type=plain 增量，区别在 chain_type（reasoning / null）。此前只按
// type 白名单过滤从未命中，思考被当正文发给设备念出来。
package ai

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// mockAstrBot 起一个按实测形态回 SSE 的 AstrBot（可注入异常事件）。
func mockAstrBot(t *testing.T) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer abk_test" {
			t.Errorf("auth = %q", r.Header.Get("Authorization"))
		}
		w.Header().Set("Content-Type", "text/event-stream")
		fl := w.(http.Flusher)
		ev := func(s string) {
			fmt.Fprintf(w, "data: %s\n\n", s)
			fl.Flush()
		}
		ev(`{"type": "session_id", "data": null, "session_id": "s1"}`)
		ev(`{"type": "run_started", "data": {"run_id": "r1"}, "streaming": false}`)
		// 思考链：type=plain + chain_type=reasoning（英文）
		ev(`{"type": "plain", "data": "The user asks about weather.", "streaming": true, "chain_type": "reasoning", "message_id": "m1"}`)
		// 思考链：中文（内容级启发式对纯中文思考无能为力，必须靠链过滤）
		ev(`{"type": "plain", "data": "用户问天气，我要简短回答。", "streaming": true, "chain_type": "reasoning", "message_id": "m1"}`)
		// 正文：chain_type=null
		ev(`{"type": "plain", "data": "今天", "streaming": true, "chain_type": null, "message_id": "m1"}`)
		ev(`{"type": "plain", "data": "晴天。", "streaming": true, "chain_type": null, "message_id": "m1"}`)
		ev(`{"type": "agent_stats", "data": {"token_usage": {"output": 8}}}`)
		// complete 全量（只含正文）
		ev(`{"type": "complete", "data": "今天晴天。"}`)
		ev(`{"type": "message_saved", "data": {"id": 1}}`)
		ev(`{"type": "end", "data": "", "streaming": false}`)
	}))
}

func TestAstrBotStreamSkipsReasoningChain(t *testing.T) {
	srv := mockAstrBot(t)
	defer srv.Close()
	u := Upstream{ID: "bot1", BaseURL: srv.URL, APIKey: "abk_test", Kind: "astrbot"}

	var sb strings.Builder
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	err := astrbotStream(ctx, u, []ChatMessage{{Role: "user", Content: "天气"}},
		func(d string) error { sb.WriteString(d); return nil })
	if err != nil {
		t.Fatalf("stream: %v", err)
	}
	got := sb.String()
	if strings.Contains(got, "The user asks") || strings.Contains(got, "用户问天气") {
		t.Fatalf("reasoning leaked: %q", got)
	}
	if !strings.Contains(got, "今天晴天。") {
		t.Fatalf("body missing: %q", got)
	}
}

func TestAstrBotCompleteSkipsReasoningChain(t *testing.T) {
	srv := mockAstrBot(t)
	defer srv.Close()
	u := Upstream{ID: "bot1", BaseURL: srv.URL, APIKey: "abk_test", Kind: "astrbot"}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	got, err := astrbotComplete(ctx, u, []ChatMessage{{Role: "user", Content: "天气"}})
	if err != nil {
		t.Fatalf("complete: %v", err)
	}
	if strings.Contains(got, "The user asks") || strings.Contains(got, "用户问天气") {
		t.Fatalf("reasoning leaked: %q", got)
	}
	if !strings.Contains(got, "今天晴天。") {
		t.Fatalf("body missing: %q", got)
	}
}

func TestExtractAstrBotEventChain(t *testing.T) {
	typ, chain, text := extractAstrBotEvent(
		`{"type": "plain", "data": "think…", "chain_type": "reasoning"}`)
	if typ != "plain" || chain != "reasoning" || text != "think…" {
		t.Fatalf("got %q %q %q", typ, chain, text)
	}
	typ, chain, _ = extractAstrBotEvent(`{"type": "plain", "data": "hi"}`)
	if chain != "" {
		t.Fatalf("body chain should be empty, got %q", chain)
	}
}

func TestIsReasoningEvent(t *testing.T) {
	cases := []struct {
		typ, chain string
		want       bool
	}{
		{"plain", "reasoning", true}, // 实测主形态
		{"plain", "Reasoning", true}, // 大小写宽容
		{"plain", "thinking", true},  // 变体防御
		{"reasoning", "", true},      // 旧版本独立类型
		{"plain", "", false},         // 正文
		{"plain", "answer", false},   // 其他链类型不是思考
		{"complete", "", false},      // 全量收尾只含正文
		{"agent_stats", "", false},   // 控制事件（另有跳过逻辑）
	}
	for _, c := range cases {
		if got := isReasoningEvent(c.typ, c.chain); got != c.want {
			t.Errorf("isReasoningEvent(%q,%q) = %v, want %v", c.typ, c.chain, got, c.want)
		}
	}
}
