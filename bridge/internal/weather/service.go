package weather

import (
	"context"
	"log/slog"
	"sync"
	"time"
)

// cacheTTL 缓存时长（设备拉取永远命中缓存；后台 goroutine 保温）。
const cacheTTL = 10 * time.Minute

// View API 输出视图（快照 + 过期/错误状态，quota 域同语义）。
type View struct {
	Snapshot  *Snapshot `json:"snapshot,omitempty"`
	Stale     bool      `json:"stale"`
	LastError string    `json:"last_error,omitempty"`
	FetchedAt time.Time `json:"fetched_at"`
}

// ResolveConfig 一次定位解析所需的配置（main 从 app_settings 现读）。
type ResolveConfig struct {
	Mode string // auto|manual
	City string // manual 模式的城市名
}

// Service 缓存 + 保温。
type Service struct {
	client  *Client
	resolve func() ResolveConfig
	logger  *slog.Logger

	mu       sync.Mutex
	warmOnce sync.Once
	snap     *Snapshot
	stale    bool
	err      string
	last     time.Time // 上次抓取尝试时间
}

// NewService resolve 每次抓取前现读配置（Web 改城市 10 分钟内生效，
// 强制刷新立即生效）。
func NewService(client *Client, resolve func() ResolveConfig, logger *slog.Logger) *Service {
	if logger == nil {
		logger = slog.Default()
	}
	return &Service{client: client, resolve: resolve, logger: logger}
}

// Warm 启动后台保温（启动即抓一次 + 每 TTL 刷新；配置变更靠 TTL 收敛
// 或 force）。必须在使用前启动一次；重复调用忽略。
func (s *Service) Warm(ctx context.Context) {
	s.warmOnce.Do(func() {
		go func() {
			s.Refresh(context.Background()) // 启动预热（失败不影响服务）
			t := time.NewTicker(cacheTTL)
			defer t.Stop()
			for {
				select {
				case <-t.C:
					s.Refresh(context.Background())
				case <-ctx.Done():
					return
				}
			}
		}()
	})
}

// Refresh 强制抓一次（成功覆盖缓存；失败保留旧数据并标 stale）。
func (s *Service) Refresh(ctx context.Context) {
	cfg := s.resolve()
	if cfg.Mode != "manual" {
		cfg.Mode = "auto"
	}
	snap, err := s.fetch(ctx, cfg)
	s.mu.Lock()
	defer s.mu.Unlock()
	s.last = time.Now()
	if err != nil {
		s.stale = true
		s.err = err.Error()
		s.logger.Warn("weather refresh failed", "mode", cfg.Mode, "err", err.Error())
		return
	}
	s.snap = snap
	s.stale = false
	s.err = ""
	s.logger.Info("weather refreshed", "location", snap.Location, "mode", snap.Mode)
}

func (s *Service) fetch(ctx context.Context, cfg ResolveConfig) (*Snapshot, error) {
	var lat, lon float64
	var name string
	var err error
	if cfg.Mode == "manual" {
		lat, lon, name, err = s.client.LocateByCity(ctx, cfg.City)
	} else {
		lat, lon, name, err = s.client.LocateByIP(ctx)
	}
	if err != nil {
		return nil, err
	}
	cur, daily, ferr := s.client.Forecast(ctx, lat, lon)
	if ferr != nil {
		return nil, ferr
	}
	return &Snapshot{
		Location: TrimCity(name), Mode: cfg.Mode,
		FetchedAt: time.Now().UTC(), Current: cur, Daily: daily,
	}, nil
}

// Get 缓存视图；超 TTL 且未保温时惰性抓取（保温常开，一般直接命中）。
func (s *Service) Get(ctx context.Context, force bool) View {
	if force || time.Since(s.lastFetch()) > cacheTTL {
		s.Refresh(ctx)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return View{Snapshot: s.snap, Stale: s.stale, LastError: s.err,
		FetchedAt: s.snapTime()}
}

// Location 当前（缓存中的）定位名——配置页预览用；空=尚无数据。
func (s *Service) Location() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.snap == nil {
		return ""
	}
	return s.snap.Location
}

func (s *Service) lastFetch() time.Time { s.mu.Lock(); defer s.mu.Unlock(); return s.last }
func (s *Service) snapTime() time.Time {
	if s.snap == nil {
		return time.Time{}
	}
	return s.snap.FetchedAt
}
