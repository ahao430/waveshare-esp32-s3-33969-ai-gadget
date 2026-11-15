// Package deepseek — DeepSeek 官方余额 Provider（A 级）。
// 端点：GET {base}/user/balance（Bearer；AIVault deepseek.dart 同源）。
// 响应：{"is_available":true,"balance_infos":[{"currency":"CNY",
// "total_balance":"110.35","gift_balance":"10.00",…}]}
// 按量计费无订阅窗口 → 只出余额（credits），不伪造窗口数据。
package deepseek

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
	requestTimeout  = 10 * time.Second
	maxBody         = 1 << 20
	defaultBase     = "https://api.deepseek.com"
	balancePath     = "/user/balance"
)

type Provider struct {
	hc *http.Client
}

func New() *Provider { return &Provider{hc: &http.Client{Timeout: requestTimeout}} }

func (p *Provider) ID() string          { return "deepseek" }
func (p *Provider) DisplayName() string { return "DeepSeek 余额" }

func (p *Provider) Capabilities() quota.ProviderCapabilities {
	return quota.ProviderCapabilities{Trust: quota.TrustA, CanRefresh: true}
}

// FetchQuota 返回余额视图：CNY 总余额 + 赠送额（若有）。
// base_url 可选（默认官方站；供镜像/中转场景覆盖）。
func (p *Provider) FetchQuota(ctx context.Context, account quota.AccountRef) (*quota.QuotaSnapshot, error) {
	if account.APIKey == "" {
		return nil, errors.New("missing_api_key")
	}
	base := strings.TrimRight(account.Extra["base_url"], "/")
	if base == "" {
		base = defaultBase
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, base+balancePath, nil)
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
	body, err := io.ReadAll(io.LimitReader(resp.Body, maxBody))
	if err != nil {
		return nil, err
	}

	var root struct {
		IsAvailable bool `json:"is_available"`
		BalanceInfos []struct {
			Currency     string `json:"currency"`
			TotalBalance string `json:"total_balance"`
			GiftBalance  string `json:"gift_balance"`
		} `json:"balance_infos"`
	}
	if err := json.Unmarshal(body, &root); err != nil || len(root.BalanceInfos) == 0 {
		return nil, fmt.Errorf("%w: deepseek balance shape", quota.ErrUnsupportedResponse)
	}

	snap := &quota.QuotaSnapshot{
		ProviderID: p.ID(), AccountID: account.ID,
		FetchedAt: time.Now(), Status: quota.StatusOK,
		RawVersion: "deepseek-balance-v1",
	}
	for i, info := range root.BalanceInfos {
		total, err := parseAmount(info.TotalBalance)
		if err != nil {
			continue
		}
		c := quota.CreditBalance{
			ID: fmt.Sprintf("balance_%d", i), Balance: &total, Unit: info.Currency,
		}
		if strings.EqualFold(info.Currency, "CNY") {
			c.Label = "余额（元）"
			c.ID = "balance"
		} else {
			c.Label = "余额（" + info.Currency + "）"
		}
		if gift, err := parseAmount(info.GiftBalance); err == nil && gift > 0 {
			c.Label += "（含赠送 " + trimZeros(gift) + "）"
		}
		snap.Credits = append(snap.Credits, c)
	}
	if len(snap.Credits) == 0 {
		return nil, fmt.Errorf("%w: no parsable balance_infos", quota.ErrUnsupportedResponse)
	}
	return snap, nil
}

func parseAmount(s string) (float64, error) {
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

func trimZeros(v float64) string {
	s := fmt.Sprintf("%.2f", v)
	return strings.TrimRight(strings.TrimRight(s, "0"), ".")
}
