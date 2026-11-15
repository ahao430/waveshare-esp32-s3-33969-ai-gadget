// noise.go — Muse Noise 会话（标准 Noise_XX_25519_AESGCM_SHA256）。
//
// 上游（esp32/main/noise_control.cpp + components/noise_core）的传输契约：
//
//	WSS wss://<noise_host>/v1/noise?vm_id=<id>   Authorization: Bearer <vm_token>
//	  握手：三条 WS 二进制帧 = Noise XX 的 msg1/msg2/msg3
//	        msg3 的 payload 是"空的 NoiseHandshakeMessage3"（0 字节）
//	        （上行 token 走 upgrade 的 Authorization 头，见 noise_upgrade.h 注释）
//	  之后：每条 WS 二进制帧 = 一个 Noise transport 密文
//	        明文 = TransportFrame（protobuf；>64KB 分片），payload = ServiceFrame
//
// 默认 host 来自上游 noise_control.cpp:75 `NOISE_DEFAULT_HOST`。
package muse

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"sync"
	"time"

	"github.com/flynn/noise"
	"github.com/gorilla/websocket"
)

// DefaultNoiseHost 上游默认值（noise_control.cpp NOISE_DEFAULT_HOST）。
const DefaultNoiseHost = "hatch.metaaivm.com"

// maxPlainFrame Noise 单条明文上限（TransportFrame 分片前的单帧）。
// 上游 kMaxEncodedFrameSize = 65535 - 16（AEAD tag）。
const maxTransportPayload = 65535 - 16 - 30 // 留 protobuf 头余量

// Config 建立会话所需的一切。
type Config struct {
	NoiseHost string // 空 = DefaultNoiseHost；可带 :port
	VMID      string // vm_id（NVS 的 vm_url/vm_id 解析后）
	Token     string // VM auth token（Bearer）
	ProxyURL  string // 空 = 走 HTTPS_PROXY/HTTP_PROXY 环境变量
	Logger    *log.Logger
	// StaticKey 持久化身份（配对可能把它绑到账号）；nil = 随机生成
	StaticKey *noise.DHKey
}

// Conn 一条已建立的 Noise 会话：多路复用请求流。
type Conn struct {
	ws     *websocket.Conn
	tx     *noise.CipherState // 发送方向
	rx     *noise.CipherState // 接收方向
	wmu    sync.Mutex         // WS 写串行化
	logger *log.Logger

	mu      sync.Mutex
	streams map[int64]*Stream
	nextID  int64

	reasm *reassembler // 接收方向 TransportFrame 重组

	// heartbeat 与读循环共用一条连接；读循环退出即会话失效
	closeOnce sync.Once
	closed    chan struct{}
	err       error
}

func newCipherSuite() noise.CipherSuite {
	return noise.NewCipherSuite(noise.DH25519, noise.CipherAESGCM, noise.HashSHA256)
}

