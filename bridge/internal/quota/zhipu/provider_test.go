package zhipu

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"

	"gadget-bridge/internal/quota"
)

func loadFixture(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("testdata", name))
	if err != nil {
		t.Fatalf("read fixture %s: %v", name, err)
	}
	return b
}

// newTestServer 起 httptest 服务，按 path 返回 fixture 内容与状态码。
type resp struct {
	path   string
	body   string
	status int
}

func newTestServer(t *testing.T, resps ...resp) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	for _, r := range resps {
		mux.HandleFunc(r.path, func(w http.ResponseWriter, req *http.Request) {
			if got := req.Header.Get("Authorization"); got != "Bearer test-key" {
				w.WriteHeader(http.StatusUnauthorized)
				return
			}
			if r.status != 0 {
				w.WriteHeader(r.status)
			}
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write([]byte(r.body))
		})
	}
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

func TestFetchQuota_MapsWindows(t *testing.T) {
	srv := newTestServer(t,
		resp{path: quotaLimitPath, body: string(loadFixture(t, "quota_ok.json"))},
		resp{path: subscriptionPath, body: string(loadFixture(t, "subscription_ok.json"))},
	)
	p := New(srv.URL)

	snap, err := p.FetchQuota(context.Background(), quota.AccountRef{ID: "acc1", APIKey: "test-key"})
	if err != nil {
		t.Fatalf("FetchQuota: %v", err)
	}

	if snap.ProviderID != "zhipu" || snap.Status != quota.StatusOK {
		t.Fatalf("bad snapshot header: %+v", snap)
	}
	// 套餐名 + 到期时间（fixture valid 区间止于 2027-12-11 → "12月11日到期"）
	if snap.PlanName != "GLM Coding Lite · 12月11日到期" {
		t.Fatalf("PlanName = %q, want fixture plan name + expiry", snap.PlanName)
	}
	if len(snap.Windows) != 3 {
		t.Fatalf("windows = %d, want 3 (5h/weekly/tools)", len(snap.Windows))
	}

	byID := map[string]quota.QuotaWindow{}
	for _, w := range snap.Windows {
		byID[w.ID] = w
	}

	w5h := byID["5h"]
	if w5h.UsedPercent == nil || *w5h.UsedPercent != 42.5 {
		t.Fatalf("5h used%% = %v, want 42.5", w5h.UsedPercent)
	}
	if w5h.RemainingPct == nil || *w5h.RemainingPct != 57.5 {
		t.Fatalf("5h remaining%% = %v, want 57.5（已用与剩余分开存储）", w5h.RemainingPct)
	}
	if w5h.WindowSeconds != 5*3600 {
		t.Fatalf("5h window seconds = %d", w5h.WindowSeconds)
	}
	if w5h.ResetAt == nil || w5h.ResetAt.Unix() != 1767223200 { // 2026-01-01T00:00:00Z (ms fixture)
		t.Fatalf("5h resetAt = %v, want 2026-01-01T00:00:00Z", w5h.ResetAt)
	}

	wk := byID["weekly"]
	if wk.UsedPercent == nil || *wk.UsedPercent != 61.2 {
		t.Fatalf("weekly used%% = %v, want 61.2", wk.UsedPercent)
	}
	if wk.WindowSeconds != 7*86400 {
		t.Fatalf("weekly window seconds = %d", wk.WindowSeconds)
	}

	if _, ok := byID["time_u2_n1"]; !ok {
		t.Fatalf("TIME_LIMIT 未映射为独立窗口: %+v", snap.Windows)
	}
}

func TestFetchQuota_SubscriptionFailureKeepsQuota(t *testing.T) {
	// 套餐接口 500：额度数据必须保留，PlanName 为空。
	srv := newTestServer(t,
		resp{path: quotaLimitPath, body: string(loadFixture(t, "quota_ok.json"))},
		resp{path: subscriptionPath, body: `{"code":500,"msg":"boom"}`, status: http.StatusInternalServerError},
	)
	p := New(srv.URL)

	snap, err := p.FetchQuota(context.Background(), quota.AccountRef{ID: "acc1", APIKey: "test-key"})
	if err != nil {
		t.Fatalf("套餐失败不应导致整体失败: %v", err)
	}
	if snap.PlanName != "" {
		t.Fatalf("PlanName 应为空，got %q", snap.PlanName)
	}
	if len(snap.Windows) == 0 {
		t.Fatal("额度窗口数据丢失")
	}
}

func TestFetchQuota_UnsupportedResponse(t *testing.T) {
	srv := newTestServer(t,
		resp{path: quotaLimitPath, body: string(loadFixture(t, "quota_unexpected.json"))},
	)
	p := New(srv.URL)

	_, err := p.FetchQuota(context.Background(), quota.AccountRef{ID: "acc1", APIKey: "test-key"})
	if !errors.Is(err, quota.ErrUnsupportedResponse) {
		t.Fatalf("want ErrUnsupportedResponse, got %v", err)
	}
}

func TestFetchQuota_MissingKey(t *testing.T) {
	p := New("")
	if _, err := p.FetchQuota(context.Background(), quota.AccountRef{ID: "acc1"}); err == nil {
		t.Fatal("缺少 APIKey 必须报错")
	}
}

func TestNormalizeResetMs(t *testing.T) {
	cases := []struct {
		in   int64
		want int64
	}{
		{1767223200000, 1767223200}, // 毫秒
		{1767223200, 1767223200},    // 秒
		{0, 0},                      // 缺失
	}
	for _, c := range cases {
		if got := normalizeResetMs(c.in); got != c.want {
			t.Errorf("normalizeResetMs(%d) = %d, want %d", c.in, got, c.want)
		}
	}
}

func TestParseSubscriptionRealArrayShape(t *testing.T) {
	// 2026-11-16 真网实测响应（智谱 /api/biz/subscription/list）：
	// data 直接是数组、套餐名 productName、到期在 valid 区间串里
	body := []byte(`{"code":200,"msg":"操作成功","data":[{"id":"108253","productName":"GLM Coding Lite","status":"VALID","purchaseTime":"2025-12-11 15:07:09","valid":"2026-12-11 10:00:00-2027-12-11 10:00:00","nextRenewTime":"2026-12-11"}],"success":true}`)
	plan, expires, err := parseSubscriptionResponseEx(body)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if plan != "GLM Coding Lite" {
		t.Fatalf("plan = %q", plan)
	}
	if expires == nil || expires.Year() != 2027 || expires.Month() != time.December {
		t.Fatalf("expires = %v (want 2027-12-11)", expires)
	}
}

func TestParseSubscriptionSkipsInvalid(t *testing.T) {
	body := []byte(`{"code":200,"data":[{"productName":"Old","status":"EXPIRED","valid":"2024-01-01 00:00:00-2025-01-01 00:00:00"}],"success":true}`)
	plan, expires, err := parseSubscriptionResponseEx(body)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if plan != "" || expires != nil {
		t.Fatalf("expired subscription must be skipped: %q %v", plan, expires)
	}
}
