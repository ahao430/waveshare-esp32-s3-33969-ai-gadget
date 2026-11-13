package ai

import (
	"bytes"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestResolve(t *testing.T) {
	r := NewRouter([]Upstream{
		{ID: "zhipu", BaseURL: "https://upstream.test/api", APIKey: "k1"},
		{ID: "deepseek", BaseURL: "https://ds.test", APIKey: "k2"},
	}, "")

	// 显式前缀
	u, rest, err := r.resolve("DeepSeek/deepseek-chat")
	if err != nil || u.ID != "deepseek" || rest != "deepseek-chat" {
		t.Fatalf("resolve prefix: %v %s %s", err, u.ID, rest)
	}
	// 无前缀 → 默认（无默认配置时 = mock）
	u, rest, err = r.resolve("glm-4-flash")
	if err != nil || u.ID != "mock" {
		t.Fatalf("resolve default: %v %s", err, u.ID)
	}
	// 未知前缀
	if _, _, err := r.resolve("nope/model"); !errors.Is(err, ErrNoUpstream) {
		t.Fatalf("unknown prefix: %v", err)
	}
	// mock 兜底注册
	if _, ok := r.upstreams["mock"]; !ok {
		t.Fatal("mock 未兜底注册")
	}
}

func TestJSONSetGet(t *testing.T) {
	body := []byte(`{"model":"x/y","messages":[{"role":"user"}],"stream":true}`)
	if jsonGetString(body, "model") != "x/y" {
		t.Fatal("get model")
	}
	out, err := jsonSetString(body, "model", "z")
	if err != nil {
		t.Fatalf("set: %v", err)
	}
	if jsonGetString(out, "model") != "z" {
		t.Fatalf("roundtrip: %s", out)
	}
	if _, err := jsonSetString([]byte(`{broken`), "model", "z"); err == nil {
		t.Fatal("broken json 应报错")
	}
}

// 端到端：mock 流式响应逐字到达。
func TestMockStreamE2E(t *testing.T) {
	r := NewRouter(nil, "")
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		body, _ := readAll(req)
		_ = r.ChatCompletions(w, body, req.Context().Done())
	}))
	t.Cleanup(srv.Close)

	resp, err := srv.Client().Post(srv.URL, "application/json",
		strings.NewReader(`{"model":"mock/dummy","stream":true,"messages":[{"role":"user","content":"你好"}]}`))
	if err != nil {
		t.Fatalf("post: %v", err)
	}
	defer resp.Body.Close()

	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "text/event-stream") {
		t.Fatalf("content-type = %s", ct)
	}
	all := readAllStr(t, resp.Body)
	if !strings.Contains(all, "data: ") || !strings.Contains(all, "[DONE]") {
		t.Fatalf("SSE 帧缺失: %s", all[:min(200, len(all))])
	}
	// 流式按字符分帧：必须拼接各 delta 后断言内容
	var sb strings.Builder
	for _, line := range strings.Split(all, "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "data: ") || strings.Contains(line, "[DONE]") {
			continue
		}
		var chunk struct {
			Choices []struct {
				Delta struct {
					Content string `json:"content"`
				} `json:"delta"`
			} `json:"choices"`
		}
		if err := json.Unmarshal([]byte(strings.TrimPrefix(line, "data: ")), &chunk); err == nil {
			if len(chunk.Choices) > 0 {
				sb.WriteString(chunk.Choices[0].Delta.Content)
			}
		}
	}
	full := sb.String()
	if !strings.Contains(full, "你好") || !strings.Contains(full, "[mock]") {
		t.Fatalf("拼接后回复缺失内容: %q", full)
	}
}

// 非流式：完整 JSON。
func TestMockNonStreamE2E(t *testing.T) {
	r := NewRouter(nil, "")
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		body, _ := readAll(req)
		_ = r.ChatCompletions(w, body, req.Context().Done())
	}))
	t.Cleanup(srv.Close)

	resp, err := srv.Client().Post(srv.URL, "application/json",
		strings.NewReader(`{"model":"","stream":false,"messages":[{"role":"user","content":"hi"}]}`))
	if err != nil {
		t.Fatalf("post: %v", err)
	}
	defer resp.Body.Close()

	var m struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&m); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(m.Choices) == 0 || !bytes.Contains([]byte(m.Choices[0].Message.Content), []byte("[mock]")) {
		t.Fatalf("回复内容错误: %+v", m)
	}
}
