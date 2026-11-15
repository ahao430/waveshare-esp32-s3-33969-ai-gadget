// Package codingplan — GPT/Claude/MiniMax/MiMo 等"Coding Plan 中转订阅"
// 的通用额度 Provider（B/C 级）。
//
// 背景：这几家的 Coding Plan 在国内多经中转站售卖，站点各自实现额度
// 接口，但事实标准是两种形状（参照 AIVault 的 kimi/newapi 适配器）：
//
//   形状一（kimi 兼容，api.kimi.com 同款）：
//     GET {base}/usages  Bearer
//     {"usage":{"limit":"…","remaining":"…","resetTime":"ISO"},   ← 周
//      "limits":[{"window":{"duration":300,"timeUnit":"MINUTES"},
//                 "detail":{"limit","remaining","resetTime"}}],   ← 5 小时
//      "user":{"membership":{"level":"LEVEL_…"}}}
//
//   形状二（newapi 控制台）：
//     GET {base}/api/user/self  Bearer
//     {"success":true,"data":{"used_quota":…}}（1 USD = 500000 配额）
//
// 探测顺序：先形状一（要的是 5h/周窗口），解析不出再试形状二（至少
// 拿到余额）。两种都不认识 → ErrUnsupportedResponse 显式报错，
// 绝不伪造窗口数据。base_url 必填（各站不同，没有官方默认端点）。
package codingplan

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
	requestTimeout = 10 * time.Second
	maxBody        = 1 << 20
	// QuotaPerDollar newapi 家族内部配额换算（与 internal/quota/newapi 一致）。
	QuotaPerDollar = 500000.0
)

// Provider 一种 Coding Plan（kind 决定 ID/显示名；实例按 kind 各建一个）。
type Provider struct {
	id   string
	name string
	hc   *http.Client
}

// New 创建指定 kind 的 Provider（如 New("gpt", "GPT Coding Plan")）。
func New(id, name string) *Provider {
	return &Provider{id: id, name: name, hc: &http.Client{Timeout: requestTimeout}}
}

func (p *Provider) ID() string          { return p.id }
func (p *Provider) DisplayName() string { return p.name }

func (p *Provider) Capabilities() quota.ProviderCapabilities {
	return quota.ProviderCapabilities{Trust: quota.TrustC, CanRefresh: true}
}

func (p *Provider) FetchQuota(ctx context.Context, account quota.AccountRef) (*quota.QuotaSnapshot, error) {
	if account.APIKey == "" {
		return nil, errors.New("missing_api_key")
	}
	base := strings.TrimRight(account.Extra["base_url"], "/")
	if base == "" {
		return nil, errors.New("missing_base_url（填中转站地址）")
	}
	base = trimVersionSegment(base)

	// 形状一：kimi 兼容 usages（5h + 周窗口）
	if body, err := p.get(ctx, account.APIKey, base+"/usages"); err == nil {
		if snap, ok := parseUsages(body, p.id, account.ID); ok {
			return snap, nil
		}
	}
	// 形状二：newapi 控制台余额
	if body, err := p.get(ctx, account.APIKey, base+"/api/user/self"); err == nil {
		if snap, ok := parseNewapiSelf(body, p.id, account.ID); ok {
			return snap, nil
		}
	}
	return nil, fmt.Errorf("%w: %s 站点既不返回 kimi 形状 /usages，也不返回 newapi 形状 /api/user/self", quota.ErrUnsupportedResponse, p.id)
}

func (p *Provider) get(ctx context.Context, token, url string) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Accept", "application/json")
	resp, err := p.hc.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("http_status_%d", resp.StatusCode)
	}
	return io.ReadAll(io.LimitReader(resp.Body, maxBody))
}

// ---- 形状一：kimi 兼容 ----

type usageWindow struct {
	Limit     string `json:"limit"`
	Remaining string `json:"remaining"`
	ResetTime string `json:"resetTime"`
}

