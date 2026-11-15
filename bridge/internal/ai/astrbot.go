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
	"io"
	"log"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

// AstrBot 对话请求参数（2026-11-14 v1）：
// username 固定 gadget 身份（stable app-controlled username，OpenAPI 要求）；
// session_id 每实例稳定（机器人侧保持上下文记忆）；
// "清空历史"（设备端/未来 Web 端）经 ResetBotSession 轮换 session id，
// 等效 AstrBot 网页端的"新建会话"——旧会话数据留在服务端但不影响新会话。
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
	payload, err := json.Marshal(astrbotPayload(msg, u))
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
	emitted := false

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
		typ, chain, text := extractAstrBotEvent(payload)
		// 思考链（chain_type=reasoning）不进正文；complete 是全量收尾
		// （与增量 plain 重复），已有输出则跳过
		if isReasoningEvent(typ, chain) {
			continue
		}
		if typ == "complete" && emitted {
			continue
		}
		if text != "" {
			emitted = true
		}
		emit(text)
	}
	fmt.Fprint(w, "data: [DONE]\n\n")
	if flusher != nil {
		flusher.Flush()
	}
	return nil
}

// extractAstrBotEvent 返回 (事件类型, 链类型, 文本)。
// 实测（2026-11-15 用户实例）：思考与正文都是 type=plain 增量，区别在
// chain_type 字段——"reasoning"=模型思考链，null=正文；complete 为全量
// 收尾（只含正文）。早期只看 type 白名单（reasoning/thinking…）从来没
// 匹配上，思考增量全部被当正文发给设备念出来——这就是"总把思考读出来"
// 的根因。
func extractAstrBotEvent(payload string) (string, string, string) {
	typ, chain := "", ""
	if strings.HasPrefix(strings.TrimSpace(payload), "{") {
		var probe map[string]any
		if json.Unmarshal([]byte(payload), &probe) == nil {
			if t, ok := probe["type"].(string); ok {
				typ = t
			}
			if c, ok := probe["chain_type"].(string); ok {
				chain = c
			}
		}
	}
	return typ, chain, extractAstrBotText(payload)
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
	// 文本事件形态一：data 直接是字符串（AstrBot 主线 plain/complete
	// 事件：{"type":"plain","data":"文本"}）——此前只认对象形态，导致
	// AstrBot 的报错文本（如"未找到可用对话模型"）被吞成空响应
	if typ, _ := probe["type"].(string); true {
		switch typ {
		// 明确非文本事件：跳过
		case "session_id", "user_message_saved", "message_saved",
			"run_started", "agent_stats", "end", "tool_call", "tool_result":
		default:
			// 其余事件（plain/complete/message/text/未知新增类型）只要
			// data 是非空字符串就当作正文——AstrBot 事件类型会随版本
			// 增删，白名单会漏（曾把报错吞成空响应）
			if v, ok := probe["data"].(string); ok && v != "" {
				return v
			}
		}
	}
	// 形态二：data 是对象（{"content":...}）——与扁平字段都兼容
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
// botSessionID：bot 实例的当前 session id（稳定段 + 轮换代）。
// 代数 bumped = 新会话 = 机器人无记忆。
var (
	sessMu     sync.Mutex
	sessBase   uint64                // 全局代：reset-all +1（覆盖未登记实例）
	sessPerBot = map[string]uint64{} // 实例覆盖代（指定 bot 重置 +1）
)

func botSessionID(botID string) string {
	sessMu.Lock()
	defer sessMu.Unlock()
	return fmt.Sprintf("gadget-bigfish-%s-%d", botID, sessBase+sessPerBot[botID])
}

// ResetBotSession 轮换指定 bot（空=全部）的会话；返回受影响实例数。
func ResetBotSession(botID string) int {
	sessMu.Lock()
	defer sessMu.Unlock()
	if botID == "" {
		sessBase++                       // 全局代：无需预知实例清单
		sessPerBot = map[string]uint64{} // 覆盖清零，统一走全局代
		return 1
	}
	sessPerBot[botID]++
	return 1
}

// astrbotPayload 组装 /api/v1/chat 请求体：
//   - config_id 始终携带（配置文件=人设/模型套餐，粘滞在会话上；显式发
//     "default" 才会把已改过的会话路由重置回默认——保证 Web 端改动即时生效）
//   - selected_provider/selected_model 仅在配置了覆盖时携带（单次请求生效）
func astrbotPayload(message string, u Upstream) map[string]any {
	cfg := u.ConfigID
	if cfg == "" {
		cfg = "default"
	}
	p := map[string]any{
		"message":          message,
		"username":         astrbotUsername,
		"session_id":       botSessionID(u.ID),
		"config_id":        cfg,
		"enable_streaming": true,
	}
	if u.Provider != "" {
		p["selected_provider"] = u.Provider
	}
	if u.Model != "" {
		p["selected_model"] = u.Model
	}
	return p
}

// astrbotStream 流式：与 astrbotComplete 同一条 SSE，但增量回调。
// 推理类事件（模型"思考"文本）必须跳过，否则语音会把英文推理念出来。
func astrbotStream(ctx context.Context, u Upstream, messages []ChatMessage,
	onDelta func(string) error) error {
	msg := lastUserText(messages)
	payload, err := json.Marshal(astrbotPayload(msg, u))
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost,
		strings.TrimRight(u.BaseURL, "/")+"/api/v1/chat", bytes.NewReader(payload))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+u.APIKey)

	resp, err := (&http.Client{Timeout: astrbotTimeout}).Do(req)
	if err != nil {
		return fmt.Errorf("astrbot: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("astrbot_http_%d", resp.StatusCode)
	}

	debugEvents := os.Getenv("ASTRBOT_DEBUG_EVENTS") == "1"
	seen := map[string]bool{}
	emitted := false
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
		typ, chain, text := extractAstrBotEvent(p)
		if debugEvents && !seen[typ+"/"+chain] {
			seen[typ+"/"+chain] = true
			preview := text
			if len(preview) > 60 {
				preview = preview[:60]
			}
			log.Printf("astrbot: event type=%q chain=%q preview=%q", typ, chain, preview)
		}
		if isReasoningEvent(typ, chain) {
			continue
		}
		if typ == "complete" && emitted {
			continue
		}
		if text == "" {
			continue
		}
		emitted = true
		if err := onDelta(text); err != nil {
			return err
		}
	}
	if !emitted {
		return fmt.Errorf("astrbot_empty_response")
	}
	return sc.Err()
}

