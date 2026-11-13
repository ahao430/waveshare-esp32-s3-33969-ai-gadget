package kimi

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

func TestFetchQuota_MapsWindows(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc(usagesPath, func(w http.ResponseWriter, req *http.Request) {
		if req.Header.Get("Authorization") != "Bearer test-key" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(loadFixture(t, "usages_ok.json"))
	})
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)

	p := New(srv.URL)
	snap, err := p.FetchQuota(context.Background(), quota.AccountRef{ID: "acc1", APIKey: "test-key"})
	if err != nil {
		t.Fatalf("FetchQuota: %v", err)
	}
	if snap.ProviderID != "kimi" || snap.Status != quota.StatusOK {
		t.Fatalf("bad snapshot header: %+v", snap)
	}

	byID := map[string]quota.QuotaWindow{}
	for _, w := range snap.Windows {
		byID[w.ID] = w
	}

	// 5 小时窗口：used/limit 字符串 → 数值与百分比
	w5h := byID["5h"]
	if w5h.WindowSeconds != 5*3600 {
		t.Fatalf("5h window seconds = %d", w5h.WindowSeconds)
	}
	if w5h.Used == nil || *w5h.Used != 139 {
		t.Fatalf("5h used = %v, want 139", w5h.Used)
	}
	if w5h.Limit == nil || *w5h.Limit != 200 {
		t.Fatalf("5h limit = %v, want 200", w5h.Limit)
	}
	if w5h.UsedPercent == nil || *w5h.UsedPercent <= 69 || *w5h.UsedPercent >= 70 {
		t.Fatalf("5h used%% = %v, want ~69.5", w5h.UsedPercent)
	}
	if w5h.ResetAt == nil || w5h.ResetAt.UTC().Format(time.RFC3339) != "2026-01-06T13:33:02Z" {
		t.Fatalf("5h resetAt = %v, want 2026-01-06T13:33:02Z", w5h.ResetAt)
	}

	// 月度/总量池
	mo := byID["monthly"]
	if mo.Used == nil || *mo.Used != 214 || mo.Limit == nil || *mo.Limit != 2048 {
		t.Fatalf("monthly used/limit = %v/%v, want 214/2048", mo.Used, mo.Limit)
	}
}

func TestFetchQuota_UnsupportedResponse(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		_, _ = w.Write(loadFixture(t, "usages_unexpected.json"))
	}))
	t.Cleanup(srv.Close)

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
