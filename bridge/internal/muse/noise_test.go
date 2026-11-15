// noise_test.go — mock Muse VM：Noise XX responder + 帧回环。
//
// 目的：在没有真实 VM/凭据时把整条链路跑通——WS upgrade（含 Bearer 头）、
// Noise XX 握手（msg3 空 payload）、TransportFrame 加密收发、流多路复用、
// 请求体流式上行与响应分片下行、Reset。真实服务端行为按上游
// noise_control.cpp / muse_chat_session.cpp 的契约模拟。
package muse

import (
	"bytes"
	"context"
	"crypto/rand"
	"log"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/flynn/noise"
	"github.com/gorilla/websocket"
)

func tLog(format string, args ...any) {
	log.Printf("mock: "+format, args...)
}

// mockVM 一个最小 Muse VM：把收到的请求回显成 JSON 响应。
type mockVM struct {
	upgrader websocket.Upgrader
	srv      *httptest.Server

	mu       sync.Mutex
	gotPath  string
	gotAuth  string
	gotBody  []byte
	gotVerbs []string
}

func newMockVM(t *testing.T) *mockVM {
	t.Helper()
	m := &mockVM{upgrader: websocket.Upgrader{}}
	m.srv = httptest.NewServer(http.HandlerFunc(m.handle))
	return m
}

func (m *mockVM) handle(w http.ResponseWriter, r *http.Request) {
	m.mu.Lock()
	m.gotPath = r.URL.RequestURI()
	m.gotAuth = r.Header.Get("Authorization")
	m.mu.Unlock()

	ws, err := m.upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer ws.Close()

	// Noise XX responder
	cs := noise.NewCipherSuite(noise.DH25519, noise.CipherAESGCM, noise.HashSHA256)
	static, _ := cs.GenerateKeypair(rand.Reader)
	hs, err := noise.NewHandshakeState(noise.Config{
		CipherSuite: cs, Pattern: noise.HandshakeXX, Initiator: false,
		StaticKeypair: static, Random: rand.Reader,
	})
	if err != nil {
		return
	}
	// msg1
	_, msg1, err := ws.ReadMessage()
	if err != nil {
		tLog("mock: read msg1: %v", err)
		return
	}
	if _, _, _, err := hs.ReadMessage(nil, msg1); err != nil {
		tLog("mock: read hs msg1: %v", err)
		return
	}
	// msg2
	msg2, _, _, err := hs.WriteMessage(nil, nil)
	if err != nil {
		return
	}
	if err := ws.WriteMessage(websocket.BinaryMessage, msg2); err != nil {
		return
	}
	// msg3（客户端 payload 必须为空）；响应方的 cipherstate 由这次
	// ReadMessage 产出（flynn/noise 没有 Channels()）。
	_, msg3, err := ws.ReadMessage()
	if err != nil {
		return
	}
	payload, cs1, cs2, err := hs.ReadMessage(nil, msg3)
	if err != nil || len(payload) != 0 || cs1 == nil || cs2 == nil {
		return
	}
	// flynn/noise 的 Split() 对两端都返回 (hk1,hk2)；Noise 规范里发起方用
	// hk1 发送、响应方用 hk2 发送——响应方这里必须反过来用。
	tx, rx := cs2, cs1

	for {
		mt, data, err := ws.ReadMessage()
		if err != nil {
			tLog("mock: read: %v", err)
			return
		}
		if mt != websocket.BinaryMessage {
			continue
		}
		plain, err := rx.Decrypt(nil, nil, data)
		if err != nil {
			tLog("mock: decrypt: %v", err)
			return
		}
		tf, err := UnmarshalTransportFrame(plain)
		if err != nil {
			return
		}
		// 客户端→服务端：ServiceRequest 信封
		svcPayload, err := unwrapRequest(tf.Payload)
		if err != nil {
			tLog("mock: unwrap: %v", err)
			return
		}
		f, err := UnmarshalServiceFrame(svcPayload)
		if err != nil {
			tLog("mock: frame: %v", err)
			return
		}
		tLog("recv frame kind=%v stream=%d", f.Kind, f.StreamID)
		switch f.Kind {
		case KindRequest:
			m.mu.Lock()
			m.gotVerbs = append(m.gotVerbs, f.Request.Verb+" "+f.Request.Path)
			m.mu.Unlock()
			// 先回 status 200，再流式回 body
			m.reply(tx, ws, &ServiceFrame{
				StreamID: f.StreamID, Kind: KindResponse,
				Response: &AppResponse{Status: 200, Headers: []Header{{"Content-Type", "application/x-ndjson"}}},
			})
			// end_body 已置位 = 请求体随请求帧一起到了，直接回响应体
			if f.Request.EndBody {
				m.sendBody(tx, ws, f.StreamID)
			}
		case KindBodyChunk:
			m.mu.Lock()
			m.gotBody = append(m.gotBody, f.Chunk.Data...)
			m.mu.Unlock()
			if f.Chunk.EndBody {
				m.sendBody(tx, ws, f.StreamID)
			}
		case KindReset:
			// 客户端取消：不再回复
		}
	}
}

