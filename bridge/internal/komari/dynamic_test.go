package komari

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
)

// 动态源：配置变更后无需重建 Service，下一次 Fetch 即用新 Source。
// 未配置（resolve→nil）语义：空快照 source="none"（不回退 mock，
// 设备/Web 端「空的就显示空的」引导去后台配置）。
func TestDynamicSourceResolvesPerFetch(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`[{"uuid":"n1","name":"vps-1","cpu":10}]`))
	}))
	t.Cleanup(srv.Close)

	useReal := false
	src := NewDynamicSource(func() Source {
		if !useReal {
			return nil // 未配置
		}
		return NewClient(srv.URL, "")
	})

	if got := src.Name(); got != "none" {
		t.Fatalf("nil resolve should be \"none\", got %q", got)
	}
	snap, err := src.Fetch(context.Background())
	if err != nil {
		t.Fatalf("unconfigured fetch should not error: %v", err)
	}
	if snap.Source != "none" || len(snap.Servers) != 0 {
		t.Fatalf("unconfigured snapshot should be empty, got %+v", snap)
	}

	useReal = true
	snap, err = src.Fetch(context.Background())
	if err != nil {
		t.Fatalf("fetch: %v", err)
	}
	if snap.Source != "komari("+srv.URL+")" || len(snap.Servers) != 1 {
		t.Fatalf("snapshot wrong: %+v", snap)
	}
}