// parseUsages 解析 kimi 形状；返回 ok=false 让调用方试下一种形状。
func parseUsages(body []byte, providerID, accountID string) (*quota.QuotaSnapshot, bool) {
	var root struct {
		Usage  *usageWindow `json:"usage"`
		Limits []struct {
			Window struct {
				Duration  any    `json:"duration"`
				TimeUnit  string `json:"timeUnit"`
			} `json:"window"`
			Detail *usageWindow `json:"detail"`
		} `json:"limits"`
		User struct {
			Membership struct {
				Level string `json:"level"`
			} `json:"membership"`
		} `json:"user"`
	}
	if err := json.Unmarshal(body, &root); err != nil || root.Usage == nil {
		return nil, false
	}

	snap := &quota.QuotaSnapshot{
		ProviderID: providerID, AccountID: accountID,
		FetchedAt: time.Now(), Status: quota.StatusOK,
		RawVersion: "codingplan-usages-v1",
	}
	if lv := levelName(root.User.Membership.Level); lv != "" {
		snap.PlanName = lv
	}
	if w := windowFromUsage(root.Usage, "weekly", "每周额度", 7*86400); w != nil {
		snap.Windows = append(snap.Windows, *w)
	}
	for _, lim := range root.Limits {
		if !is5hWindow(lim.Window.Duration, lim.Window.TimeUnit) {
			continue
		}
		u := lim.Detail
		if u == nil {
			u = root.Usage
		}
		if w := windowFromUsage(u, "5h", "5 小时窗口", 5*3600); w != nil {
			snap.Windows = append(snap.Windows, *w)
			break
		}
	}
	if len(snap.Windows) == 0 {
		return nil, false
	}
	return snap, true
}

func is5hWindow(duration any, unit string) bool {
	switch d := duration.(type) {
	case float64:
		return d == 300 && strings.Contains(strings.ToUpper(unit), "MINUTE")
	case string:
		return d == "300" && strings.Contains(strings.ToUpper(unit), "MINUTE")
	}
	return false
}

// windowFromUsage limit/remaining 字符串 → 窗口（remaining/limit/used 全量）。
func windowFromUsage(u *usageWindow, id, label string, windowSec int64) *quota.QuotaWindow {
	limit, err1 := parseFloat(u.Limit)
	remaining, err2 := parseFloat(u.Remaining)
	if err1 != nil || limit <= 0 {
		return nil
	}
	if err2 != nil {
		remaining = limit
	}
	if remaining < 0 {
		remaining = 0
	}
	if remaining > limit {
		remaining = limit
	}
	used := limit - remaining
	usedPct := used / limit * 100
	remPct := 100 - usedPct
	w := &quota.QuotaWindow{
		ID: id, Label: label, WindowSeconds: windowSec,
		Used: &used, Limit: &limit, Remaining: &remaining,
		UsedPercent: &usedPct, RemainingPct: &remPct,
		Unit: "points", SourceField: "usages:" + id,
	}
	if t, err := time.Parse(time.RFC3339, u.ResetTime); err == nil {
		w.ResetAt = &t
	}
	return w
}

func levelName(raw string) string {
	if raw == "" {
		return ""
	}
	m := map[string]string{
		"LEVEL_BASIC": "basic", "LEVEL_INTERMEDIATE": "pro", "LEVEL_ADVANCED": "max",
	}
	if v, ok := m[raw]; ok {
		return v
	}
	return strings.ToLower(strings.TrimPrefix(raw, "LEVEL_"))
}

func parseFloat(s string) (float64, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return 0, errors.New("empty")
	}
	var v float64
	if _, err := fmt.Sscanf(s, "%g", &v); err != nil {
		return 0, err
	}
	return v, nil
}

// ---- 形状二：newapi 控制台 ----

func parseNewapiSelf(body []byte, providerID, accountID string) (*quota.QuotaSnapshot, bool) {
	var root struct {
		Success bool `json:"success"`
		Data    struct {
			UsedQuota float64 `json:"used_quota"`
		} `json:"data"`
	}
	if err := json.Unmarshal(body, &root); err != nil || !root.Success {
		return nil, false
	}
	used := root.Data.UsedQuota / QuotaPerDollar
	return &quota.QuotaSnapshot{
		ProviderID: providerID, AccountID: accountID,
		PlanName: "本月已用（newapi）",
		FetchedAt: time.Now(), Status: quota.StatusOK,
		Credits: []quota.CreditBalance{{
			ID: "used", Label: "本月已用", Balance: &used, Unit: "usd",
		}},
		RawVersion: "codingplan-newapi-v1",
	}, true
}

// trimVersionSegment 去掉末尾 /v1 等版本段（与 newapi provider 同规则）。
func trimVersionSegment(raw string) string {
	idx := len(raw)
	for i := len(raw) - 1; i >= 0; i-- {
		c := raw[i]
		if c == '/' {
			seg := raw[i+1 : idx]
			if len(seg) >= 2 && seg[0] == 'v' && seg[1] >= '0' && seg[1] <= '9' {
				return raw[:i]
			}
			return raw
		}
		if !(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') {
			return raw
		}
	}
	return raw
}
