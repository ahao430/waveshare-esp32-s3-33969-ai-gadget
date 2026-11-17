package newapi

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"

	"gadget-bridge/internal/quota"
)

func accountOf(extra map[string]string, key string) quota.AccountRef {
	return quota.AccountRef{ID: "sub-x", APIKey: key, Extra: extra}
}

func baseExtra(sysToken, uid string) map[string]string {
	return map[string]string{
		"base_url":        "https://example.com/v1",
		"system_token":    sysToken,
		"system_user_id":  uid,
	}
}

func TestFetchQuota_NeedsSystemToken(t *testing.T) {
	p := New()
	// sk- Key 在 APIKey（聊天用）但没有系统令牌 → 明确指引
	_, err := p.FetchQuota(context.Background(), accountOf(baseExtra("", ""), "sk-chat"))
	if err == nil {
		t.Fatal("want explicit error when system_token missing")
	}
}

func TestFetchQuota_HeadersAndParse(t *testing.T) {
	var gotAuth, gotUID, gotPath string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		gotAuth = r.Header.Get("Authorization")
		gotUID = r.Header.Get("New-Api-User")
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"success":true,"data":{"quota":2500000,"used_quota":125000,"email":"a@b.c"}}`))
	}))
	defer srv.Close()

	extra := baseExtra("sys-token-1", "42")
	extra["base_url"] = srv.URL + "/v1" // 带 /v1 版本段：应被去掉
	p := New()
	snap, err := p.FetchQuota(context.Background(), accountOf(extra, "sk-chat"))
	if err != nil {
		t.Fatalf("FetchQuota: %v", err)
	}
	if gotPath != "/api/user/self" {
		t.Fatalf("path = %s（/v1 应被剥掉）", gotPath)
	}
	if gotAuth != "Bearer sys-token-1" {
		t.Fatalf("auth = %s（必须用系统访问令牌，不是 sk-）", gotAuth)
	}
	if gotUID != "42" {
		t.Fatalf("New-Api-User = %q", gotUID)
	}
	if len(snap.Credits) == 0 || snap.Credits[0].Balance == nil || *snap.Credits[0].Balance != 5.0 {
		t.Fatalf("余额换算错误：%v（2500000/500000 应为 5.0）", snap.Credits)
	}
}

func TestFetchQuota_CNYCurrency(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"success":true,"data":{"quota":2500000,"used_quota":125000}}`))
	}))
	defer srv.Close()
	extra := baseExtra("sys-token-1", "42")
	extra["base_url"] = srv.URL
	extra["currency"] = "cny" // 国内中转站：余额以元展示
	snap, err := New().FetchQuota(context.Background(), accountOf(extra, "sk-chat"))
	if err != nil {
		t.Fatalf("FetchQuota: %v", err)
	}
	if snap.PlanName != "NewAPI 中转站 · 余额（元）" {
		t.Fatalf("plan = %s", snap.PlanName)
	}
	if len(snap.Credits) == 0 || snap.Credits[0].Unit != "CNY" {
		t.Fatalf("credits unit = %+v（cny 应输出 CNY）", snap.Credits)
	}
}

func TestFetchQuota_SiteErrorMessage(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"success":false,"message":"无效的访问令牌"}`))
	}))
	defer srv.Close()
	extra := baseExtra("wrong-token", "1")
	extra["base_url"] = srv.URL
	_, err := New().FetchQuota(context.Background(), accountOf(extra, "sk"))
	if err == nil || err.Error() == "" {
		t.Fatalf("want site message surfaced, got %v", err)
	}
}
