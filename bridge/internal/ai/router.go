// Package ai 实现 Bigfish Chat 的模型路由（PLAN §7.4）。
//
// 设计：Bridge 是无状态透传代理——客户端发 OpenAI 兼容请求
// （model = "<provider>/<model>"），Bridge 按前缀选择 upstream、注入
// 对应 API Key，把 upstream 响应（JSON 或 SSE）原样转发回客户端。
// 会话历史由客户端维护；Provider Key 只存在于 Bridge（PLAN §5.3）。
//
// upstream 全部要求 OpenAI 兼容 /chat/completions（DeepSeek/GLM 开放
// 平台/豆包/OpenAI 均满足）；mock upstream 无 Key 时兜底，保证全链路
// 可演示。
package ai

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"time"
)

// Upstream 是一个 OpenAI 兼容服务端点。
type Upstream struct {
	ID      string // 路由前缀，如 "zhipu"
	BaseURL string // 如 https://open.bigmodel.cn/api/paas/v4
	APIKey  string
}

// Router 选择 upstream 并代理请求。
type Router struct {
	upstreams map[string]Upstream // key = provider 前缀（小写）
	defaultID string
	hc        *http.Client
}

// 请求体上限与 upstream 超时。
const (
	maxRequestBytes  = 1 << 20
	upstreamTimeout  = 120 * time.Second // LLM 生成可能较慢
	defaultModelName = "dummy"
)

var ErrNoUpstream = errors.New("no_upstream")

// NewRouter 构建 Router；upstreams 为空时自动注册 mock。
func NewRouter(upstreams []Upstream, defaultID string) *Router {
	m := make(map[string]Upstream, len(upstreams)+1)
	for _, u := range upstreams {
		if u.ID == "" || u.BaseURL == "" {
			continue
		}
		m[strings.ToLower(u.ID)] = u
	}
	if _, ok := m["mock"]; !ok {
		m["mock"] = Upstream{ID: "mock", BaseURL: "mock://"}
	}
	if _, ok := m[strings.ToLower(defaultID)]; !ok {
		defaultID = "mock"
	}
	return &Router{
		upstreams: m,
		defaultID: strings.ToLower(defaultID),
		hc:        &http.Client{Timeout: upstreamTimeout}, // 流式响应超时只管到首字节后的整体窗口
	}
}

// Providers 返回已配置的 provider id 列表（/api/v1/chat/providers 用）。
func (r *Router) Providers() []string {
	ids := make([]string, 0, len(r.upstreams))
	for id := range r.upstreams {
		ids = append(ids, id)
	}
	return ids
}

// resolve 解析 model 字段（"<provider>/<model>"；无 "/" 用默认 provider）。
func (r *Router) resolve(model string) (Upstream, string, error) {
	id := r.defaultID
	rest := model
	if i := strings.Index(model, "/"); i >= 0 {
		id = strings.ToLower(model[:i])
		rest = model[i+1:]
	}
	u, ok := r.upstreams[id]
	if !ok {
		return Upstream{}, "", fmt.Errorf("%w: %s", ErrNoUpstream, id)
	}
	if rest == "" {
		rest = defaultModelName
	}
	return u, rest, nil
}

// ChatCompletions 代理 /chat/completions。
// 客户端 body 原样上传（Bridge 不解析 messages），仅改写 model 字段
// 与 Authorization；响应（含 SSE）流式回传。
// mock provider 特判：不走真实 HTTP，直接生成响应（见 mockupstream.go）。
func (r *Router) ChatCompletions(w http.ResponseWriter, body []byte, clientGone <-chan struct{}) error {
	// 提取并改写 model
	model := jsonGetString(body, "model")
	u, modelName, err := r.resolve(model)
	if err != nil {
		return err
	}
	if u.ID == "mock" {
		return serveMockChat(w, body)
	}

	upstreamBody, err := jsonSetString(body, "model", modelName)
	if err != nil {
		return fmt.Errorf("bad_request_body: %w", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() {
		select {
		case <-clientGone:
			cancel() // 客户端断开：中止 upstream 请求
		case <-ctx.Done():
		}
	}()

	req, err := http.NewRequestWithContext(ctx, http.MethodPost,
		strings.TrimRight(u.BaseURL, "/")+"/chat/completions",
		bytesReader(upstreamBody))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json, text/event-stream")
	if u.ID != "mock" {
		// mock upstream 无鉴权；真实 upstream Key 只进此 header，不进日志
		req.Header.Set("Authorization", "Bearer "+u.APIKey)
	}

	resp, err := r.hc.Do(req)
	if err != nil {
		return fmt.Errorf("upstream: %w", err)
	}
	defer resp.Body.Close()

	// 透传状态码与内容类型，流式拷贝
	w.Header().Set("Content-Type", resp.Header.Get("Content-Type"))
	if resp.Header.Get("Content-Type") == "text/event-stream" {
		w.Header().Set("Cache-Control", "no-cache")
		w.Header().Set("X-Accel-Buffering", "no")
	}
	w.WriteHeader(resp.StatusCode)
	flusher, _ := w.(http.Flusher)
	buf := make([]byte, 4096)
	for {
		n, err := resp.Body.Read(buf)
		if n > 0 {
			if _, werr := w.Write(buf[:n]); werr != nil {
				return nil // 客户端断开
			}
			if flusher != nil {
				flusher.Flush()
			}
		}
		if err != nil {
			return nil // EOF 或 upstream 结束
		}
	}
}
