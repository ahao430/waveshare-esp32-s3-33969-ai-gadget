// volcengine_test.go — 火山豆包 ASR/TTS 契约单测（不起真网）。
// TTS 用 httptest 校验 Bearer; 分号鉴权、body 字段与 base64 解码；
// ASR 用 gorilla mock WS 对打二进制分帧协议（gzip JSON 元数据 →
// 音频分片 → 末包 0xFFFFFFFF → 负序列号响应收尾）。
package voice

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func TestVolcCreds(t *testing.T) {
	id, tok, err := volcCreds("1234567:my-token")
	if err != nil || id != "1234567" || tok != "my-token" {
		t.Fatalf("got %q %q %v", id, tok, err)
	}
	for _, bad := range []string{"", "no-colon", ":only-token", "only-appid:"} {
		if _, _, err := volcCreds(bad); err == nil {
			t.Fatalf("expected error for %q", bad)
		}
	}
}

func TestVolcengineTTS(t *testing.T) {
	var gotAuth string
	var gotBody map[string]any
	// 16k PCM：2 个满幅样本，合成结果应原样回来
	pcm := []int16{1234, -5678}
	pcmBytes := make([]byte, len(pcm)*2)
	for i, v := range pcm {
		binary.LittleEndian.PutUint16(pcmBytes[i*2:], uint16(v))
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotAuth = r.Header.Get("Authorization")
		_ = json.NewDecoder(r.Body).Decode(&gotBody)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"code": 3000, "message": "success",
			"data": base64.StdEncoding.EncodeToString(pcmBytes),
		})
	}))
	defer srv.Close()
	t.Setenv("VOLC_TTS_BASE_URL", srv.URL)

	out, err := volcengineTTS(context.Background(), "1234567:tok", "你好。", "BV407_streaming")
	if err != nil {
		t.Fatalf("tts: %v", err)
	}
	if !bytes.Equal(bytesFromInt16(out), pcmBytes) {
		t.Fatalf("pcm mismatch: %v", out)
	}
	// 鉴权：官方要求 "Bearer;<token>"（分号，不是空格）
	if gotAuth != "Bearer;tok" {
		t.Fatalf("auth = %q, want Bearer;tok", gotAuth)
	}
	app := gotBody["app"].(map[string]any)
	audio := gotBody["audio"].(map[string]any)
	req := gotBody["request"].(map[string]any)
	if app["appid"] != "1234567" || app["cluster"] != "volcano_tts" {
		t.Fatalf("app = %v", app)
	}
	if audio["voice_type"] != "BV407_streaming" || audio["encoding"] != "pcm" ||
		audio["rate"] != float64(16000) {
		t.Fatalf("audio = %v", audio)
	}
	if req["operation"] != "query" || req["text"] != "你好。" || req["reqid"] == "" {
		t.Fatalf("request = %v", req)
	}
}

func TestVolcengineTTSError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"code": 3001, "message": "quota exceeded for types: concurrency",
		})
	}))
	defer srv.Close()
	t.Setenv("VOLC_TTS_BASE_URL", srv.URL)
	if _, err := volcengineTTS(context.Background(), "a:b", "x", ""); err == nil ||
		!strings.Contains(err.Error(), "3001") {
		t.Fatalf("want code-3001 error, got %v", err)
	}
	// Key 格式错：直接报格式
	if _, err := volcengineTTS(context.Background(), "bad", "x", ""); err == nil ||
		!strings.Contains(err.Error(), "AppID:AccessToken") {
		t.Fatalf("want format error, got %v", err)
	}
}

// TestVolcengineASRMock — mock WS 服务端对打完整一句话识别流程。
func TestVolcengineASRMock(t *testing.T) {
	pcm := make([]int16, 3200*3+777) // 3 个整包 + 1 个尾包
	for i := range pcm {
		pcm[i] = int16(i % 3000)
	}
	pcmBytes := bytesFromInt16(pcm)

	var sawMeta map[string]any
	var gotAudio []byte
	up := websocket.Upgrader{}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Api-App-Key") != "1234567" ||
			r.Header.Get("X-Api-Access-Key") != "tok" ||
			r.Header.Get("X-Api-Resource-Id") != "volc.bigasr.sauc.duration" {
			t.Errorf("bad auth headers: %+v", r.Header)
		}
		conn, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		// 1) full client request：gzip JSON
		mt, data, err := conn.ReadMessage()
		if err != nil || mt != websocket.BinaryMessage || len(data) < 8 {
			t.Errorf("meta read: %v", err)
			return
		}
		if data[0] != 0x11 || data[1]>>4 != volcMsgFullClient || data[2]>>4 != volcSerGzipJSON {
			t.Errorf("meta header = % x", data[:4])
		}
		size := binary.BigEndian.Uint32(data[4:8])
		meta, err := volcGunzip(data[8 : 8+size])
		if err != nil {
			t.Errorf("meta gunzip: %v", err)
			return
		}
		_ = json.Unmarshal(meta, &sawMeta)
		// 2) audio only 直到末包（大小 0xFFFFFFFF）
		for {
			_, data, err := conn.ReadMessage()
			if err != nil {
				return
			}
			if len(data) >= 8 && binary.BigEndian.Uint32(data[4:8]) == 0xFFFFFFFF {
				break
			}
			chunk, err := volcGunzip(data[8:])
			if err != nil {
				t.Errorf("audio gunzip: %v", err)
				return
			}
			gotAudio = append(gotAudio, chunk...)
		}
		// 3) 回负序列号响应：gzip JSON 结果
		payload, _ := json.Marshal(map[string]any{
			"result": []map[string]string{{"text": "今天天气怎么样"}},
		})
		zb, _ := volcGzip(payload)
		frame := []byte{0x11, 0x09<<4 | volcFlagSeqNeg, 0x11, 0x00, 0, 0, 0, 1}
		frame = binary.BigEndian.AppendUint32(frame, uint32(len(zb)))
		frame = append(frame, zb...)
		_ = conn.WriteMessage(websocket.BinaryMessage, frame)
	}))
	defer srv.Close()
	t.Setenv("VOLC_ASR_WS_URL", "ws"+strings.TrimPrefix(srv.URL, "http"))

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	text, err := volcengineTranscribe(ctx, "1234567:tok", pcmBytes)
	if err != nil {
		t.Fatalf("transcribe: %v", err)
	}
	if text != "今天天气怎么样" {
		t.Fatalf("text = %q", text)
	}
	if !bytes.Equal(gotAudio, pcmBytes) {
		t.Fatalf("audio bytes mismatch: got %d want %d", len(gotAudio), len(pcmBytes))
	}
	audio := sawMeta["audio"].(map[string]any)
	req := sawMeta["request"].(map[string]any)
	if audio["format"] != "pcm" || audio["rate"] != float64(16000) ||
		audio["channel"] != float64(1) {
		t.Fatalf("audio meta = %v", audio)
	}
	if req["model_name"] != "bigmodel" || req["enable_punc"] != true {
		t.Fatalf("request meta = %v", req)
	}
}

func bytesFromInt16(s []int16) []byte {
	b := make([]byte, len(s)*2)
	for i, v := range s {
		binary.LittleEndian.PutUint16(b[i*2:], uint16(v))
	}
	return b
}