// Dial 完成 TLS + WS upgrade + Noise 握手。
func Dial(ctx context.Context, cfg Config) (*Conn, error) {
	host := cfg.NoiseHost
	if host == "" {
		host = DefaultNoiseHost
	}
	if cfg.VMID == "" || cfg.Token == "" {
		return nil, fmt.Errorf("muse: vm_id 与 token 必填")
	}
	logger := cfg.Logger
	if logger == nil {
		logger = log.New(os.Stderr, "muse: ", log.LstdFlags)
	}

	scheme := "wss"
	if os.Getenv("MUSE_NOISE_INSECURE") == "1" { // 测试用：本地 mock
		scheme = "ws"
	}
	u := url.URL{
		Scheme:   scheme,
		Host:     host,
		Path:     "/v1/noise",
		RawQuery: "vm_id=" + url.QueryEscape(cfg.VMID),
	}

	dialer := websocket.Dialer{
		HandshakeTimeout: 15 * time.Second,
		TLSClientConfig:  &tls.Config{MinVersion: tls.VersionTLS12},
	}
	if cfg.ProxyURL != "" {
		pu, err := url.Parse(cfg.ProxyURL)
		if err != nil {
			return nil, fmt.Errorf("muse: proxy url: %w", err)
		}
		dialer.Proxy = http.ProxyURL(pu)
	} else {
		dialer.Proxy = http.ProxyFromEnvironment
	}
	// 移动/受限网络：给 TLS 握手上限，但保留上下文取消
	dctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()

	hdr := http.Header{}
	hdr.Set("Authorization", "Bearer "+cfg.Token)
	ws, resp, err := dialer.DialContext(dctx, u.String(), hdr)
	if err != nil {
		if resp != nil {
			// 401/403 = VM token 被拒（过期或不属于该 VM）
			return nil, fmt.Errorf("muse: upgrade http %d: %w", resp.StatusCode, err)
		}
		return nil, fmt.Errorf("muse: upgrade: %w", err)
	}
	logger.Printf("ws 已升级 host=%s vm=%s", host, cfg.VMID)

	key := cfg.StaticKey
	if key == nil {
		cs := newCipherSuite()
		k, err := cs.GenerateKeypair(rand.Reader)
		if err != nil {
			ws.Close()
			return nil, fmt.Errorf("muse: 生成静态密钥: %w", err)
		}
		key = &k
	}
	hs, err := noise.NewHandshakeState(noise.Config{
		CipherSuite:   newCipherSuite(),
		Pattern:       noise.HandshakeXX,
		Initiator:     true,
		StaticKeypair: *key,
		Random:        rand.Reader,
	})
	if err != nil {
		ws.Close()
		return nil, fmt.Errorf("muse: 初始化握手: %w", err)
	}

	// msg1
	msg1, _, _, err := hs.WriteMessage(nil, nil)
	if err != nil {
		ws.Close()
		return nil, fmt.Errorf("muse: msg1: %w", err)
	}
	if err := ws.WriteMessage(websocket.BinaryMessage, msg1); err != nil {
		ws.Close()
		return nil, fmt.Errorf("muse: 发送 msg1: %w", err)
	}
	// msg2
	_ = ws.SetReadDeadline(time.Now().Add(20 * time.Second))
	mt, msg2, err := ws.ReadMessage()
	if err != nil {
		ws.Close()
		return nil, fmt.Errorf("muse: 读 msg2: %w", err)
	}
	if mt != websocket.BinaryMessage {
		ws.Close()
		return nil, fmt.Errorf("muse: msg2 不是二进制帧 (type=%d)", mt)
	}
	if _, _, _, err := hs.ReadMessage(nil, msg2); err != nil {
		ws.Close()
		return nil, fmt.Errorf("muse: 解析 msg2: %w", err)
	}
	// msg3（payload 必须为空，见文件头注释）
	msg3, cs1, cs2, err := hs.WriteMessage(nil, nil)
	if err != nil {
		ws.Close()
		return nil, fmt.Errorf("muse: msg3: %w", err)
	}
	if err := ws.WriteMessage(websocket.BinaryMessage, msg3); err != nil {
		ws.Close()
		return nil, fmt.Errorf("muse: 发送 msg3: %w", err)
	}
	if cs1 == nil || cs2 == nil {
		ws.Close()
		return nil, fmt.Errorf("muse: 握手未产出传输密钥")
	}
	_ = ws.SetReadDeadline(time.Time{})
	logger.Printf("Noise 会话已建立（%d 字节 msg3）", len(msg3))

	c := &Conn{
		ws:      ws,
		tx:      cs1,
		rx:      cs2,
		logger:  logger,
		streams: map[int64]*Stream{},
		nextID:  1,
		reasm:   newReassembler(),
		closed:  make(chan struct{}),
	}
	go c.readLoop()
	return c, nil
}

// sendFrame 编码 ServiceFrame → ServiceRequest 信封 → TransportFrame → 加密 → WS。
func (c *Conn) sendFrame(f *ServiceFrame) error {
	payload := MarshalRequestEnvelope(ServiceDaemon, f.Marshal())
	tf := &TransportFrame{ChunkID: uint64(time.Now().UnixNano()), Index: 0, Total: 1}
	if len(payload) <= maxTransportPayload {
		tf.Payload = payload
		return c.sendTransport(tf)
	}
	// 大消息分片（dictation 音频用不到这么大的单帧，但保持协议完整）
	total := (len(payload) + maxTransportPayload - 1) / maxTransportPayload
	for i := 0; i < total; i++ {
		end := (i + 1) * maxTransportPayload
		if end > len(payload) {
			end = len(payload)
		}
		tf := &TransportFrame{ChunkID: 1, Index: uint32(i), Total: uint32(total), Payload: payload[i*maxTransportPayload : end]}
		if err := c.sendTransport(tf); err != nil {
			return err
		}
	}
	return nil
}

