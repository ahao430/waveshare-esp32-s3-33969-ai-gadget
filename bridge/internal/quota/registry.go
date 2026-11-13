package quota

import (
	"fmt"
	"sort"
	"sync"
)

// Registry 管理 Provider 的注册与查找。非并发安全场景由 Store/API 层加锁。
type Registry struct {
	mu        sync.RWMutex
	providers map[string]QuotaProvider
}

func NewRegistry() *Registry {
	return &Registry{providers: make(map[string]QuotaProvider)}
}

// Register 注册一个 Provider；ID 冲突直接返回错误（启动期问题，fail-fast）。
func (r *Registry) Register(p QuotaProvider) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	id := p.ID()
	if id == "" {
		return fmt.Errorf("quota: provider with empty id")
	}
	if _, dup := r.providers[id]; dup {
		return fmt.Errorf("quota: duplicate provider id %q", id)
	}
	r.providers[id] = p
	return nil
}

// Get 按 ID 查找。
func (r *Registry) Get(id string) (QuotaProvider, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	p, ok := r.providers[id]
	return p, ok
}

// InfoEntry 是 /api/v1/quota/providers 的条目。
type InfoEntry struct {
	ID           string              `json:"id"`
	DisplayName  string              `json:"display_name"`
	Capabilities ProviderCapabilities `json:"capabilities"`
}

// List 返回按 ID 排序的 Provider 信息列表。
func (r *Registry) List() []InfoEntry {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]InfoEntry, 0, len(r.providers))
	for _, p := range r.providers {
		out = append(out, InfoEntry{
			ID:           p.ID(),
			DisplayName:  p.DisplayName(),
			Capabilities: p.Capabilities(),
		})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}
