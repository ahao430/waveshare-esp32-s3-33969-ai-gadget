// manager.go — 身份/凭据管理与 voice 集成入口（对齐 internal/muse.Manager 形态）。
//
// 凭据持久化在 storage（app_settings.xiaozhi），Manager 只拿 Creds/Save
// 闭包：Web 保存配置后下一次请求即生效，无需重启。
package xiaozhi

import (
	"context"
	"fmt"
	"log"
	"sync"
)

// Credentials 一台模拟设备的全部身份（整体加密落库）。
type Credentials struct {
	DeviceID string `json:"device_id"` // 02: 开头本地管理 MAC（稳定）
	ClientID string `json:"client_id"` // UUIDv4（稳定）
	OTAURL   string `json:"ota_url"`   // 空=官方端点
	WsURL    string `json:"ws_url"`    // OTA 下发
	WsToken  string `json:"ws_token"`  // OTA 下发
	Bound    bool   `json:"bound"`     // 已在控制台绑定
}

// Ready 能否跑回合：身份 + WS 端点都在（token 可为空→触发重 OTA 刷新）。
func (c Credentials) Ready() bool {
	return c.DeviceID != "" && c.ClientID != "" && c.WsURL != ""
}

// ConnectStatus /connect 端点返回。
type ConnectStatus struct {
	Bound          bool   `json:"bound"`
	ActivationCode string `json:"activation_code,omitempty"` // 未绑定时给 Web 展示
	Message        string `json:"message,omitempty"`
}

// Manager 官方服务管理器（并发安全；回合串行化——设备同一时刻一轮 PTT）。
type Manager struct {
	// Creds 读当前凭据（每次现读，Web 改配置即时生效）
	Creds func() Credentials
	// SaveCreds 写回（OTA/激活结果持久化）
	SaveCreds func(Credentials) error
	Log       *log.Logger

	mu      sync.Mutex // 回合串行
	lastErr string
}

func (m *Manager) logf(format string, v ...any) {
	if m.Log != nil {
		m.Log.Printf(format, v...)
	}
}

// Available voice 集成的开关：凭据齐且已绑定（未绑定走回落链路）。
func (m *Manager) Available() bool {
	return m.Creds().Ready() && m.Creds().Bound
}

// LastError 最近一次错误（Web 状态展示）。
func (m *Manager) LastError() string { return m.lastErr }

func (m *Manager) setErr(format string, v ...any) {
	m.lastErr = fmt.Sprintf(format, v...)
	m.logf("%s", m.lastErr)
}

// EnsureIdentity 凭据缺身份时生成并持久化（幂等）。
func (m *Manager) EnsureIdentity() (Credentials, error) {
	c := m.Creds()
	if c.DeviceID != "" && c.ClientID != "" {
		return c, nil
	}
	if c.DeviceID == "" {
		c.DeviceID = NewDeviceID()
	}
	if c.ClientID == "" {
		c.ClientID = newUUID()
	}
	if err := m.SaveCreds(c); err != nil {
		return c, fmt.Errorf("xiaozhi: save identity: %w", err)
	}
	m.logf("identity created: device=%s", c.DeviceID)
	return c, nil
}

// Connect 跑一次 OTA：刷新 ws url/token；未绑定返回激活码（Web 展示，
// 用户到 xiaozhi.me 控制台输入）。绑定状态同步写回。
func (m *Manager) Connect(ctx context.Context) ConnectStatus {
	c, err := m.EnsureIdentity()
	if err != nil {
		m.setErr("connect: %v", err)
		return ConnectStatus{Message: err.Error()}
	}
	wsURL, token, code, err := OTA(ctx, c.OTAURL, c.DeviceID, c.ClientID)
	if err != nil {
		m.setErr("%v", err)
		return ConnectStatus{Message: err.Error()}
	}
	c.WsURL, c.WsToken, c.Bound = wsURL, token, code == ""
	if err := m.SaveCreds(c); err != nil {
		m.setErr("connect: save: %v", err)
		return ConnectStatus{Message: err.Error()}
	}
	m.lastErr = ""
	st := ConnectStatus{Bound: c.Bound, ActivationCode: code}
	if !c.Bound {
		st.Message = "未绑定：请在 xiaozhi.me 控制台输入激活码后点「我已绑定」"
	}
	m.logf("connect: bound=%v code=%q ws=%s", c.Bound, code, wsURL)
	return st
}

// Activate 查询一次绑定结果（202=未绑 / 200=已绑）。Web 每 3s 轮询。
func (m *Manager) Activate(ctx context.Context) bool {
	c := m.Creds()
	if !c.Ready() {
		return false
	}
	bound, err := Activate(ctx, c.OTAURL, c.DeviceID, c.ClientID)
	if err != nil {
		m.setErr("%v", err)
		return false
	}
	if bound && !c.Bound {
		// 绑定完成：re-OTA 刷新 token（绑定前后 token 可能不同）
		if wsURL, token, code, oerr := OTA(ctx, c.OTAURL, c.DeviceID, c.ClientID); oerr == nil {
			c.WsURL, c.WsToken, c.Bound = wsURL, token, code == ""
			_ = m.SaveCreds(c)
		} else {
			m.setErr("re-ota: %v", oerr)
		}
		m.logf("activated: bound=%v", c.Bound)
	}
	if bound {
		m.lastErr = ""
	}
	return bound
}

// AskVoice 一轮官方 PTT：PCM(16k/16bit/mono 小端) 上行，下行 Opus 解码为
// 16k PCM 经 OnAudio 流式回调；返回转写/字幕。设备协议直通接口。
func (m *Manager) AskVoice(ctx context.Context, pcm []byte, cb TurnCallbacks) (TurnResult, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	c := m.Creds()
	if !c.Ready() {
		return TurnResult{}, fmt.Errorf("xiaozhi: not configured")
	}
	res, err := RunTurn(ctx, c.WsURL, c.WsToken, c.DeviceID, c.ClientID, pcm, cb)
	if err != nil {
		m.setErr("turn: %v", err)
		// 鉴权类失败多半是 token 过期：置空 token 提示重连
		return res, err
	}
	m.lastErr = ""
	return res, nil
}
