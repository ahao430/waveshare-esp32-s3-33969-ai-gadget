// Package zhipu 实现智谱 GLM Coding Plan 订阅额度 Adapter（PLAN §6.3）。
//
// 接口可信度：C 级（社区观察到的控制台内部接口，非公开 API）。
// 响应结构来自社区项目（dsh-provider-balance / CodexBar）记录，尚未用
// 真实账号验证 —— 解析逻辑必须对未知结构返回 ErrUnsupportedResponse，
// 不得伪造数据。真机账号验证完成后应更新 testdata fixtures。
package zhipu

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
	// DefaultBaseURLCN 国内站。
	DefaultBaseURLCN = "https://open.bigmodel.cn"
	// DefaultBaseURLIntl 国际站（按账号区域选择，不要混用）。
	DefaultBaseURLIntl = "https://api.z.ai"
)

// quotaLimitPath 与 subscriptionPath 是 C 级社区观察接口。
const (
	quotaLimitPath    = "/api/monitor/usage/quota/limit"
	subscriptionPath  = "/api/biz/subscription/list"
	requestTimeout    = 10 * time.Second
	maxResponseBytes  = 1 << 20 // 1MB 上限，防御异常响应
)

// Provider 是智谱额度 Provider。
type Provider struct {
	baseURL string
	hc      *http.Client
}

// New 创建 Provider；baseURL 传空串时使用国内站。
func New(baseURL string) *Provider {
	if baseURL == "" {
		baseURL = DefaultBaseURLCN
	}
	return &Provider{
		baseURL: strings.TrimRight(baseURL, "/"),
		hc:      &http.Client{Timeout: requestTimeout},
	}
}

func (p *Provider) ID() string          { return "zhipu" }
func (p *Provider) DisplayName() string { return "智谱 GLM Coding Plan" }

func (p *Provider) Capabilities() quota.ProviderCapabilities {
	return quota.ProviderCapabilities{
		Trust:        quota.TrustC,
		CanRefresh:   true,
		CanPrewarm:   true,
		Experimental: true, // C 级接口默认标注实验性
	}
}

// FetchQuota 分别请求额度接口与套餐接口。
// 套餐接口失败只影响 PlanName（保持空），不让额度数据丢失（PLAN §6.3）。
func (p *Provider) FetchQuota(ctx context.Context, account quota.AccountRef) (*quota.QuotaSnapshot, error) {
	if account.APIKey == "" {
		return nil, errors.New("missing_api_key")
	}

	windows, rawVer, err := p.fetchLimitWindows(ctx, account.APIKey)
	if err != nil {
		return nil, err
	}

	snap := &quota.QuotaSnapshot{
		ProviderID: p.ID(),
		AccountID:  account.ID,
		FetchedAt:  time.Now(),
		Status:     quota.StatusOK,
		Windows:    windows,
		RawVersion: rawVer,
	}

	// 套餐接口独立容错：失败不影响额度快照。
	if plan, planErr := p.fetchPlanName(ctx, account.APIKey); planErr == nil {
		snap.PlanName = plan
	}
	return snap, nil
}

func (p *Provider) fetchLimitWindows(ctx context.Context, apiKey string) ([]quota.QuotaWindow, string, error) {
	body, err := p.get(ctx, apiKey, quotaLimitPath)
	if err != nil {
		return nil, "", fmt.Errorf("quota_limit: %w", err)
	}
	windows, rawVer, err := parseQuotaResponse(body)
	if err != nil {
		return nil, "", fmt.Errorf("quota_limit: %w", err)
	}
	return windows, rawVer, nil
}

func (p *Provider) fetchPlanName(ctx context.Context, apiKey string) (string, error) {
	body, err := p.get(ctx, apiKey, subscriptionPath)
	if err != nil {
		return "", err
	}
	return parseSubscriptionResponse(body)
}

func (p *Provider) get(ctx context.Context, apiKey, path string) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, p.baseURL+path, nil)
	if err != nil {
		return nil, err
	}
	// 安全：Authorization 只进请求头，永不进日志（PLAN §6.12）。
	req.Header.Set("Authorization", "Bearer "+apiKey)
	req.Header.Set("Accept", "application/json")

	resp, err := p.hc.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("http_status_%d", resp.StatusCode)
	}
	return io.ReadAll(io.LimitReader(resp.Body, maxResponseBytes))
}

// rawEnvelope 是所有智谱接口的公共外壳。
type rawEnvelope struct {
	Code any    `json:"code"` // 可能是 number 或 string，宽松处理
	Msg  string `json:"msg,omitempty"`
	Data json.RawMessage `json:"data,omitempty"`
}

func envelopeOK(env rawEnvelope) bool {
	// 社区记录成功 code 为 200；其他值按失败处理。
	switch v := env.Code.(type) {
	case float64:
		return v == 200
	case string:
		return v == "200"
	}
	return false
}

