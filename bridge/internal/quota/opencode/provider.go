// Package opencode — OpenCode Zen (Go) 订阅额度 Provider（B 级）。
// 端点：GET {base}/usage（默认 https://opencode.ai/zen/go/v1），
// 解析参考 AIVault additional_providers.dart parseOpencodeUsage。
package opencode

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
	DefaultBaseURL   = "https://opencode.ai/zen/go/v1"
	usagePath        = "/usage"
	requestTimeout   = 10 * time.Second
	maxResponseBytes = 1 << 20
)

// Provider 实现 QuotaProvider。
type Provider struct {
	baseURL string
	hc      *http.Client
}

func New(baseURL string) *Provider {
	if baseURL == "" {
		baseURL = DefaultBaseURL
	}
	return &Provider{baseURL: strings.TrimRight(baseURL, "/"), hc: &http.Client{Timeout: requestTimeout}}
}

func (p *Provider) ID() string          { return "opencode" }
func (p *Provider) DisplayName() string { return "OpenCode Zen (Go)" }

func (p *Provider) Capabilities() quota.ProviderCapabilities {
	return quota.ProviderCapabilities{Trust: quota.TrustB, CanRefresh: true}
}

func (p *Provider) FetchQuota(ctx context.Context, account quota.AccountRef) (*quota.QuotaSnapshot, error) {
	if account.APIKey == "" {
		return nil, errors.New("missing_api_key")
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, p.baseURL+usagePath, nil)
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
	return parseUsageResponse(body)
}

// usageWindow: {status, percent(已用 0-100), resetsAt}。
func parseWindow(win json.RawMessage, id, label string, secs int64) (quota.QuotaWindow, bool) {
	var w struct {
		Percent  *float64 `json:"percent"`
		ResetsAt *string  `json:"resetsAt"`
		Status   string   `json:"status"`
	}
	if err := json.Unmarshal(win, &w); err != nil || w.Percent == nil {
		return quota.QuotaWindow{}, false
	}
	out := quota.QuotaWindow{
		ID: id, Label: label, WindowSeconds: secs,
		UsedPercent: w.Percent, Unit: "percent",
		SourceField: "usage." + id,
	}
	if w.ResetsAt != nil {
		if t, err := time.Parse(time.RFC3339, *w.ResetsAt); err == nil {
			tt := t
			out.ResetAt = &tt
		}
	}
	return out, true
}

func parseUsageResponse(body []byte) (*quota.QuotaSnapshot, error) {
	var root struct {
		Usage struct {
			Rolling json.RawMessage `json:"rolling"`
			Weekly  json.RawMessage `json:"weekly"`
		} `json:"usage"`
	}
	if err := json.Unmarshal(body, &root); err != nil {
		return nil, fmt.Errorf("%w: %v", quota.ErrUnsupportedResponse, err)
	}
	snap := &quota.QuotaSnapshot{Status: quota.StatusOK, FetchedAt: time.Now()}
	if w, ok := parseWindow(root.Usage.Rolling, "5h", "5 小时滚动", 18000); ok {
		snap.Windows = append(snap.Windows, w)
	}
	if w, ok := parseWindow(root.Usage.Weekly, "weekly", "每周额度", 604800); ok {
		snap.Windows = append(snap.Windows, w)
	}
	if len(snap.Windows) == 0 {
		return nil, quota.ErrUnsupportedResponse
	}
	return snap, nil
}
