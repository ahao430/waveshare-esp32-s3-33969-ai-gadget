package komari

import (
	"context"
	"errors"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

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

func TestParseNodes_DataWrapped(t *testing.T) {
	servers, err := ParseNodes(loadFixture(t, "nodes_ok.json"))
	if err != nil {
		t.Fatalf("ParseNodes: %v", err)
	}
	if len(servers) != 3 {
		t.Fatalf("servers = %d, want 3", len(servers))
	}

	s := servers[0]
	if s.UUID != "a1b2c3" || s.Name != "hk-01" {
		t.Fatalf("identity fields wrong: %+v", s)
	}
	if s.CPUPct == nil || *s.CPUPct != 23.5 {
		t.Fatalf("cpu = %v, want 23.5（已是百分数不缩放）", s.CPUPct)
	}
	if s.MemPct == nil || math.Abs(*s.MemPct-55) > 0.001 {
		t.Fatalf("mem = %v, want 55（0.55 比例归一到百分数）", s.MemPct)
	}
	if !s.Online {
		t.Fatal("默认在线语义错误")
	}

	// 第二台显式离线
	if servers[1].Online {
		t.Fatal("offline 标记丢失")
	}
}

func TestParseNodes_TopLevelArray(t *testing.T) {
	servers, err := ParseNodes([]byte(`[{"id":"x1","tag":"sg-01","cpu":0.42}]`))
	if err != nil {
		t.Fatalf("ParseNodes: %v", err)
	}
	if len(servers) != 1 || servers[0].Name != "sg-01" || *servers[0].CPUPct != 42 {
		t.Fatalf("顶层数组/id/tag 别名解析错误: %+v", servers)
	}
}

func TestParseNodes_Unsupported(t *testing.T) {
	if _, err := ParseNodes(loadFixture(t, "nodes_unexpected.json")); !errors.Is(err, ErrUnsupportedResponse) && !errors.Is(err, quota.ErrUnsupportedResponse) {
		t.Fatalf("want unsupported_response, got %v", err)
	}
}

func TestClient_Fetch(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/nodes", func(w http.ResponseWriter, req *http.Request) {
		if req.Header.Get("Authorization") != "Bearer tk" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(loadFixture(t, "nodes_ok.json"))
	})
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)

	c := NewClient(srv.URL, "tk")
	snap, err := c.Fetch(context.Background())
	if err != nil {
		t.Fatalf("Fetch: %v", err)
	}
	if snap.Source == "" || len(snap.Servers) != 3 {
		t.Fatalf("snapshot wrong: %+v", snap)
	}
}

func TestMockSource(t *testing.T) {
	m := NewMockSource()
	snap, err := m.Fetch(context.Background())
	if err != nil {
		t.Fatalf("Fetch: %v", err)
	}
	if len(snap.Servers) == 0 {
		t.Fatal("mock fleet 为空")
	}
}
