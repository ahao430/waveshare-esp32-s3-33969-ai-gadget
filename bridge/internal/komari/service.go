package komari

import (
	"context"
	"sync"
	"time"
)

// cacheTTL：前台 30~60s 刷新（PLAN §5.8），Bridge 侧统一 30s 节流，
// 避免多客户端（Web+设备）叠加打爆上游。
const cacheTTL = 30 * time.Second

// StoredView 是 API 输出视图：快照 + 过期/错误状态（与 quota 域同语义）。
type StoredView struct {
	Snapshot  *Snapshot `json:"snapshot,omitempty"`
	Stale     bool      `json:"stale"`
	LastError string    `json:"last_error,omitempty"`
	FetchedAt time.Time `json:"fetched_at"`
}

// Service 包装一个 Source：缓存 + 失败保留旧数据。
type Service struct {
	src Source

	mu      sync.Mutex
	snap    *Snapshot
	stale   bool
	errMsg  string
	lastAt  time.Time
	fetched time.Time
	// fetching：后台刷新单飞（设备端 HTTP 超时 1.5s，不能让请求同步等上游）
	fetching bool
}

func NewService(src Source) *Service { return &Service{src: src} }

// Nodes 返回缓存视图。
//
// 关键约束（2026-11-15 用户反馈"监控页显示是空的"）：设备端 HTTP 超时
// 只有 1500ms，而同步抓上游 Komari 实测 1.3~1.5s——同步等待会让设备
// 每次都超时。所以：
//   - force（显式刷新按钮）：同步抓，调用方愿意等
//   - 有旧快照且过期：立即返回旧数据（stale=true），后台刷新
//   - 冷启动无数据：后台抓一次，本次返回空快照，设备下次刷新就有
//   - 未过期：直接返回缓存
func (s *Service) Nodes(ctx context.Context, force bool) StoredView {
	if force {
		s.refreshSync(ctx)
	} else {
		s.mu.Lock()
		expired := s.snap == nil || time.Since(s.lastAt) > cacheTTL
		haveSnap := s.snap != nil
		if expired {
			s.stale = haveSnap // 有旧数据时标记为"陈旧但可用"
		}
		s.mu.Unlock()
		if expired {
			s.refreshAsync()
		}
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	return StoredView{
		Snapshot:  s.snap,
		Stale:     s.stale,
		LastError: s.errMsg,
		FetchedAt: s.fetched,
	}
}

// Warm 启动时预热缓存（不阻塞启动）。
func (s *Service) Warm(ctx context.Context) { s.refreshSync(ctx) }

// refreshSync 同步抓取并写缓存（force/Warm 用）。
func (s *Service) refreshSync(ctx context.Context) {
	fetchCtx, cancel := context.WithTimeout(ctx, requestTimeout)
	snap, err := s.src.Fetch(fetchCtx)
	cancel()

	s.mu.Lock()
	if err != nil {
		s.stale = true
		s.errMsg = err.Error()
		s.lastAt = time.Now()
	} else {
		s.snap = snap
		s.stale = false
		s.errMsg = ""
		s.lastAt = time.Now()
		s.fetched = snap.FetchedAt
	}
	s.mu.Unlock()
}

// refreshAsync 后台刷新（单飞：已有抓取在跑就不重复起）。
func (s *Service) refreshAsync() {
	s.mu.Lock()
	if s.fetching {
		s.mu.Unlock()
		return
	}
	s.fetching = true
	s.mu.Unlock()

	go func() {
		defer func() {
			s.mu.Lock()
			s.fetching = false
			s.mu.Unlock()
		}()
		// 不继承请求 ctx：设备断开不该取消上游抓取
		ctx, cancel := context.WithTimeout(context.Background(), requestTimeout)
		defer cancel()
		s.refreshSync(ctx)
	}()
}

// dynamicSource 每次抓取时重新解析底层 Source（Web 后台改配置即时生效，
// 无需重启 Bridge；PLAN §5.8 监控源可配置）。
type dynamicSource struct {
	resolve func() Source
}

// NewDynamicSource 把"按配置现取 Source"适配为 Source；
// resolve 返回 nil（未配置）→ 空快照 source="none"。
// 不回退 mock：设备/Web 端「空的就显示空的」，提示去后台配置。
func NewDynamicSource(resolve func() Source) Source {
	return &dynamicSource{resolve: resolve}
}

func (d *dynamicSource) Name() string {
	if s := d.resolve(); s != nil {
		return s.Name()
	}
	return "none"
}

func (d *dynamicSource) Fetch(ctx context.Context) (*Snapshot, error) {
	s := d.resolve()
	if s == nil {
		// Servers 必须显式空 slice：nil slice 序列化为 null，设备端
		// cJSON_IsArray 判 false 会走「结构异常」路径不清 server_count
		return &Snapshot{FetchedAt: time.Now(), Source: "none", Servers: []Server{}}, nil
	}
	return s.Fetch(ctx)
}
