// Package mock 提供确定性的 Mock QuotaProvider，用于开发与端到端联调。
// 数据在每次抓取时基于时间伪随机变化，模拟真实接口行为。
package mock

import (
	"context"
	"time"

	"gadget-bridge/internal/quota"
)

// Provider 是 Mock Provider（Trust A：本地数据，无外部依赖）。
type Provider struct{}

func New() *Provider { return &Provider{} }

func (p *Provider) ID() string          { return "mock" }
func (p *Provider) DisplayName() string { return "Mock Provider" }

func (p *Provider) Capabilities() quota.ProviderCapabilities {
	return quota.ProviderCapabilities{
		Trust:        quota.TrustA,
		CanRefresh:   true,
		CanPrewarm:   false,
		Experimental: false,
	}
}

func (p *Provider) FetchQuota(ctx context.Context, account quota.AccountRef) (*quota.QuotaSnapshot, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	now := time.Now()
	// 以分钟为粒度的时间伪随机，避免每次请求剧烈跳变。
	seed := now.Unix() / 60

	used5h := float64(seed%73 + 5)            // 5~77%
	usedWeek := float64((seed/7)%61 + 10)     // 10~70%
	reset5h := now.Add(time.Duration(25+seed%180) * time.Minute)
	resetWeek := now.Add(time.Duration(30+seed%300) * time.Hour)

	pct := func(v float64) *float64 { return &v }

	return &quota.QuotaSnapshot{
		ProviderID: p.ID(),
		AccountID:  account.ID,
		PlanName:   "GLM Coding Plan (mock)",
		FetchedAt:  now,
		Status:     quota.StatusOK,
		Windows: []quota.QuotaWindow{
			{
				ID:            "5h",
				Label:         "5 小时窗口",
				WindowSeconds: 5 * 3600,
				UsedPercent:   pct(used5h),
				RemainingPct:  pct(100 - used5h),
				ResetAt:       &reset5h,
				Unit:          "percent",
				SourceField:   "mock",
			},
			{
				ID:            "weekly",
				Label:         "每周额度",
				WindowSeconds: 7 * 86400,
				UsedPercent:   pct(usedWeek),
				RemainingPct:  pct(100 - usedWeek),
				ResetAt:       &resetWeek,
				Unit:          "percent",
				SourceField:   "mock",
			},
		},
		RawVersion: "mock-v1",
	}, nil
}
