package quota

import (
	"sync"
	"time"
)

// StoredSnapshot 是 API 层对外暴露的快照视图：
// 保留上一次成功数据 + 数据过期/错误状态（PLAN §6.1 规则）。
type StoredSnapshot struct {
	Snapshot  *QuotaSnapshot `json:"snapshot,omitempty"`
	Stale     bool           `json:"stale"`                // true=展示的是上次成功数据，最近一次抓取失败
	LastError string         `json:"last_error,omitempty"` // 最近一次抓取错误（已脱敏）
	FetchedAt time.Time      `json:"fetched_at"`           // 上一次成功抓取时间；零值=从未成功
}

// Persister 是 Store 的可选持久层（internal/storage 实现）。
type Persister interface {
	SaveStored(providerID string, view StoredSnapshot) error
	LoadAll() (map[string]StoredSnapshot, []error)
}

// Store 保存各 Provider 最近一次成功快照与错误状态。
// 可注入 Persister（SQLite）；nil 时纯内存（开发/测试）。
type Store struct {
	mu      sync.Mutex
	entries map[string]*StoredSnapshot
	persist Persister
}

// NewStore 创建内存 Store；用 AttachPersister 挂持久层。
func NewStore() *Store {
	return &Store{entries: make(map[string]*StoredSnapshot)}
}

// AttachPersister 挂载持久层并恢复历史快照（已有内存数据不覆盖）。
func (s *Store) AttachPersister(p Persister) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.persist = p
	loaded, errs := p.LoadAll()
	for id, view := range loaded {
		if _, exists := s.entries[id]; !exists {
			v := view // 拷贝
			s.entries[id] = &v
		}
	}
	if len(errs) > 0 {
		return errs[0]
	}
	return nil
}

// Get 返回某 Provider 的当前视图；从未抓取时返回零值视图（Snapshot=nil）。
func (s *Store) Get(providerID string) StoredSnapshot {
	s.mu.Lock()
	defer s.mu.Unlock()
	if e, ok := s.entries[providerID]; ok {
		return *e
	}
	return StoredSnapshot{}
}

// SaveSuccess 记录一次成功抓取，清除错误状态。
func (s *Store) SaveSuccess(providerID string, snap *QuotaSnapshot) {
	s.mu.Lock()
	view := &StoredSnapshot{
		Snapshot:  snap,
		Stale:     false,
		FetchedAt: snap.FetchedAt,
	}
	s.entries[providerID] = view
	persist := s.persist
	s.mu.Unlock()

	if persist != nil {
		_ = persist.SaveStored(providerID, *view) // 持久化失败不影响内存态；日志由调用方记
	}
}

// SaveError 记录一次失败：保留旧快照并标记 stale（如有旧数据），
// 没有旧数据时 Snapshot 保持 nil，由 API 层返回错误态。
func (s *Store) SaveError(providerID, errMsg string) {
	s.mu.Lock()
	prev := s.entries[providerID]
	next := &StoredSnapshot{Stale: true, LastError: errMsg}
	if prev != nil {
		next.Snapshot = prev.Snapshot
		next.FetchedAt = prev.FetchedAt
	}
	s.entries[providerID] = next
	persist := s.persist
	s.mu.Unlock()

	if persist != nil {
		_ = persist.SaveStored(providerID, *next)
	}
}
