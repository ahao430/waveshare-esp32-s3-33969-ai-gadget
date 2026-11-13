// Package kimi 实现 Kimi Coding Plan 订阅额度 Adapter（PLAN §6.7）。
//
// 接口：GET {base}/coding/v1/usages，Bearer Coding Plan 专用 API Key。
// 注意：Kimi Coding Plan 与 Moonshot/Kimi Open Platform API 不是同一计费体系，
// 不能用普通模型 Key 查询。响应结构来自社区记录（CodexBar docs/kimi.md），
// 已知该路径被 Kimi Code 控制台使用、支持专用 API Key，定为 B 级；
// 字段解析对未知结构返回 ErrUnsupportedResponse。真机账号验证后更新 fixtures。
package kimi

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

const (
	// DefaultBaseURLCN 中国区。
	DefaultBaseURLCN = "https://api.kimi.com"
	// DefaultBaseURLIntl 国际区。
	DefaultBaseURLIntl = "https://api.kimi.ai"

	usagesPath       = "/coding/v1/usages"
	requestTimeout   = 10 * time.Second
	maxResponseBytes = 1 << 20
)

// Provider 是 Kimi Code 额度 Provider。
type Provider struct {
	baseURL string
	hc      *http.Client
}

// New 创建 Provider；baseURL 为空时使用中国区。
func New(baseURL string) *Provider {
	if baseURL == "" {
		baseURL = DefaultBaseURLCN
	}
	return &Provider{
		baseURL: strings.TrimRight(baseURL, "/"),
		hc:      &http.Client{Timeout: requestTimeout},
	}
}

func (p *Provider) ID() string          { return "kimi" }
func (p *Provider) DisplayName() string { return "Kimi Coding Plan" }

func (p *Provider) Capabilities() quota.ProviderCapabilities {
	return quota.ProviderCapabilities{
		Trust:      quota.TrustB, // Coding Plan 专用 Key + 控制台使用路径
		CanRefresh: true,
	}
}

func (p *Provider) FetchQuota(ctx context.Context, account quota.AccountRef) (*quota.QuotaSnapshot, error) {
	if account.APIKey == "" {
		return nil, errors.New("missing_api_key")
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, p.baseURL+usagesPath, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+account.APIKey)
	req.Header.Set("Accept", "application/json")

	resp, err := p.hc.Do(req)
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

	windows, err := parseUsagesResponse(body)
	if err != nil {
		return nil, fmt.Errorf("usages: %w", err)
	}
	return &quota.QuotaSnapshot{
		ProviderID: p.ID(),
		AccountID:  account.ID,
		FetchedAt:  time.Now(),
		Status:     quota.StatusOK,
		Windows:    windows,
		RawVersion: "kimi-usages-v1",
	}, nil
}

// usagesResponse 社区记录的响应结构（数值均为字符串）。
type usagesResponse struct {
	Usage *struct {
		Limit     string `json:"limit"`
		Used      string `json:"used"`
		Remaining string `json:"remaining"`
		ResetTime string `json:"resetTime"`
	} `json:"usage"`
	Limits []struct {
		Window *struct {
			Duration int    `json:"duration"`
			TimeUnit string `json:"timeUnit"`
		} `json:"window"`
		Detail *struct {
			Limit     string `json:"limit"`
			Used      string `json:"used"`
			Remaining string `json:"remaining"`
			ResetTime string `json:"resetTime"`
		} `json:"detail"`
	} `json:"limits"`
}

// parseUsagesResponse 解析 /coding/v1/usages。
// 映射（PLAN §6.7）：
//   - usage                → 月度/总量池
//   - limits[].window duration=300 & TIME_UNIT_MINUTE → 5 小时限流窗口
//   - 其余 limits 条目保留为通用窗口（含每周，接口实际返回时自然命中）
func parseUsagesResponse(body []byte) ([]quota.QuotaWindow, error) {
	var r usagesResponse
	if err := json.Unmarshal(body, &r); err != nil {
		return nil, fmt.Errorf("%w: bad json: %v", quota.ErrUnsupportedResponse, err)
	}
	if r.Usage == nil && len(r.Limits) == 0 {
		return nil, fmt.Errorf("%w: no usage/limits", quota.ErrUnsupportedResponse)
	}

	var windows []quota.QuotaWindow
	if r.Usage != nil {
		windows = append(windows, buildWindow("monthly", "Monthly/Total Pool", 0, r.Usage.Limit, r.Usage.Used, r.Usage.ResetTime, "usage"))
	}
	for i, lim := range r.Limits {
		if lim.Window == nil || lim.Detail == nil {
			continue
		}
		var (
			id    = fmt.Sprintf("limit_%d", i)
			label = fmt.Sprintf("Limit window #%d", i)
			secs  int64
		)
		if lim.Window.TimeUnit == "TIME_UNIT_MINUTE" && lim.Window.Duration == 300 {
			id, label, secs = "5h", "5-hour window", 5*3600
		}
		src := fmt.Sprintf("limits[%d]", i)
		windows = append(windows, buildWindow(id, label, secs, lim.Detail.Limit, lim.Detail.Used, lim.Detail.ResetTime, src))
	}
	if len(windows) == 0 {
		return nil, fmt.Errorf("%w: limits entries missing window/detail", quota.ErrUnsupportedResponse)
	}
	return windows, nil
}

// buildWindow 由字符串数值构造 QuotaWindow；解析失败的字段保持缺失（nil）。
func buildWindow(id, label string, windowSecs int64, limit, used, resetTime, src string) quota.QuotaWindow {
	w := quota.QuotaWindow{
		ID:            id,
		Label:         label,
		WindowSeconds: windowSecs,
		Unit:          "quota",
		SourceField:   src,
	}
	if v, ok := parseFloat(limit); ok {
		w.Limit = &v
	}
	if v, ok := parseFloat(used); ok {
		w.Used = &v
	}
	if w.Limit != nil && w.Used != nil && *w.Limit > 0 {
		pct := *w.Used / *w.Limit * 100
		w.UsedPercent = &pct
		rem := 100 - pct
		w.RemainingPct = &rem
	}
	if t, err := time.Parse(time.RFC3339, resetTime); err == nil {
		utc := t.UTC()
		w.ResetAt = &utc
	}
	return w
}

func parseFloat(s string) (float64, bool) {
	var v float64
	if s == "" {
		return 0, false
	}
	if _, err := fmt.Sscanf(s, "%g", &v); err != nil {
		return 0, false
	}
	return v, true
}
