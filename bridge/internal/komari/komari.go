// Package komari 对接 Komari 服务器监控（PLAN §3.3 / §5.8）。
//
// ⚠️ 接口可信度：C 级。`/api/nodes` 等路径来自社区讨论线索，不是 Komari
// 承诺的稳定公共 API；字段结构按社区观察整理（见 testdata fixture），
// 必须对照实际部署版本验证后再调整 fixture 与解析（PLAN §3.3 要求）。
// 解析对未知结构返回 ErrUnsupportedResponse，绝不伪造数据。
package komari

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	"gadget-bridge/internal/quota"
)

// ErrUnsupportedResponse 与 quota 域同语义：结构无法解析。
var ErrUnsupportedResponse = errors.New("unsupported_response")

// Server 是一台被监控服务器的摘要（PLAN §5.8 展示字段）。
type Server struct {
	UUID       string    `json:"uuid"`
	Name       string    `json:"name"`
	Online     bool      `json:"online"`
	CPUPct     *float64  `json:"cpu_pct,omitempty"`    // 0~100；nil=未提供
	MemPct     *float64  `json:"mem_pct,omitempty"`    // 0~100
	DiskPct    *float64  `json:"disk_pct,omitempty"`   // 0~100
	NetUpBps   *int64    `json:"net_up_bps,omitempty"` // 字节/秒
	NetDownBps *int64    `json:"net_down_bps,omitempty"`
	UptimeS    *int64    `json:"uptime_s,omitempty"`
	LastReport time.Time `json:"last_report"`
}

// Snapshot 是一次抓取的全部服务器列表。
type Snapshot struct {
	FetchedAt time.Time `json:"fetched_at"`
	Source    string    `json:"source"` // "mock" | "komari(<url>)"
	Servers   []Server  `json:"servers"`
}

// Source 是监控数据源（真实 Komari / Mock 统一接口）。
type Source interface {
	Name() string
	Fetch(ctx context.Context) (*Snapshot, error)
}

// ---- 真实 Komari 客户端（C 级） ----

const (
	requestTimeout   = 10 * time.Second
	maxResponseBytes = 1 << 20
)

// Client 抓取 Komari 服务端。
type Client struct {
	baseURL string
	token   string // Komari 部署开启鉴权时使用（Bearer）
	hc      *http.Client
}

// NewClient 创建客户端；baseURL 形如 https://komari.example.com。
func NewClient(baseURL, token string) *Client {
	return &Client{
		baseURL: strings.TrimRight(baseURL, "/"),
		token:   token,
		hc:      &http.Client{Timeout: requestTimeout},
	}
}

func (c *Client) Name() string { return "komari(" + c.baseURL + ")" }

// Fetch 拉取节点列表与实时状态。
// 首选 Komari JSON-RPC（/api/rpc2，实测 1.1.x/2.x 均可用）：
//
//	common:getNodes              → 静态信息（名称等）
//	common:getNodesLatestStatus  → CPU/内存/磁盘/网络/在线/运行时长
//
// 失败回退社区观察的 GET /api/nodes（只有静态信息，指标为 nil）。
func (c *Client) Fetch(ctx context.Context) (*Snapshot, error) {
	if snap, err := c.fetchRPC(ctx); err == nil {
		return snap, nil
	}
	return c.fetchREST(ctx)
}

// Fetch 回退路径：GET /api/nodes。
func (c *Client) fetchREST(ctx context.Context) (*Snapshot, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.baseURL+"/api/nodes", nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/json")
	if c.token != "" {
		req.Header.Set("Authorization", "Bearer "+c.token)
	}

	resp, err := c.hc.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("http_status_%d", resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, maxResponseBytes))
	if err != nil {
		return nil, err
	}

	servers, err := ParseNodes(body)
	if err != nil {
		return nil, fmt.Errorf("nodes: %w", err)
	}
	return &Snapshot{FetchedAt: time.Now(), Source: c.Name(), Servers: servers}, nil
}

// rawServer 宽松字段名：社区观察到不同版本字段命名有差异
// （uuid/id、cpu/process_cpu、load 等），逐一尝试。
type rawServer struct {
	UUID   string `json:"uuid"`
	ID     string `json:"id"`
	Name   string `json:"name"`
	Tag    string `json:"tag"`
	Online *bool  `json:"online"`

	CPU   *float64 `json:"cpu"`
	CPU2  *float64 `json:"cpu_percent"`
	Load1 *float64 `json:"load_1"`

	Mem   *float64 `json:"mem"`
	MemP  *float64 `json:"mem_percent"`
	Disk  *float64 `json:"disk"`
	DiskP *float64 `json:"disk_percent"`

	NetIn  *int64 `json:"net_in"`  // 下载（字节/秒）
	NetOut *int64 `json:"net_out"` // 上传
	Down   *int64 `json:"download"`
	Up     *int64 `json:"upload"`

	Uptime *int64 `json:"uptime"`
}