// parseQuotaResponse 解析额度接口响应（结构见 testdata fixtures）。
// 已知窗口映射（PLAN §6.3 字段映射）：
//   - TOKENS_LIMIT + unit=3,number=5  → 5 小时窗口
//   - TOKENS_LIMIT + unit=6,number=1  → 每周窗口
//   - TIME_LIMIT                      → 工具/时间额度（独立窗口，不与 Token 窗口混算）
// 其余组合保留原始 unit/number 到 SourceField，标记为未分类。
func parseQuotaResponse(body []byte) ([]quota.QuotaWindow, string, error) {
	var env rawEnvelope
	if err := json.Unmarshal(body, &env); err != nil {
		return nil, "", fmt.Errorf("%w: bad json: %v", quota.ErrUnsupportedResponse, err)
	}
	if !envelopeOK(env) {
		return nil, "", fmt.Errorf("%w: code=%v msg=%q", quota.ErrUnsupportedResponse, env.Code, env.Msg)
	}

	var data struct {
		Limits []struct {
			Type          string   `json:"type"`
			Unit          int      `json:"unit"`
			Number        int      `json:"number"`
			Percentage    *float64 `json:"percentage"`     // 已使用百分比（非剩余）
			NextResetTime int64    `json:"nextResetTime"`  // Unix 毫秒（社区记录）
		} `json:"limits"`
	}
	if err := json.Unmarshal(env.Data, &data); err != nil {
		return nil, "", fmt.Errorf("%w: data shape: %v", quota.ErrUnsupportedResponse, err)
	}
	if len(data.Limits) == 0 {
		return nil, "", fmt.Errorf("%w: empty limits", quota.ErrUnsupportedResponse)
	}

	var windows []quota.QuotaWindow
	for i, lim := range data.Limits {
		w := quota.QuotaWindow{
			Unit:        "percent",
			SourceField: fmt.Sprintf("limits[%d].%s(unit=%d,number=%d)", i, lim.Type, lim.Unit, lim.Number),
		}
		switch {
		case lim.Type == "TOKENS_LIMIT" && lim.Unit == 3 && lim.Number == 5:
			w.ID, w.Label, w.WindowSeconds = "5h", "5 小时窗口", 5*3600
		case lim.Type == "TOKENS_LIMIT" && lim.Unit == 6 && lim.Number == 1:
			w.ID, w.Label, w.WindowSeconds = "weekly", "每周额度", 7*86400
		case lim.Type == "TIME_LIMIT":
			// 工具/搜索类时间额度：语义按实际账号确认，先按独立窗口透出。
			w.ID, w.Label = fmt.Sprintf("time_u%d_n%d", lim.Unit, lim.Number), "工具/时间额度"
		default:
			w.ID, w.Label = fmt.Sprintf("unknown_%d", i), "未分类额度窗口"
		}

		if lim.Percentage != nil {
			used := *lim.Percentage
			w.UsedPercent = &used
			rem := 100 - used
			w.RemainingPct = &rem
		}
		if ts := normalizeResetMs(lim.NextResetTime); ts > 0 {
			t := time.Unix(ts, 0).UTC()
			w.ResetAt = &t
		}
		windows = append(windows, w)
	}
	return windows, "zhipu-console-v1", nil
}

// normalizeResetMs 社区记录 nextResetTime 为 Unix 毫秒；
// 防御性兼容秒级时间戳（>1e11 视为毫秒，1e9~1e11 视为秒，其余视为缺失）。
func normalizeResetMs(v int64) int64 {
	switch {
	case v > 1e11:
		return v / 1000
	case v > 1e9:
		return v
	default:
		return 0
	}
}

// parseSubscriptionResponse 解析套餐接口，返回套餐名。
// 套餐响应结构未经验证：兼容 data.list[] 中 name/planName/title 字段，
// 找不到时返回空串（缺失不伪造）。
func parseSubscriptionResponse(body []byte) (string, error) {
	var env rawEnvelope
	if err := json.Unmarshal(body, &env); err != nil {
		return "", fmt.Errorf("%w: bad json: %v", quota.ErrUnsupportedResponse, err)
	}
	if !envelopeOK(env) {
		return "", fmt.Errorf("%w: code=%v", quota.ErrUnsupportedResponse, env.Code)
	}

	var data struct {
		List []struct {
			Name     string `json:"name"`
			PlanName string `json:"planName"`
			Title    string `json:"title"`
		} `json:"list"`
	}
	if err := json.Unmarshal(env.Data, &data); err != nil {
		return "", fmt.Errorf("%w: data shape: %v", quota.ErrUnsupportedResponse, err)
	}
	for _, it := range data.List {
		for _, cand := range []string{it.PlanName, it.Name, it.Title} {
			if cand != "" {
				return cand, nil
			}
		}
	}
	return "", nil // 结构可解析但没有套餐字段：不视为错误
}
