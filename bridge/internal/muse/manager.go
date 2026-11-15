// manager.go — Muse 账号/VM 解析与会话生命周期管理。
//
// 凭据来自配对后设备 NVS 导出的四项（access_token/refresh_token/node_id/
// noise_host）+ 账号 API 地址；fetch_vms 换出 vm_id/vm_auth_token。
// 会话按凭据指纹缓存，凭据一改（Web 保存）下次请求自动重建。
package muse

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"sync"
	"time"
)

// Credentials 一组配对凭据（Bridge 侧加密保存）。
type Credentials struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token,omitempty"`
	NodeID       string `json:"node_id"`
	NoiseHost    string `json:"noise_host,omitempty"`
	APIURL       string `json:"api_url,omitempty"` // 默认 https://api.muse.ai
	ProxyURL     string `json:"proxy_url,omitempty"`
}

// Ready 凭据是否够建立会话。
func (c Credentials) Ready() bool {
	return c.AccessToken != "" && c.NodeID != ""
}

func (c Credentials) apiURL() string {
	if c.APIURL != "" {
		return c.APIURL
	}
	return "https://api.muse.ai"
}

// VM 一个 Muse VM（fetch_vms 的一行）。
type VM struct {
	ID        string `json:"vm_id"`
	WSURL     string `json:"vm_ws_url"`
	Name      string `json:"vm_name"`
	AuthToken string `json:"vm_auth_token"`
	IsDefault bool   `json:"is_default"`
}

// FetchVMs 用 access token 拉 VM 列表（Bearer，走代理）。
func FetchVMs(ctx context.Context, c Credentials) ([]VM, error) {
	if c.AccessToken == "" {
		return nil, fmt.Errorf("muse: 未配置 access token")
	}
	tr := &http.Transport{}
	if c.ProxyURL != "" {
		pu, err := url.Parse(c.ProxyURL)
		if err != nil {
			return nil, fmt.Errorf("muse: proxy: %w", err)
		}
		tr.Proxy = http.ProxyURL(pu)
	} else {
		tr.Proxy = http.ProxyFromEnvironment
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.apiURL()+"/fetch_vms", nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+c.AccessToken)
	resp, err := (&http.Client{Transport: tr, Timeout: 30 * time.Second}).Do(req)
	if err != nil {
		return nil, fmt.Errorf("muse: fetch_vms: %w", err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("muse: fetch_vms http %d: %.160s", resp.StatusCode, string(body))
	}
	var out struct {
		VMList []VM `json:"vm_list"`
	}
	if err := json.Unmarshal(body, &out); err != nil {
		return nil, fmt.Errorf("muse: fetch_vms 解析: %w", err)
	}
	return out.VMList, nil
}

// Manager 按需建立/复用 Agent；线程安全。
type Manager struct {
	// Creds 每次取最新配置（保存即生效）；nil 或 !Ready 视为未配置。
	Creds func() Credentials
	Log   *log.Logger

	mu      sync.Mutex
	agent   *Agent
	agentFP string // 建会话时的凭据指纹
	lastErr error
}

// Available 是否已配置可用凭据（不断言服务可达）。
func (m *Manager) Available() bool {
	if m.Creds == nil {
		return false
	}
	return m.Creds().Ready()
}

func fingerprint(c Credentials) string {
	// 只用于判断"凭据变了没"，不落盘、不打印
	h := c.NodeID + "|" + c.NoiseHost + "|" + c.APIURL + "|" + c.ProxyURL
	if len(c.AccessToken) > 12 {
		h += "|" + c.AccessToken[:8] + fmt.Sprint(len(c.AccessToken))
	}
	return h
}

// agentFor 取（必要时新建）当前凭据对应的会话。
func (m *Manager) agentFor(ctx context.Context) (*Agent, error) {
	c := m.Creds()
	fp := fingerprint(c)

	m.mu.Lock()
	if m.agent != nil && m.agentFP == fp {
		a := m.agent
		select {
		case <-a.Done():
			m.agent = nil // 已断，重建
		default:
			m.mu.Unlock()
			return a, nil
		}
	}
	m.mu.Unlock()

	vms, err := FetchVMs(ctx, c)
	if err != nil {
		m.setErr(err)
		return nil, err
	}
	if len(vms) == 0 {
		err := fmt.Errorf("muse: 账号下没有 VM")
		m.setErr(err)
		return nil, err
	}
	vm := vms[0]
	for _, v := range vms {
		if v.IsDefault {
			vm = v
		}
	}
	a, err := NewAgent(ctx, Config{
		NoiseHost: c.NoiseHost, VMID: vm.ID, Token: vm.AuthToken,
		ProxyURL: c.ProxyURL, Logger: m.Log,
	})
	if err != nil {
		m.setErr(err)
		return nil, err
	}
	m.mu.Lock()
	old := m.agent
	m.agent, m.agentFP, m.lastErr = a, fp, nil
	m.mu.Unlock()
	if old != nil {
		old.Close()
	}
	if m.Log != nil {
		m.Log.Printf("muse: 会话就绪 vm=%s host=%s", vm.ID, c.NoiseHost)
	}
	return a, nil
}

func (m *Manager) setErr(err error) {
	m.mu.Lock()
	m.lastErr = err
	m.mu.Unlock()
}

// LastError 最近一次连接错误（Web 诊断用）。
func (m *Manager) LastError() string {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.lastErr == nil {
		return ""
	}
	return m.lastErr.Error()
}

// Close 关掉缓存的会话（Bridge 退出时）。
func (m *Manager) Close() {
	m.mu.Lock()
	a := m.agent
	m.agent = nil
	m.agentFP = ""
	m.mu.Unlock()
	if a != nil {
		a.Close()
	}
}

// Ask 一个语音回合：PCM(16k) → (heard, reply)。
func (m *Manager) Ask(ctx context.Context, pcm []byte,
	onHeard, onText func(string)) (string, string, error) {
	a, err := m.agentFor(ctx)
	if err != nil {
		return "", "", err
	}
	heard, reply, err := a.AskVoice(ctx, pcm, onHeard, onText)
	if err != nil {
		// 会话可能已死：清缓存，下次请求重建
		m.mu.Lock()
		if m.agent == a {
			m.agent = nil
			m.agentFP = ""
		}
		m.mu.Unlock()
		a.Close()
		m.setErr(err)
		return "", "", err
	}
	return heard, reply.Text, nil
}

// AskText 文本回合（探针/调试用）。
func (m *Manager) AskText(ctx context.Context, text string, onText func(string)) (string, error) {
	a, err := m.agentFor(ctx)
	if err != nil {
		return "", err
	}
	r, err := a.Chat(ctx, text, onText)
	if err != nil {
		return "", err
	}
	return r.Text, nil
}
