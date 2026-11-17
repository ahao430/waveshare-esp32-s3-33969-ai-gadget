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

// FetchQuota 返回余额视图（credits[0]=剩余余额，credits[1]=累计已用——
// new-api 的 used_quota 是开户以来累计消耗，非月度）。
// base_url 从 account.Extra["base_url"] 取（必填，无公共默认站点）。
// 币种 Extra["currency"]："cny"=元（国内中转站常见），默认 "usd"=美元
// （500000 配额/单位是 new-api 内部约定，两种币只是展示标签不同）。
//
// 鉴权修正（2026-11-17，对齐 AIVault newapi.dart）：/api/user/self 用
// 的是 Web 控制台「系统访问令牌」（Extra["system_token"]），不是 sk-
// API Key——sk 只能调聊天端点，站点对它报 invalid access token。
// 新版 new-api 还强制 New-Api-User 头（用户 ID，Extra["system_user_id"]）。
func (p *Provider) FetchQuota(ctx context.Context, account quota.AccountRef) (*quota.QuotaSnapshot, error) {
	sysToken := account.Extra["system_token"]
	if sysToken == "" {
		return nil, errors.New("需要系统访问令牌：Web 控制台「个人设置」生成（不是 sk- API Key），在订阅表单填入")
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
	req.Header.Set("Authorization", "Bearer "+sysToken)
	req.Header.Set("Accept", "application/json")
	if uid := account.Extra["system_user_id"]; uid != "" {
		req.Header.Set("New-Api-User", uid) // 新版 new-api 必需
	}

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
		Success bool   `json:"success"`
		Message string `json:"message"`
		Data    struct {
			Quota     *float64 `json:"quota"`      // 剩余（内部配额单位）
			UsedQuota *float64 `json:"used_quota"` // 本月累计已用
			Email     string   `json:"email"`
		} `json:"data"`
	}
	if err := json.Unmarshal(body, &root); err != nil || !root.Success {
		if !root.Success && root.Message != "" {
			return nil, fmt.Errorf("站点返回：%s", root.Message)
		}
		return nil, quota.ErrUnsupportedResponse
	}
	unit, unitName := "usd", "美元"
	if account.Extra["currency"] == "cny" {
		unit, unitName = "CNY", "元"
	}
	balUSD := 0.0
	if root.Data.Quota != nil {
		balUSD = *root.Data.Quota / QuotaPerDollar
	}
	usedUSD := 0.0
	if root.Data.UsedQuota != nil {
		usedUSD = *root.Data.UsedQuota / QuotaPerDollar
	}
	credits := []quota.CreditBalance{{
		ID: "balance", Label: "剩余余额",
		Balance: &balUSD, Unit: unit,
	}}
	if usedUSD > 0 {
		credits = append(credits, quota.CreditBalance{
			ID: "used_total", Label: "累计已用",
			Balance: &usedUSD, Unit: unit,
		})
	}
	return &quota.QuotaSnapshot{
		ProviderID: p.ID(), AccountID: account.ID,
		PlanName:   "NewAPI 中转站 · 余额（" + unitName + "）",
		FetchedAt:  time.Now(), Status: quota.StatusOK,
		Credits:    credits,
		RawVersion: "newapi-v2",
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
