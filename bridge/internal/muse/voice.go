// voice.go — Muse 回合（Agent：一条会话 + 一个订阅 + 串行回合）。
//
// 两条路径（上游 components/muse/muse_chat_session.cpp 的契约）：
//
//	文本：POST /chat/stream {"message":"…","output_modality":"text","device":"mcu-wearable"}
//	语音：同一条 /chat/stream，把录音打成 base64 WAV 放进消息段：
//	      {"message":"","output_modality":"text","device":"mcu-wearable",
//	       "items":[{"type":"file","mime_type":"audio/wav",
//	                 "filename":"voice_note.wav","data_base64":"…"}]}
//	      服务端转写后直接回答；转写文本以 message.user 的 display_text 回来。
//
// 回复都从连接上长期存在的 POST /chat/subscribe 事件流来：
//
//	delta.message_start / delta.text_append(text) / delta.message_done
//
// 为什么不用 /api/voice/dictation：2026-11-15 真网实测该路由对配对凭据
// 返回 403 "route not allowed for this credential's scopes"；上游注释也
// 写明该路由后端暂无 ASR（VOICE_NOTE=1 才是默认路径）。
package muse

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"strings"
	"sync"
)

const (
	spokenDevice = "mcu-wearable"
	maxNoteBytes = 4 << 20 // WAV 保护（设备录音上限 8s≈256KB）
)

// Reply 一条助手回复。
type Reply struct {
	MessageID string
	Text      string
}

// Agent 串行回合的 Muse 会话封装（Bridge 每个机器人实例一个）。
type Agent struct {
	conn *Conn
	sub  *Stream

	mu      sync.Mutex
	pending *turnWait
	closed  chan struct{}
}

type turnWait struct {
	text    strings.Builder
	heard   string
	done    chan Reply
	onText  func(string)
	onHeard func(string)
}

// NewAgent 建会话并打开 /chat/subscribe 订阅。
func NewAgent(ctx context.Context, cfg Config) (*Agent, error) {
	conn, err := Dial(ctx, cfg)
	if err != nil {
		return nil, err
	}
	sub, err := conn.Open("POST", "/chat/subscribe",
		[]Header{{Key: "Content-Type", Value: "application/json"},
			{Key: "Accept", Value: "application/x-ndjson"}},
		[]byte("{}"), true)
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("muse: 订阅: %w", err)
	}
	a := &Agent{conn: conn, sub: sub, closed: make(chan struct{})}
	go a.readLoop()
	return a, nil
}

// Close 关闭会话。
func (a *Agent) Close() {
	select {
	case <-a.closed:
	default:
		close(a.closed)
	}
	a.conn.Close()
}

// Done 会话结束信号。
func (a *Agent) Done() <-chan struct{} { return a.closed }

// readLoop 消费订阅事件，把回复拼给当前等待者。
func (a *Agent) readLoop() {
	defer close(a.closed)
	for {
		select {
		case <-a.conn.Done():
			return
		case ev, ok := <-a.sub.Events():
			if !ok {
				return
			}
			if ev.Kind != EvBody {
				continue
			}
			for _, line := range strings.Split(string(ev.Body), "\n") {
				if line = strings.TrimSpace(line); line != "" {
					a.handleLine(line)
				}
			}
		}
	}
}

func (a *Agent) handleLine(line string) {
	var ev struct {
		Type    string          `json:"type"`
		Event   string          `json:"event"`
		Payload json.RawMessage `json:"payload"`
	}
	if err := json.Unmarshal([]byte(line), &ev); err != nil || ev.Type != "event" {
		return
	}
	var p struct {
		MessageID   string `json:"message_id"`
		Text        string `json:"text"`
		DisplayText string `json:"display_text"`
	}
	_ = json.Unmarshal(ev.Payload, &p)

	a.mu.Lock()
	w := a.pending
	a.mu.Unlock()
	if w == nil {
		return
	}
	switch ev.Event {
	case "message.user":
		// 语音消息路径：服务端把转写放进 display_text（尾随 [file:…] 标记）
		if txt := cleanHeard(p.DisplayText); txt != "" {
			w.heard = txt
			if w.onHeard != nil {
				w.onHeard(txt)
			}
		}
	case "delta.text_append":
		if p.Text == "" {
			return
		}
		w.text.WriteString(p.Text)
		if w.onText != nil {
			w.onText(p.Text)
		}
	case "delta.message_done", "message.assistant":
		text := strings.TrimSpace(w.text.String())
		if text == "" {
			var full struct {
				DisplayText string `json:"display_text"`
				ReplyText   string `json:"reply_text"`
				Content     string `json:"content"`
			}
			_ = json.Unmarshal(ev.Payload, &full)
			text = strings.TrimSpace(firstNonEmpty(full.DisplayText, full.ReplyText, full.Content))
		}
		if text == "" {
			return
		}
		select {
		case w.done <- Reply{MessageID: p.MessageID, Text: text}:
		default:
		}
	}
}

