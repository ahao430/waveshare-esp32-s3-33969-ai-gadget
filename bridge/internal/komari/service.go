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
}

func NewService(src Source) *Service { return &Service{src: src} }

// Nodes 返回缓存视图；超过 TTL 或 force 时重新抓取。
// 抓取失败保留旧快照并标记 stale（无旧数据时 Snapshot=nil）。
func (s *Service) Nodes(ctx context.Context, force bool) StoredView {
	s.mu.Lock()
	needFetch := force || s.snap == nil || time.Since(s.lastAt) > cacheTTL
	s.mu.Unlock()

	if needFetch {
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

	s.mu.Lock()
	defer s.mu.Unlock()
	return StoredView{
		Snapshot:  s.snap,
		Stale:     s.stale,
		LastError: s.errMsg,
		FetchedAt: s.fetched,
	}
}