// ParseNodes 解析 /api/nodes 响应：兼容顶层数组或 {"data": []}。
// 百分比语义：>1 视为已是百分数（0~100），≤1 视为比例（0~1）。
func ParseNodes(body []byte) ([]Server, error) {
	var arr []rawServer
	if err := json.Unmarshal(body, &arr); err != nil {
		var wrapped struct {
			Data []rawServer `json:"data"`
		}
		if err2 := json.Unmarshal(body, &wrapped); err2 != nil || wrapped.Data == nil {
			return nil, fmt.Errorf("%w: not an array nor {data:[]}", quota.ErrUnsupportedResponse)
		}
		arr = wrapped.Data
	}
	if len(arr) == 0 {
		return nil, fmt.Errorf("%w: empty nodes", ErrUnsupportedResponse)
	}

	out := make([]Server, 0, len(arr))
	for _, r := range arr {
		s := Server{
			UUID:       firstNonEmpty(r.UUID, r.ID),
			Name:       firstNonEmpty(r.Name, r.Tag, r.UUID, r.ID),
			Online:     r.Online == nil || *r.Online,
			CPUPct:     normPct(firstPtr(r.CPU, r.CPU2)),
			MemPct:     normPct(firstPtr(r.Mem, r.MemP)),
			DiskPct:    normPct(firstPtr(r.Disk, r.DiskP)),
			NetDownBps: firstPtr(r.NetIn, r.Down),
			NetUpBps:   firstPtr(r.NetOut, r.Up),
			UptimeS:    r.Uptime,
			LastReport: time.Now(),
		}
		out = append(out, s)
	}
	return out, nil
}

// normPct 统一到 0~100。
func normPct(v *float64) *float64 {
	if v == nil {
		return nil
	}
	x := *v
	if x <= 1 {
		x *= 100
	}
	return &x
}

func firstNonEmpty(vs ...string) string {
	for _, v := range vs {
		if v != "" {
			return v
		}
	}
	return ""
}

func firstPtr[T any](vs ...*T) *T {
	for _, v := range vs {
		if v != nil {
			return v
		}
	}
	return nil
}

// ---- JSON-RPC（/api/rpc2）----

type rpcEnvelope struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      int             `json:"id"`
	Result  json.RawMessage `json:"result"`
	Error   *struct {
		Code    int    `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
}

func (c *Client) rpc(ctx context.Context, id int, method string, params any) (json.RawMessage, error) {
	body, err := json.Marshal(map[string]any{
		"jsonrpc": "2.0", "id": id, "method": method, "params": params,
	})
	if err != nil {
		return nil, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+"/api/rpc2", strings.NewReader(string(body)))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	if c.token != "" {
		req.Header.Set("Authorization", "Bearer "+c.token)
	}
	resp, err := c.hc.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("rpc_http_%d", resp.StatusCode)
	}
	raw, err := io.ReadAll(io.LimitReader(resp.Body, maxResponseBytes))
	if err != nil {
		return nil, err
	}
	var env rpcEnvelope
	if err := json.Unmarshal(raw, &env); err != nil {
		return nil, err
	}
	if env.Error != nil {
		return nil, fmt.Errorf("rpc_error: %s", env.Error.Message)
	}
	if len(env.Result) == 0 {
		return nil, ErrUnsupportedResponse
	}
	return env.Result, nil
}

type rpcNodeInfo struct {
	UUID string `json:"uuid"`
	Name string `json:"name"`
}

type rpcStatus struct {
	CPU       *float64 `json:"cpu"` // 0~100
	Ram       *float64 `json:"ram"`
	RamTotal  *float64 `json:"ram_total"`
	Disk      *float64 `json:"disk"`
	DiskTotal *float64 `json:"disk_total"`
	NetIn     *int64   `json:"net_in"`  // 入站（下载）字节/秒
	NetOut    *int64   `json:"net_out"` // 出站（上传）
	Online    *bool    `json:"online"`
	Uptime    *int64   `json:"uptime"`
}

func pctOf(used, total *float64) *float64 {
	if used == nil || total == nil || *total <= 0 {
		return nil
	}
	v := *used / *total * 100
	return &v
}

// fetchRPC 组装静态信息 + 实时状态（按 uuid 合并）。
func (c *Client) fetchRPC(ctx context.Context) (*Snapshot, error) {
	nodesRaw, err := c.rpc(ctx, 1, "common:getNodes", map[string]any{})
	if err != nil {
		return nil, err
	}
	statusRaw, err := c.rpc(ctx, 2, "common:getNodesLatestStatus", map[string]any{})
	if err != nil {
		return nil, err
	}

	var nodes map[string]rpcNodeInfo
	if err := json.Unmarshal(nodesRaw, &nodes); err != nil {
		return nil, fmt.Errorf("rpc nodes: %w", err)
	}
	var statuses map[string]rpcStatus
	if err := json.Unmarshal(statusRaw, &statuses); err != nil {
		return nil, fmt.Errorf("rpc status: %w", err)
	}
	if len(nodes) == 0 {
		return nil, fmt.Errorf("%w: empty nodes", ErrUnsupportedResponse)
	}

	now := time.Now()
	out := make([]Server, 0, len(nodes))
	for uuid, n := range nodes {
		srv := Server{UUID: uuid, Name: n.Name, Online: false, LastReport: now}
		if n.Name == "" {
			srv.Name = uuid
		}
		if st, ok := statuses[uuid]; ok {
			srv.CPUPct = st.CPU
			srv.MemPct = pctOf(st.Ram, st.RamTotal)
			srv.DiskPct = pctOf(st.Disk, st.DiskTotal)
			srv.NetDownBps = st.NetIn
			srv.NetUpBps = st.NetOut
			srv.UptimeS = st.Uptime
			if st.Online != nil {
				srv.Online = *st.Online
			}
		}
		out = append(out, srv)
	}
	return &Snapshot{FetchedAt: now, Source: c.Name(), Servers: out}, nil
}
