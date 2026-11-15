// session.go — 官方服务 WebSocket 一轮 PTT 会话（XIAOZHI_PROTOCOL §5.2/5.3）。
//
//	握手：Authorization: Bearer <token> / Protocol-Version: 1 / Device-Id / Client-Id
//	hello → 服务端 hello（session_id + 下行音频参数）→
//	listen start(manual) → 二进制 Opus 包（v1 裸包，一包一帧，无长度前缀）→
//	listen stop → 事件流：stt / llm / tts(start|sentence_start|stop) + Opus 音频
//	tts stop = 回合结束
//
// 二进制 v1 = 裸 Opus 包，WebSocket 消息边界即包边界；VBR/DTX 使包长不固定。
package xiaozhi

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const (
	helloTimeout    = 10 * time.Second
	turnIdleTimeout = 60 * time.Second // 单回合整体上限（无任何消息即失败）
	wsPingInterval  = 20 * time.Second
)

// TurnCallbacks 回合事件（都可能为 nil；audio 为 16k mono int16 的字节序
// 无关切片，调用方自行转 []byte 或追加）。
type TurnCallbacks struct {
	OnHeard func(text string)          // stt：转写文本
	OnText  func(sentence string)      // tts sentence_start：字幕增量
	OnAudio func(pcm []int16)          // 下行音频帧（16k mono）
	OnEmote func(emotion string)       // llm emotion（忽略亦无害）
}

// TurnResult 回合汇总。
type TurnResult struct {
	Heard      string // 最终转写
	Reply      string // 字幕累计（sentence_start 拼接）
	Emotion    string
	AudioBytes int // 下行 16k PCM 字节总数
}

// RunTurn 建连并跑完整 PTT 回合。pcm 为 16k/16bit/mono 小端整段录音。
func RunTurn(ctx context.Context, wsURL, token, deviceID, clientID string,
	pcm []byte, cb TurnCallbacks) (TurnResult, error) {
	var res TurnResult

	dialer := websocket.Dialer{
		Proxy:            http.ProxyFromEnvironment,
		HandshakeTimeout: 15 * time.Second,
	}
	hdr := http.Header{}
	hdr.Set("Authorization", "Bearer "+token)
	hdr.Set("Protocol-Version", "1")
	hdr.Set("Device-Id", deviceID)
	hdr.Set("Client-Id", clientID)

	conn, _, err := dialer.DialContext(ctx, wsURL, hdr)
	if err != nil {
		return res, fmt.Errorf("xiaozhi ws dial: %w", err)
	}
	defer conn.Close()

	// 1) hello
	helloReq := map[string]any{
		"type":      "hello",
		"version":   1,
		"transport": "websocket",
		"features":  map[string]bool{"mcp": false, "mcp_20260728": false, "udp_probe": false, "aec": false},
		"audio_params": map[string]any{
			"format": "opus", "sample_rate": 16000,
			"channels": 1, "frame_duration": 60,
		},
	}
	if err := conn.WriteControl(websocket.PingMessage, nil,
		time.Now().Add(5*time.Second)); err == nil {
		_ = conn.SetReadDeadline(time.Now().Add(helloTimeout))
	}
	if err := writeJSON(conn, helloReq); err != nil {
		return res, fmt.Errorf("xiaozhi hello: %w", err)
	}

	// 2) 等服务端 hello（拿 session_id；下行参数只用日志级信息——
	//    解码器固定 16k 输出，libopus 内部重采样）
	sessionID, err := readServerHello(conn)
	if err != nil {
		return res, err
	}

	// 3) 发送侧：listen start → Opus 帧 → listen stop（独立协程灌包，
	//    读侧同时收事件）。发送不加节流：WebSocket 有序 + 服务端缓冲。
	var sendErr error
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		sendErr = sendAudio(conn, sessionID, pcm)
	}()

	// 4) 读侧：事件 + 音频 → 回调，直到 tts stop / 超时 / 出错
	res, err = readLoop(conn, cb)
	wg.Wait()
	if err != nil {
		return res, err
	}
	if sendErr != nil {
		return res, fmt.Errorf("xiaozhi send: %w", sendErr)
	}
	return res, nil
}

func writeJSON(conn *websocket.Conn, v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return conn.WriteMessage(websocket.TextMessage, b)
}

// readServerHello 等服务端 hello：必须 transport=websocket；10s 超时。
func readServerHello(conn *websocket.Conn) (string, error) {
	for {
		mt, data, err := conn.ReadMessage()
		if err != nil {
			return "", fmt.Errorf("xiaozhi server hello: %w", err)
		}
		if mt != websocket.TextMessage {
			continue // 握手期不期待二进制
		}
		var msg struct {
			Type      string `json:"type"`
			Transport string `json:"transport"`
			SessionID string `json:"session_id"`
		}
		if err := json.Unmarshal(data, &msg); err != nil {
			continue
		}
		if msg.Type == "hello" {
			if msg.Transport != "" && msg.Transport != "websocket" {
				return "", fmt.Errorf("xiaozhi: server transport %q", msg.Transport)
			}
			return msg.SessionID, nil
		}
		// 其他消息（少见）：继续等
	}
}

