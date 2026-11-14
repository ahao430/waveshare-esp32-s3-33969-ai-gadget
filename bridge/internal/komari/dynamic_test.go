package komari

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
)

// 动态源：配置变更后无需重建 Service，下一次 Fetch 即用新 Source。
func TestDynamicSourceResolvesPerFetch(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`[{"uuid":"n1","name":"vps-1","cpu":10}]`))
	}))
	t.Cleanup(srv.Close)

	useReal := false
	src := NewDynamicSource(func() Source {
		if !useReal {
			return nil // nil 回退 mock
		}
		return NewClient(srv.URL, "")
	})

	if got := src.Name(); got != "mock" {
		t.Fatalf("nil resolve should fall back to mock, got %q", got)
	}

	useReal = true
	snap, err := src.Fetch(context.Background())
	if err != nil {
		t.Fatalf("fetch: %v", err)
	}
	if snap.Source != "komari("+srv.URL+")" || len(snap.Servers) != 1 {
		t.Fatalf("snapshot wrong: %+v", snap)
	}
}
