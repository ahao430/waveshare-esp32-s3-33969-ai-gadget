package zhipu

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sync"
	"time"

	"gadget-bridge/internal/quota"
)

// 预热规范（PLAN §5.6）：
//   - 只能由用户交互触发；Bridge 端再校验幂等 Key、冷却与周额度。
//   - 发起一次明确的轻量请求，记录请求前后额度与时间。
//   - 不自动预热、不后台消耗。
//   - 预热 ≠ 重置 5 小时窗口；UI 必须按供应商真实窗口规则解释，
//     本服务只报告观测到的前后变化，不做任何"重置"承诺。
const (
	prewarmCooldown    = 10 * time.Minute // 两次预热最小间隔
	prewarmIdemTTL     = 15 * time.Minute // 幂等 Key 记忆时长
	prewarmWeeklyMax   = 95.0             // 周额度已用 ≥95% 时拒绝预热
	prewarmHTTPTimeout = 15 * time.Second
)

// PrewarmStatus 结果状态。
type PrewarmStatus string

const (
	PrewarmExecuted    PrewarmStatus = "executed"          // 轻量请求已发出
	PrewarmFailed      PrewarmStatus = "failed"            // 请求发出但失败；before 已记录
	PrewarmSkippedLow  PrewarmStatus = "skipped_low_quota" // 周额度保护，未发请求
	PrewarmSkippedCool PrewarmStatus = "skipped_cooldown"  // 冷却中，未发请求
	PrewarmDuplicate   PrewarmStatus = "duplicate"         // 幂等命中，返回上次结果
)

// PrewarmResult 一次预热的完整记录（含前后快照）。
type PrewarmResult struct {
	IdempotencyKey  string               `json:"idempotency_key"`
	Status          PrewarmStatus        `json:"status"`
	Message         string               `json:"message,omitempty"`
	Model           string               `json:"model,omitempty"`
	LightHTTPStatus int                  `json:"light_http_status,omitempty"` // 轻量请求 HTTP 码
	DurationMS      int64                `json:"duration_ms,omitempty"`
	CooldownRemainS int64                `json:"cooldown_remain_s,omitempty"` // skipped_cooldown 时给出
	Before          *quota.QuotaSnapshot `json:"before,omitempty"`
	After           *quota.QuotaSnapshot `json:"after,omitempty"`
	CreatedAt       time.Time            `json:"created_at"`
}

// LightRequester 发起轻量模型请求；生产与测试各自注入。
type LightRequester func(ctx context.Context, apiKey, model string) (int, error)

// Prewarmer 预热服务（每 Provider 一实例）。
type Prewarmer struct {
	provider *Provider
	account  func() quota.AccountRef // 账号可能热更新，取函数
	model    string                  // 轻量请求模型（env 可配）
	light    LightRequester

	mu     sync.Mutex
	lastAt time.Time
	idem   map[string]*PrewarmResult
}

// NewPrewarmer 创建预热服务。model 传空串时用默认（注意：模型名随
// 智谱产品线变化，默认值需按实际可用模型验证，可经
// ZHIPU_PREWARM_MODEL 覆盖）。
func NewPrewarmer(p *Provider, account func() quota.AccountRef, model string) *Prewarmer {
	if model == "" {
		/* 套餐主模型：glm-4-flash 是免费模型，发请求也不消耗 Coding
		 * Plan 额度、不启动 5h 窗口（2026-11-17 实测余额恒 100%）。
		 * 注意：智谱限制套餐额度仅在官方工具内消耗，裸 API 即使换
		 * 模型也不计套餐——此默认值保证至少是真实计费模型。 */
		model = "glm-5.3"
	}
	return &Prewarmer{
		provider: p,
		account:  account,
		model:    model,
		light:    defaultLightRequester(p),
		idem:     make(map[string]*PrewarmResult),
	}
}

