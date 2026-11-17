package zhipu

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"gadget-bridge/internal/quota"
)

// prewarmTestEnv：quota 固定 42.5%，light 端点可控，provider 指向 httptest。
type prewarmTestEnv struct {
	srv     *httptest.Server
	pw      *Prewarmer
	lightFn func() (int, error)
}

func newPrewarmEnv(t *testing.T, weeklyUsed float64) *prewarmTestEnv {
	t.Helper()
	env := &prewarmTestEnv{}

	mux := http.NewServeMux()
	mux.HandleFunc(quotaLimitPath, func(w http.ResponseWriter, req *http.Request) {
		if req.Header.Get("Authorization") != "Bearer tk" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(fmt.Sprintf(`{
			"code": 200, "msg": "ok",
			"data": {"limits": [
				{"type":"TOKENS_LIMIT","unit":3,"number":5,"percentage":%.1f,"nextResetTime":1767223200000},
				{"type":"TOKENS_LIMIT","unit":6,"number":1,"percentage":%.1f,"nextResetTime":1767482400000}
			]}
		}`, 42.5, weeklyUsed)))
	})
	lightPath := "/api/coding/paas/v4/chat/completions" /* 36d3091：套餐消耗唯一通道（坑 #24） */
	mux.HandleFunc(lightPath, func(w http.ResponseWriter, req *http.Request) {
		if env.lightFn == nil {
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte(`{"choices":[{"message":{"content":"!"}}]}`))
			return
		}
		status, err := env.lightFn()
		if err != nil {
			w.WriteHeader(status)
			return
		}
		w.WriteHeader(status)
		_, _ = w.Write([]byte(`{"choices":[]}`))
	})

	env.srv = httptest.NewServer(mux)
	t.Cleanup(env.srv.Close)

	p := New(env.srv.URL)
	env.pw = NewPrewarmer(p, func() quota.AccountRef { return quota.AccountRef{ID: "a", APIKey: "tk"} }, "test-model")
	return env
}

func TestPrewarm_Executed(t *testing.T) {
	env := newPrewarmEnv(t, 30)
	r, err := env.pw.Prewarm(context.Background(), "key-1")
	if err != nil {
		t.Fatalf("Prewarm: %v", err)
	}
	if r.Status != PrewarmExecuted {
		t.Fatalf("status = %s, want executed", r.Status)
	}
	if r.Before == nil || r.After == nil {
		t.Fatal("前后快照必须都记录")
	}
	if r.LightHTTPStatus != 200 {
		t.Fatalf("light status = %d", r.LightHTTPStatus)
	}
	if r.DurationMS < 0 {
		t.Fatal("duration 记录错误")
	}
}

func TestPrewarm_Idempotent(t *testing.T) {
	env := newPrewarmEnv(t, 30)
	r1, _ := env.pw.Prewarm(context.Background(), "key-2")
	if r1.Status != PrewarmExecuted {
		t.Fatalf("first: %s", r1.Status)
	}
	r2, err := env.pw.Prewarm(context.Background(), "key-2")
	if err != nil {
		t.Fatalf("second: %v", err)
	}
	if r2.Status != PrewarmDuplicate {
		t.Fatalf("status = %s, want duplicate", r2.Status)
	}
	if r2.Before == nil {
		t.Fatal("duplicate 应返回上次结果")
	}
}

func TestPrewarm_Cooldown(t *testing.T) {
	env := newPrewarmEnv(t, 30)
	if _, err := env.pw.Prewarm(context.Background(), "k-a"); err != nil {
		t.Fatalf("first: %v", err)
	}
	r, err := env.pw.Prewarm(context.Background(), "k-b") // 不同 key 仍受冷却
	if err != nil {
		t.Fatalf("second: %v", err)
	}
	if r.Status != PrewarmSkippedCool || r.CooldownRemainS <= 0 {
		t.Fatalf("status=%s remain=%d, want skipped_cooldown", r.Status, r.CooldownRemainS)
	}
	if r.Before != nil {
		t.Fatal("冷却跳过不应发任何请求（不应有 before 快照）")
	}
}

func TestPrewarm_WeeklyQuotaGuard(t *testing.T) {
	env := newPrewarmEnv(t, 97)
	r, err := env.pw.Prewarm(context.Background(), "key-3")
	if err != nil {
		t.Fatalf("Prewarm: %v", err)
	}
	if r.Status != PrewarmSkippedLow {
		t.Fatalf("status = %s, want skipped_low_quota", r.Status)
	}
	if r.After != nil {
		t.Fatal("保护跳过时不应有 after")
	}
	// 冷却未占用：可立即再次触发（仍是保护跳过）
	r2, _ := env.pw.Prewarm(context.Background(), "key-4")
	if r2.Status != PrewarmSkippedLow {
		t.Fatalf("保护跳过不应进入冷却: %s", r2.Status)
	}
}

func TestPrewarm_LightFailed(t *testing.T) {
	env := newPrewarmEnv(t, 30)
	env.lightFn = func() (int, error) { return http.StatusBadGateway, errors.New("http_status_502") }
	r, err := env.pw.Prewarm(context.Background(), "key-5")
	if err != nil {
		t.Fatalf("Prewarm: %v", err)
	}
	if r.Status != PrewarmFailed || r.LightHTTPStatus != 502 {
		t.Fatalf("status=%s http=%d, want failed/502", r.Status, r.LightHTTPStatus)
	}
	if r.Before == nil {
		t.Fatal("失败也要记录 before")
	}
}

func TestPrewarm_MissingKey(t *testing.T) {
	env := newPrewarmEnv(t, 30)
	if _, err := env.pw.Prewarm(context.Background(), ""); err == nil {
		t.Fatal("缺幂等 key 必须报错")
	}
	pw := NewPrewarmer(New(""), func() quota.AccountRef { return quota.AccountRef{ID: "a"} }, "m")
	if _, err := pw.Prewarm(context.Background(), "k"); err == nil {
		t.Fatal("缺 API key 必须报错")
	}
}

var _ = time.Now // 保持 time 引用（文档化冷却参数）