// sendAudio listen start → 整段 PCM 按 60ms 帧编码发送 → listen stop。
// 尾部不足一帧补静音凑满。
func sendAudio(conn *websocket.Conn, sessionID string, pcm []byte) error {
	if err := writeJSON(conn, map[string]any{
		"session_id": sessionID, "type": "listen",
		"state": "start", "mode": "manual",
	}); err != nil {
		return err
	}

	enc, err := NewEncoder()
	if err != nil {
		return err
	}
	// 写超时按整段上限放宽（长录音突发发送）
	_ = conn.SetWriteDeadline(time.Now().Add(60 * time.Second))

	n := len(pcm) / 2 // int16 数
	samples := make([]int16, opusFrame)
	for off := 0; off < n; off += opusFrame {
		chunk := samples[:0]
		end := off + opusFrame
		if end > n {
			end = n
		}
		for i := off; i < end; i++ {
			chunk = append(chunk, int16(pcm[2*i])|int16(pcm[2*i+1])<<8)
		}
		for len(chunk) < opusFrame { // 尾帧补静音
			chunk = append(chunk, 0)
		}
		packet, err := enc.Encode(chunk)
		if err != nil {
			return err
		}
		if err := conn.WriteMessage(websocket.BinaryMessage, packet); err != nil {
			return err
		}
	}

	return writeJSON(conn, map[string]any{
		"session_id": sessionID, "type": "listen", "state": "stop",
	})
}

// readLoop 收事件直至 tts stop（manual 模式回合结束）、空闲超时或错误。
func readLoop(conn *websocket.Conn, cb TurnCallbacks) (TurnResult, error) {
	var res TurnResult
	dec, err := NewDecoder()
	if err != nil {
		return res, err
	}
	_ = conn.SetReadDeadline(time.Now().Add(turnIdleTimeout))
	done := make(chan struct{})

	// keepalive ping（官方生态 20s 周期）
	go func() {
		t := time.NewTicker(wsPingInterval)
		defer t.Stop()
		for {
			select {
			case <-t.C:
				_ = conn.WriteControl(websocket.PingMessage, nil, time.Now().Add(5*time.Second))
			case <-done:
				return
			}
		}
	}()
	defer close(done)

	for {
		mt, data, err := conn.ReadMessage()
		if err != nil {
			// 正常回合结束后服务端可能直接断开——已有内容则视为完成
			if res.Reply != "" || res.AudioBytes > 0 {
				return res, nil
			}
			return res, fmt.Errorf("xiaozhi read: %w", err)
		}
		_ = conn.SetReadDeadline(time.Now().Add(turnIdleTimeout))

		switch mt {
		case websocket.BinaryMessage:
			pcm, derr := dec.Decode(data)
			if derr != nil {
				continue // 坏包跳过，回合不断
			}
			res.AudioBytes += len(pcm) * 2
			if cb.OnAudio != nil {
				cb.OnAudio(pcm)
			}
		case websocket.TextMessage:
			var msg struct {
				Type    string `json:"type"`
				Text    string `json:"text"`
				State   string `json:"state"`
				Emotion string `json:"emotion"`
			}
			if json.Unmarshal(data, &msg) != nil {
				continue
			}
			switch msg.Type {
			case "stt":
				res.Heard = msg.Text
				if cb.OnHeard != nil {
					cb.OnHeard(msg.Text)
				}
			case "llm":
				res.Emotion = msg.Emotion
				if cb.OnEmote != nil {
					cb.OnEmote(msg.Emotion)
				}
			case "tts":
				switch msg.State {
				case "sentence_start":
					res.Reply += msg.Text
					if cb.OnText != nil {
						cb.OnText(msg.Text)
					}
				case "stop":
					return res, nil // manual 模式回合结束
					// start：无动作
				}
			case "alert":
				if res.Reply == "" && res.AudioBytes == 0 {
					return res, fmt.Errorf("xiaozhi alert: %s", firstLine(msg.Text))
				}
			default:
				// system/unknown：忽略（system reboot 不会出现在 PTT 会话）
			}
		}
	}
}

func firstLine(s string) string {
	for i := 0; i < len(s); i++ {
		if s[i] == '\n' {
			return s[:i]
		}
	}
	if len(s) > 120 {
		return s[:120]
	}
	return s
}