// Prewarm 执行一次预热（PLAN §15.2：用户触发 + Bridge 再校验）。
func (pw *Prewarmer) Prewarm(ctx context.Context, idemKey string) (*PrewarmResult, error) {
	if idemKey == "" {
		return nil, fmt.Errorf("missing_idempotency_key")
	}
	acc := pw.account()
	if acc.APIKey == "" {
		return nil, fmt.Errorf("missing_api_key")
	}

	pw.mu.Lock()
	if r, ok := pw.idem[idemKey]; ok && time.Since(r.CreatedAt) < prewarmIdemTTL {
		pw.mu.Unlock()
		dup := *r
		dup.Status = PrewarmDuplicate
		return &dup, nil
	}
	if remain := prewarmCooldown - time.Since(pw.lastAt); remain > 0 {
		pw.mu.Unlock()
		return &PrewarmResult{
			IdempotencyKey:  idemKey,
			Status:          PrewarmSkippedCool,
			CooldownRemainS: int64(remain.Seconds()) + 1,
			CreatedAt:       time.Now(),
		}, nil
	}
	// 占位防止并发重入；失败时回滚
	pw.lastAt = time.Now()
	pw.mu.Unlock()

	rollback := func() {
		pw.mu.Lock()
		pw.lastAt = time.Time{}
		pw.mu.Unlock()
	}

	// 1) 前置额度
	before, err := pw.provider.FetchQuota(ctx, acc)
	if err != nil {
		rollback()
		return nil, fmt.Errorf("fetch_before: %w", err)
	}

	// 2) 周额度保护
	for _, w := range before.Windows {
		if w.ID == "weekly" && w.UsedPercent != nil && *w.UsedPercent >= prewarmWeeklyMax {
			rollback()
			return &PrewarmResult{
				IdempotencyKey: idemKey,
				Status:         PrewarmSkippedLow,
				Message:        fmt.Sprintf("weekly used %.1f%% >= %.0f%%", *w.UsedPercent, prewarmWeeklyMax),
				Before:         before,
				CreatedAt:      time.Now(),
			}, nil
		}
	}

	// 3) 轻量请求
	start := time.Now()
	status, lightErr := pw.light(ctx, acc.APIKey, pw.model)
	dur := time.Since(start)

	result := &PrewarmResult{
		IdempotencyKey:  idemKey,
		Model:           pw.model,
		LightHTTPStatus: status,
		DurationMS:      dur.Milliseconds(),
		Before:          before,
		CreatedAt:       time.Now(),
	}
	if lightErr != nil {
		result.Status = PrewarmFailed
		result.Message = lightErr.Error()
	} else {
		result.Status = PrewarmExecuted
	}

	// 4) 后置额度（失败不影响结果返回，只是 After 缺失）
	if after, err := pw.provider.FetchQuota(ctx, acc); err == nil {
		result.After = after
	}

	pw.mu.Lock()
	pw.idem[idemKey] = result
	// 顺带清理过期幂等项
	for k, v := range pw.idem {
		if time.Since(v.CreatedAt) >= prewarmIdemTTL {
			delete(pw.idem, k)
		}
	}
	pw.mu.Unlock()
	return result, nil
}

// defaultLightRequester：POST /api/paas/v4/chat/completions，max_tokens=1。
// 这是标准模型推理 API（非订阅额度 C 级接口），但模型名与计费影响需
// 实际账号验证。
func defaultLightRequester(p *Provider) LightRequester {
	return func(ctx context.Context, apiKey, model string) (int, error) {
		ctx, cancel := context.WithTimeout(ctx, prewarmHTTPTimeout)
		defer cancel()

		body, _ := json.Marshal(map[string]any{
			"model":      model,
			"messages":   []map[string]string{{"role": "user", "content": "hi"}},
			"max_tokens": 1,
			"stream":     false,
		})
		req, err := http.NewRequestWithContext(ctx, http.MethodPost,
			p.baseURL+"/api/paas/v4/chat/completions", bytes.NewReader(body))
		if err != nil {
			return 0, err
		}
		req.Header.Set("Authorization", "Bearer "+apiKey)
		req.Header.Set("Content-Type", "application/json")

		resp, err := p.hc.Do(req)
		if err != nil {
			return 0, err
		}
		defer resp.Body.Close()
		io.Copy(io.Discard, io.LimitReader(resp.Body, maxResponseBytes))

		if resp.StatusCode >= 300 {
			return resp.StatusCode, fmt.Errorf("http_status_%d", resp.StatusCode)
		}
		return resp.StatusCode, nil
	}
}

// WindowPct 从快照取窗口已用百分比（Web/设备端展示辅助）。
func WindowPct(snap *quota.QuotaSnapshot, windowID string) *float64 {
	if snap == nil {
		return nil
	}
	for _, w := range snap.Windows {
		if w.ID == windowID {
			return w.UsedPercent
		}
	}
	return nil
}