// isReasoningEvent：模型的思维链事件。主判据是 chain_type=reasoning
// （实测形态：type=plain + chain_type=reasoning 的增量）；type 名单是
// 对其他版本命名的防御。
func isReasoningEvent(typ, chain string) bool {
	if c := strings.ToLower(strings.TrimSpace(chain)); c != "" {
		return c == "reasoning" || c == "reasoning_content" ||
			c == "thinking" || c == "think" || c == "analysis"
	}
	switch strings.ToLower(typ) {
	case "reasoning", "reasoning_content", "thinking", "think", "analysis":
		return true
	}
	return false
}

// lastUserText 取最后一条 user 消息（AstrBot 单轮 message 语义）。
func lastUserText(messages []ChatMessage) string {
	for i := len(messages) - 1; i >= 0; i-- {
		if messages[i].Role == "user" && messages[i].Content != "" {
			return messages[i].Content
		}
	}
	return "你好"
}

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
	payload, err := json.Marshal(astrbotPayload(msg, u))
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
		typ, chain, text := extractAstrBotEvent(p)
		if isReasoningEvent(typ, chain) {
			continue // 思考链不进正文（非流式拼全量同样要滤）
		}
		if typ == "complete" && b.Len() > 0 {
			continue // 全量收尾与增量重复
		}
		b.WriteString(text)
	}
	if b.Len() == 0 {
		return "", fmt.Errorf("astrbot_empty_response")
	}
	return b.String(), nil
}

// ProbeAstrBot 连通性测试（Web 后台"测试"按钮）：发一条 ping 并取回
// 首段回复。只返回截断文本与脱敏错误码，不带 URL/token。
func ProbeAstrBot(ctx context.Context, id, baseURL, token, configID, provider, model string) (string, error) {
	reply, err := astrbotComplete(ctx, Upstream{
		ID: id, BaseURL: baseURL, APIKey: token, Kind: "astrbot",
		ConfigID: configID, Provider: provider, Model: model,
	}, []ChatMessage{{Role: "user", Content: "ping（连通性测试，请回复 pong）"}})
	if err != nil {
		return "", err
	}
	// AstrBot 自身报错也走"正常回复"通道（plain 事件）——先按完整文本
	// 判失败（截断会把"余额不足/未配置模型"等关键原因切掉），再截断成功回复
	for _, bad := range []string{"LLM 请求失败", "LLM 响应错误", "未找到任何可用",
		"All chat models failed", "RateLimitError", "余额不足",
		"Unauthorized", "invalid_api_key"} {
		if strings.Contains(reply, bad) {
			r := []rune(reply)
			if len(r) > 160 {
				r = r[:160]
			}
			return "", fmt.Errorf("%s", string(r))
		}
	}
	r := []rune(reply)
	if len(r) > 60 {
		r = r[:60]
	}
	return string(r), nil
}

// AstrBotConfig AstrBot 配置文件摘要（人设/模型套餐）。
type AstrBotConfig struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	IsDefault bool   `json:"is_default"`
}

// FetchAstrBotConfigs 拉取配置文件列表（GET {base}/api/v1/chat/configs，
// 需 config scope；403 时返回可读提示）。
func FetchAstrBotConfigs(ctx context.Context, baseURL, token string) ([]AstrBotConfig, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet,
		strings.TrimRight(baseURL, "/")+"/api/v1/chat/configs", nil)
	if err != nil {
		return nil, fmt.Errorf("bad_base_url")
	}
	req.Header.Set("Accept", "application/json")
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := (&http.Client{Timeout: 15 * time.Second}).Do(req)
	if err != nil {
		return nil, fmt.Errorf("fetch_failed")
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusForbidden {
		return nil, fmt.Errorf("403：API Key 需勾选 config 权限（设置→OpenAPI→编辑 Key）")
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("http_%d", resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, fmt.Errorf("read_failed")
	}
	// 实测 AstrBot 响应：{"status":"ok","data":{"info_list":[{"id","name","path"}]}}
	var wrapped struct {
		Configs []AstrBotConfig `json:"configs"`
		Data    struct {
			InfoList []AstrBotConfig `json:"info_list"`
		} `json:"data"`
	}
	if err := json.Unmarshal(body, &wrapped); err != nil {
		return nil, fmt.Errorf("unsupported_response")
	}
	out := wrapped.Configs
	if out == nil {
		out = wrapped.Data.InfoList
	}
	// 空列表是合法状态（未创建配置文件）；结构不认识才报错
	if out == nil && !strings.Contains(string(body), "info_list") {
		return nil, fmt.Errorf("unsupported_response")
	}
	if out == nil {
		out = []AstrBotConfig{}
	}
	return out, nil
}
