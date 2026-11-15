// voiceturn.go — 探针用的独立语音回合（dictation → chat → 订阅回复）。
package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"log"
	"strings"
	"time"

	"gadget-bridge/internal/muse"
)

// runVoiceTurn 走"语音消息"路径（上游默认 VOICE_NOTE=1）：
// 录音打成 base64 WAV 塞进 /chat/stream 的 items[0].data_base64，
// 由服务端转写并直接回答。流式 dictation 在当前账号上返回 403
// （"route not allowed for this credential's scopes"，且上游注释也说
// 该路由后端暂无 ASR）。
func runVoiceTurn(ctx context.Context, conn *muse.Conn, pcm16k []byte) (string, string, error) {
	reply, err := askNote(ctx, conn, pcm16k, 90*time.Second)
	return "", reply, err
}

// askNote 把 16k PCM 打成 WAV + base64，POST /chat/stream，等订阅侧回复。
func askNote(ctx context.Context, conn *muse.Conn, pcm []byte, wait time.Duration) (string, error) {
	sub, err := conn.Open("POST", "/chat/subscribe",
		[]muse.Header{{Key: "Content-Type", Value: "application/json"},
			{Key: "Accept", Value: "application/x-ndjson"}}, []byte("{}"), true)
	if err != nil {
		return "", err
	}
	replyCh := make(chan string, 1)
	go func() {
		var b strings.Builder
		for ev := range sub.Events() {
			if ev.Kind != muse.EvBody {
				continue
			}
			for _, line := range strings.Split(strings.TrimSpace(string(ev.Body)), "\n") {
				var e struct {
					Type    string          `json:"type"`
					Event   string          `json:"event"`
					Payload json.RawMessage `json:"payload"`
				}
				if json.Unmarshal([]byte(line), &e) != nil || e.Type != "event" {
					continue
				}
				var p struct {
					Text        string `json:"text"`
					DisplayText string `json:"display_text"`
				}
				_ = json.Unmarshal(e.Payload, &p)
				switch e.Event {
				case "message.user":
					if p.DisplayText != "" {
						log.Printf("服务端转写: %q", p.DisplayText)
					}
				case "delta.text_append":
					b.WriteString(p.Text)
				case "delta.message_done":
					select {
					case replyCh <- strings.TrimSpace(b.String()):
					default:
					}
				}
			}
		}
	}()

	wav := wav16k(pcm)
	b64 := base64.StdEncoding.EncodeToString(wav)
	body := `{"message":"","output_modality":"text","device":"mcu-wearable",` +
		`"items":[{"type":"file","mime_type":"audio/wav","filename":"voice_note.wav",` +
		`"data_base64":"` + b64 + `"}]}`
	log.Printf("语音消息：wav %d 字节 → base64 %d 字节", len(wav), len(b64))

	s, err := conn.Open("POST", "/chat/stream",
		[]muse.Header{{Key: "Content-Type", Value: "application/json"}},
		[]byte(body), true)
	if err != nil {
		return "", err
	}
	st, _, ack, err := s.Response(ctx)
	if err != nil {
		return "", err
	}
	log.Printf("chat/stream status=%d ack=%.200s", st, string(ack))
	select {
	case r := <-replyCh:
		return r, nil
	case <-time.After(wait):
		return "", fmt.Errorf("等回复超时")
	case <-ctx.Done():
		return "", ctx.Err()
	}
}

// wav16k 把 16k/16bit/mono PCM 打成标准 WAV（长度已知，用真实大小）。
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
	b.Write(le32(32000))
	b.Write(le16(2))
	b.Write(le16(16))
	b.WriteString("data")
	b.Write(le32(n))
	b.Write(pcm)
	return b.Bytes()
}

func le32(v uint32) []byte { return []byte{byte(v), byte(v >> 8), byte(v >> 16), byte(v >> 24)} }
func le16(v uint16) []byte { return []byte{byte(v), byte(v >> 8)} }

// ask 走 /chat/stream，回复从订阅流里按 message_done 收。
func ask(ctx context.Context, conn *muse.Conn, text string, wait time.Duration) (string, error) {
	sub, err := conn.Open("POST", "/chat/subscribe",
		[]muse.Header{{Key: "Content-Type", Value: "application/json"},
			{Key: "Accept", Value: "application/x-ndjson"}}, []byte("{}"), true)
	if err != nil {
		return "", err
	}
	replyCh := make(chan string, 1)
	go func() {
		var b strings.Builder
		for ev := range sub.Events() {
			if ev.Kind != muse.EvBody {
				continue
			}
			for _, line := range strings.Split(strings.TrimSpace(string(ev.Body)), "\n") {
				var e struct {
					Type    string          `json:"type"`
					Event   string          `json:"event"`
					Payload json.RawMessage `json:"payload"`
				}
				if json.Unmarshal([]byte(line), &e) != nil || e.Type != "event" {
					continue
				}
				var p struct {
					Text string `json:"text"`
				}
				_ = json.Unmarshal(e.Payload, &p)
				switch e.Event {
				case "delta.text_append":
					b.WriteString(p.Text)
					log.Printf("增量: %q", p.Text)
				case "delta.message_done":
					select {
					case replyCh <- strings.TrimSpace(b.String()):
					default:
					}
				}
			}
		}
	}()

	body, _ := json.Marshal(map[string]string{
		"message": text, "output_modality": "text", "device": "mcu-wearable",
	})
	s, err := conn.Open("POST", "/chat/stream",
		[]muse.Header{{Key: "Content-Type", Value: "application/json"}}, body, true)
	if err != nil {
		return "", err
	}
	if _, _, _, err := s.Response(ctx); err != nil {
		return "", err
	}
	select {
	case r := <-replyCh:
		return r, nil
	case <-time.After(wait):
		return "", fmt.Errorf("等回复超时")
	case <-ctx.Done():
		return "", ctx.Err()
	}
}

// up24k 16k→24k 线性插值。
func up24k(pcm []byte) []byte {
	n := len(pcm) / 2
	if n == 0 {
		return nil
	}
	src := make([]int16, n)
	for i := 0; i < n; i++ {
		src[i] = int16(uint16(pcm[i*2]) | uint16(pcm[i*2+1])<<8)
	}
	outN := n * 3 / 2
	out := make([]byte, outN*2)
	for i := 0; i < outN; i++ {
		pos := float64(i) * 2.0 / 3.0
		i0 := int(pos)
		var v int16
		if i0 >= n-1 {
			v = src[n-1]
		} else {
			f := pos - float64(i0)
			v = int16(float64(src[i0])*(1-f) + float64(src[i0+1])*f)
		}
		out[i*2] = byte(uint16(v))
		out[i*2+1] = byte(uint16(v) >> 8)
	}
	return out
}