func (c *Conn) sendTransport(tf *TransportFrame) error {
	plain := tf.Marshal()
	c.wmu.Lock()
	defer c.wmu.Unlock()
	out, err := c.tx.Encrypt(nil, nil, plain)
	if err != nil {
		return fmt.Errorf("muse: 加密: %w", err)
	}
	return c.ws.WriteMessage(websocket.BinaryMessage, out)
}

// readLoop 读 WS → 解密 → 重组 → 分发到流。
func (c *Conn) readLoop() {
	defer c.shutdown(fmt.Errorf("muse: 连接关闭"))
	for {
		mt, data, err := c.ws.ReadMessage()
		if err != nil {
			c.shutdown(err)
			return
		}
		if mt != websocket.BinaryMessage || len(data) == 0 {
			continue
		}
		plain, err := c.rx.Decrypt(nil, nil, data)
		if err != nil {
			c.shutdown(fmt.Errorf("muse: 解密失败: %w", err))
			return
		}
		tf, err := UnmarshalTransportFrame(plain)
		if err != nil {
			c.logger.Printf("坏 TransportFrame: %v", err)
			continue
		}
		full, done := c.reasm.add(tf)
		if !done {
			continue
		}
		// 入站是 ServiceResponse 信封（字段1=payload），payload 才是
		// ServiceFrame——与出站方向（ServiceRequest 信封）对称。
		payload, err := UnmarshalResponseEnvelope(full)
		if err != nil {
			c.logger.Printf("坏响应信封: %v", err)
			continue
		}
		frame, err := UnmarshalServiceFrame(payload)
		if err != nil {
			c.logger.Printf("坏 ServiceFrame: %v", err)
			continue
		}
		c.dispatch(frame)
	}
}

func (c *Conn) dispatch(f *ServiceFrame) {
	c.mu.Lock()
	s := c.streams[f.StreamID]
	c.mu.Unlock()
	if s == nil {
		return
	}
	s.deliver(f)
}

func (c *Conn) shutdown(err error) {
	c.closeOnce.Do(func() {
		c.err = err
		close(c.closed)
		c.ws.Close()
		// 先把流表摘下来再逐个关闭：closeWith 会回头拿 c.mu，
		// 持锁遍历调用会自锁死。
		c.mu.Lock()
		pending := c.streams
		c.streams = map[int64]*Stream{}
		c.mu.Unlock()
		for _, s := range pending {
			s.closeWith(err)
		}
	})
}

// Close 主动关闭会话。
func (c *Conn) Close() {
	c.shutdown(nil)
}

// Done 会话结束信号。
func (c *Conn) Done() <-chan struct{} { return c.closed }

// Err 会话结束原因（nil = 主动关闭）。
func (c *Conn) Err() error {
	select {
	case <-c.closed:
		return c.err
	default:
		return nil
	}
}

// reassembler 把 TransportFrame 分片拼回完整消息。
type reassembler struct {
	cur   map[uint64][]byte
	have  map[uint64]uint32
	total map[uint64]uint32
}

func newReassembler() *reassembler {
	return &reassembler{
		cur:   map[uint64][]byte{},
		have:  map[uint64]uint32{},
		total: map[uint64]uint32{},
	}
}

// add 返回 (完整消息, 是否凑齐)。
func (r *reassembler) add(tf *TransportFrame) ([]byte, bool) {
	if tf.Total == 1 {
		return tf.Payload, true
	}
	id := tf.ChunkID
	if r.total[id] == 0 {
		r.total[id] = tf.Total
	} else if r.total[id] != tf.Total {
		delete(r.cur, id)
		delete(r.have, id)
		delete(r.total, id)
		return nil, false
	}
	r.cur[id] = append(r.cur[id], tf.Payload...)
	r.have[id]++
	if r.have[id] < tf.Total {
		if len(r.cur[id]) > 16<<20 { // 上游 kMaxReassembledLen
			delete(r.cur, id)
			delete(r.have, id)
			delete(r.total, id)
		}
		return nil, false
	}
	full := r.cur[id]
	delete(r.cur, id)
	delete(r.have, id)
	delete(r.total, id)
	return full, true
}

// 保证 net 包被使用（Dialer 里通过环境代理需要 net/http）
var _ = net.Dial
