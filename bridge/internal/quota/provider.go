// Package quota 实现订阅额度域：统一 Provider 接口、数据模型与快照缓存。
// 规范见 PLAN.md §6。
package quota

import (
	"context"
	"errors"
	"time"
)

// Provider 接口可信度等级（PLAN §6.2）。
type TrustLevel string

const (
	// TrustA 官方公开 API。
	TrustA TrustLevel = "A"
	// TrustB 官方 CLI/OAuth 提供的配额接口，但非通用公开 API。
	TrustB TrustLevel = "B"
	// TrustC 社区已观察到的控制台内部接口。
	TrustC TrustLevel = "C"
	// TrustD 只能从本地使用记录估算。
	TrustD TrustLevel = "D"
)

// ErrUnsupportedResponse 表示供应商响应结构无法解析。
// 调用方必须把它当作错误呈现，绝不能用假数据填充。
var ErrUnsupportedResponse = errors.New("unsupported_response")

// ProviderCapabilities 描述 Provider 支持的操作与可信度。
type ProviderCapabilities struct {
	Trust        TrustLevel `json:"trust"`
	CanRefresh   bool       `json:"can_refresh"`
	CanPrewarm   bool       `json:"can_prewarm"`
	Experimental bool       `json:"experimental"`
}

// AccountRef 是一次抓取的账号引用。
// 凭据只存在于 Bridge 内存/加密存储；任何实现都不得把它写入日志。
type AccountRef struct {
	ID          string
	DisplayName string
	APIKey      string
	Extra       map[string]string
}

// QuotaWindow 一个限流/额度窗口。
// 指针字段为 nil 表示供应商没有返回该数据，不得伪造为 0。
type QuotaWindow struct {
	ID            string     `json:"id"`
	Label         string     `json:"label"`
	WindowSeconds int64      `json:"window_seconds,omitempty"`
	Used          *float64   `json:"used,omitempty"`
	Limit         *float64   `json:"limit,omitempty"`
	Remaining     *float64   `json:"remaining,omitempty"`
	UsedPercent   *float64   `json:"used_percent,omitempty"`
	RemainingPct  *float64   `json:"remaining_pct,omitempty"`
	ResetAt       *time.Time `json:"reset_at,omitempty"`
	Unit          string     `json:"unit,omitempty"`
	IsUnlimited   bool       `json:"is_unlimited,omitempty"`
	SourceField   string     `json:"source_field,omitempty"`
}

// CreditBalance 独立于窗口的余额（credits）。
type CreditBalance struct {
	ID      string   `json:"id"`
	Label   string   `json:"label"`
	Balance *float64 `json:"balance,omitempty"`
	Unit    string   `json:"unit,omitempty"`
}

// QuotaSnapshot 一次成功抓取的完整快照。
type QuotaSnapshot struct {
	ProviderID string           `json:"provider_id"`
	AccountID  string           `json:"account_id"`
	PlanName   string           `json:"plan_name,omitempty"`
	FetchedAt  time.Time        `json:"fetched_at"`
	Status     string           `json:"status"`
	Windows    []QuotaWindow    `json:"windows"`
	Credits    []CreditBalance  `json:"credits,omitempty"`
	RawVersion string           `json:"raw_version,omitempty"`
}

// Status 常量。
const (
	StatusOK = "ok"
)

// QuotaProvider 所有订阅额度 Provider 的统一接口（PLAN §6.1）。
type QuotaProvider interface {
	ID() string
	DisplayName() string
	Capabilities() ProviderCapabilities
	FetchQuota(ctx context.Context, account AccountRef) (*QuotaSnapshot, error)
}