// sendBody 两段 body，最后一段 end_body。
func (m *mockVM) sendBody(tx *noise.CipherState, ws *websocket.Conn, streamID int64) {
	m.reply(tx, ws, &ServiceFrame{
		StreamID: streamID, Kind: KindBodyChunk,
		Chunk: &BodyChunk{Data: []byte(`{"text":"`)},
	})
	m.reply(tx, ws, &ServiceFrame{
		StreamID: streamID, Kind: KindBodyChunk,
		Chunk: &BodyChunk{Data: []byte(`你好"}` + "\n"), EndBody: true},
	})
}

func (m *mockVM) reply(tx *noise.CipherState, ws *websocket.Conn, f *ServiceFrame) {
	tLog("send frame kind=%v stream=%d", f.Kind, f.StreamID)
	// 服务端→客户端：ServiceResponse 信封
	env := appendBytes(nil, 1, f.Marshal(), true)
	tf := &TransportFrame{ChunkID: 1, Index: 0, Total: 1, Payload: env}
	plain := tf.Marshal()
	out, err := tx.Encrypt(nil, nil, plain)
	if err != nil {
		return
	}
	ws.WriteMessage(websocket.BinaryMessage, out)
}

// unwrapRequest 解客户端信封（字段 2 = payload）。
func unwrapRequest(b []byte) ([]byte, error) {
	p := &parser{b: b}
	for {
		field, wire := p.next()
		if p.err != nil || field == 0 {
			break
		}
		if field == 2 && wire == 2 {
			return p.bytes(), nil
		}
		p.skip(wire)
	}
	return nil, p.err
}

// TestNoiseSessionEndToEnd 客户端 ↔ mock VM 全链路。
func TestNoiseSessionEndToEnd(t *testing.T) {
	vm := newMockVM(t)
	defer vm.srv.Close()
	t.Setenv("MUSE_NOISE_INSECURE", "1")

	host := strings.TrimPrefix(vm.srv.URL, "http://")
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	c, err := Dial(ctx, Config{NoiseHost: host, VMID: "vm-test-1", Token: "tok-abc"})
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer c.Close()

	// 服务端应看到 upgrade 路径与 Bearer
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		vm.mu.Lock()
		p, a := vm.gotPath, vm.gotAuth
		vm.mu.Unlock()
		if p != "" && a != "" {
			if !strings.Contains(p, "vm_id=vm-test-1") {
				t.Fatalf("upgrade path = %q", p)
			}
			if a != "Bearer tok-abc" {
				t.Fatalf("auth = %q", a)
			}
			break
		}
		time.Sleep(20 * time.Millisecond)
	}

	// 打开流、流式上行、收响应
	s, err := c.Open("POST", "/api/voice/dictation?sample_rate_hz=24000",
		nil, nil, false)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	for i := 0; i < 3; i++ {
		if err := s.Send([]byte{1, 2, 3, 4}); err != nil {
			t.Fatalf("send: %v", err)
		}
	}
	if err := s.Finish(); err != nil {
		t.Fatalf("finish: %v", err)
	}
	st, hdrs, body, err := s.Response(ctx)
	if err != nil {
		t.Fatalf("response: %v", err)
	}
	if st != 200 {
		t.Fatalf("status = %d", st)
	}
	if len(hdrs) != 1 || hdrs[0].Key != "Content-Type" {
		t.Fatalf("headers = %+v", hdrs)
	}
	if !bytes.Equal(body, []byte(`{"text":"你好"}`+"\n")) {
		t.Fatalf("body = %q", body)
	}

	// 服务端收到的上行字节与请求路径
	vm.mu.Lock()
	gotBody, verbs := vm.gotBody, vm.gotVerbs
	vm.mu.Unlock()
	if len(gotBody) != 12 {
		t.Fatalf("server got %d body bytes, want 12", len(gotBody))
	}
	if len(verbs) != 1 || !strings.HasPrefix(verbs[0], "POST /api/voice/dictation") {
		t.Fatalf("verbs = %v", verbs)
	}
}

// TestNoiseMultipleStreams 并发流互不串台。
func TestNoiseMultipleStreams(t *testing.T) {
	vm := newMockVM(t)
	defer vm.srv.Close()
	t.Setenv("MUSE_NOISE_INSECURE", "1")

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	c, err := Dial(ctx, Config{NoiseHost: strings.TrimPrefix(vm.srv.URL, "http://"), VMID: "vm1", Token: "t"})
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer c.Close()

	var wg sync.WaitGroup
	errs := make([]error, 4)
	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			s, err := c.Open("POST", "/chat/stream", nil, []byte(`{}`), true)
			if err != nil {
				errs[i] = err
				return
			}
			_, _, body, err := s.Response(ctx)
			if err == nil && !bytes.Contains(body, []byte("你好")) {
				err = errBadBody
			}
			errs[i] = err
		}(i)
	}
	wg.Wait()
	for i, err := range errs {
		if err != nil {
			t.Fatalf("stream %d: %v", i, err)
		}
	}
}

var errBadBody = &bodyErr{}

type bodyErr struct{}

func (*bodyErr) Error() string { return "响应体不含预期内容" }

// TestDialRequiresCreds 缺凭据要早失败，而不是发出半截请求。
func TestDialRequiresCreds(t *testing.T) {
	_, err := Dial(context.Background(), Config{NoiseHost: "example.invalid"})
	if err == nil || !strings.Contains(err.Error(), "必填") {
		t.Fatalf("want creds error, got %v", err)
	}
}

func TestMain(m *testing.M) {
	os.Exit(m.Run())
}
