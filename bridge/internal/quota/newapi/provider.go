// Package newapi — NewAPI 中转站余额 Provider（B 级）。
// 端点：GET {origin}/api/user/self（系统访问令牌，与 sk- Key 不同）；
// used_quota 内部配额 → 美元（1 USD = 500000），参考 AIVault newapi.dart。
package newapi

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
	// QuotaPerDollar NewAPI 内部配额换算：1 美元 = 500000 配额。
	QuotaPerDollar = 500000.0
)

type Provider struct {
	hc *http.Client
}

func New() *Provider {
	return &Provider{hc: &http.Client{Timeout: requestTimeout}}
}

func (p *Provider) ID() string          { return "newapi" }
func (p *Provider) DisplayName() string { return "NewAPI 中转站" }

func (p *Provider) Capabilities() quota.ProviderCapabilities {
	return quota.ProviderCapabilities{Trust: quota.TrustB, CanRefresh: true}
}

// FetchQuota 返回"本月已用"余额视图（credits）。
// base_url 从 account.Extra["base_url"] 取（必填，无公共默认站点）。
func (p *Provider) FetchQuota(ctx context.Context, account quota.AccountRef) (*quota.QuotaSnapshot, error) {
	if account.APIKey == "" {
		return nil, errors.New("missing_api_key")
	}
	origin := account.Extra["base_url"]
	if origin == "" {
		return nil, errors.New("missing_base_url")
	}
	origin = strings.TrimRight(origin, "/")
	origin = trimVersionSegment(origin) // /v1 等版本段去掉（控制台 API 在站点根）

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, origin+"/api/user/self", nil)
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
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, err
	}

	var root struct {
		Success bool `json:"success"`
		Data    struct {
			UsedQuota float64 `json:"used_quota"`
			Email     string  `json:"email"`
		} `json:"data"`
	}
	if err := json.Unmarshal(body, &root); err != nil || !root.Success {
		return nil, quota.ErrUnsupportedResponse
	}
	usedUSD := root.Data.UsedQuota / QuotaPerDollar
	return &quota.QuotaSnapshot{
		ProviderID: p.ID(), AccountID: account.ID,
		PlanName:  "NewAPI · 本月已用",
		FetchedAt: time.Now(), Status: quota.StatusOK,
		Credits: []quota.CreditBalance{{
			ID: "balance", Label: "本月已用",
			Balance: &usedUSD, Unit: "usd",
		}},
		RawVersion: "newapi-v1",
	}, nil
}

// trimVersionSegment 去掉 base_url 末尾的 /v1 等版本段。
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