// cleanHeard 去掉转写里的附件标记（"[file:audio/wav …]"）与首尾空白。
func cleanHeard(s string) string {
	if i := strings.Index(s, "\n[file:"); i >= 0 {
		s = s[:i]
	}
	if i := strings.Index(s, "[file:"); i >= 0 {
		s = s[:i]
	}
	return strings.TrimSpace(s)
}

func firstNonEmpty(vals ...string) string {
	for _, v := range vals {
		if v != "" {
			return v
		}
	}
	return ""
}

func (a *Agent) beginTurn(onHeard, onText func(string)) *turnWait {
	w := &turnWait{done: make(chan Reply, 1), onText: onText, onHeard: onHeard}
	a.mu.Lock()
	a.pending = w
	a.mu.Unlock()
	return w
}

func (a *Agent) endTurn() {
	a.mu.Lock()
	a.pending = nil
	a.mu.Unlock()
}

// Chat 发一条文本，等回复。
func (a *Agent) Chat(ctx context.Context, text string, onText func(string)) (Reply, error) {
	w := a.beginTurn(nil, onText)
	defer a.endTurn()

	body, _ := json.Marshal(map[string]any{
		"message":         text,
		"output_modality": "text",
		"device":          spokenDevice,
	})
	s, err := a.conn.Open("POST", "/chat/stream",
		[]Header{{Key: "Content-Type", Value: "application/json"}}, body, true)
	if err != nil {
		return Reply{}, fmt.Errorf("muse: chat/stream: %w", err)
	}
	if _, _, _, err := s.Response(ctx); err != nil {
		return Reply{}, fmt.Errorf("muse: chat/stream: %w", err)
	}
	return a.wait(w, ctx)
}

// AskVoice 一个语音回合：录音（16k/16bit/mono PCM）→ 服务端转写 → 回复。
func (a *Agent) AskVoice(ctx context.Context, pcm16k []byte,
	onHeard, onText func(string)) (heard string, reply Reply, err error) {
	if len(pcm16k) < 3200 { // <0.1s：本地拦掉，不发云端
		return "", Reply{}, fmt.Errorf("按住时间太短")
	}
	w := a.beginTurn(onHeard, onText)
	defer a.endTurn()

	body, err := noteBody(pcm16k)
	if err != nil {
		return "", Reply{}, err
	}
	s, err := a.conn.Open("POST", "/chat/stream",
		[]Header{{Key: "Content-Type", Value: "application/json"}}, body, true)
	if err != nil {
		return "", Reply{}, fmt.Errorf("muse: chat/stream: %w", err)
	}
	st, _, ack, err := s.Response(ctx)
	if err != nil {
		return "", Reply{}, fmt.Errorf("muse: chat/stream: %w", err)
	}
	if st != 200 {
		return "", Reply{}, fmt.Errorf("muse: chat/stream http %d: %.160s", st, string(ack))
	}
	r, err := a.wait(w, ctx)
	return w.heard, r, err
}

func (a *Agent) wait(w *turnWait, ctx context.Context) (Reply, error) {
	select {
	case r := <-w.done:
		if r.Text == "" {
			return Reply{}, fmt.Errorf("muse: 空回复")
		}
		return r, nil
	case <-ctx.Done():
		return Reply{}, ctx.Err()
	case <-a.conn.Done():
		return Reply{}, fmt.Errorf("muse: 会话已断开")
	}
}

// noteBody 组语音消息请求体（WAV + base64 放进 items[0]）。
func noteBody(pcm16k []byte) ([]byte, error) {
	wav := wav16k(pcm16k)
	if len(wav) > maxNoteBytes {
		return nil, fmt.Errorf("录音太长（%d 字节）", len(wav))
	}
	body := map[string]any{
		"message":         "",
		"output_modality": "text",
		"device":          spokenDevice,
		"items": []map[string]string{{
			"type":        "file",
			"mime_type":   "audio/wav",
			"filename":    "voice_note.wav",
			"data_base64": base64.StdEncoding.EncodeToString(wav),
		}},
	}
	return json.Marshal(body)
}

// wav16k 16k/16bit/mono PCM → 标准 WAV（长度已知，用真实大小）。
func wav16k(pcm []byte) []byte {
	var b bytes.Buffer
	n := uint32(len(pcm))
	b.WriteString("RIFF")
	b.Write(le32(36 + n))
	b.WriteString("WAVEfmt ")
	b.Write(le32(16))
	b.Write(le16(1)) // PCM
	b.Write(le16(1)) // mono
	b.Write(le32(16000))
	b.Write(le32(32000)) // byte rate
	b.Write(le16(2))     // block align
	b.Write(le16(16))    // bits
	b.WriteString("data")
	b.Write(le32(n))
	b.Write(pcm)
	return b.Bytes()
}

func le32(v uint32) []byte { return []byte{byte(v), byte(v >> 8), byte(v >> 16), byte(v >> 24)} }
func le16(v uint16) []byte { return []byte{byte(v), byte(v >> 8)} }
