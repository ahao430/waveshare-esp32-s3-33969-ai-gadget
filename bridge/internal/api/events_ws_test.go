package api

import (
	"encoding/json"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"gadget-bridge/internal/events"
	"gadget-bridge/internal/quota"
	"gadget-bridge/internal/quota/mock"
)

// 集成测试：WS 订阅 → POST refresh → 客户端收到 quota.mock 事件。
func TestEventsWebSocketPush(t *testing.T) {
	hub := events.NewHub()
	reg := quota.NewRegistry()
	if err := reg.Register(mock.New()); err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(Deps{
		Registry: reg,
		Store:    quota.NewStore(),
		Logger:   nil,
		Events:   hub,
		Accounts: func(string) []quota.AccountRef { return nil },
	})
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)

	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http") + "/api/v1/events"
	conn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	// 等订阅生效后触发刷新
	time.Sleep(100 * time.Millisecond)
	refresh := srv.Client()
	if _, err := refresh.Post(srv.URL+"/api/v1/quota/mock/refresh", "application/json", nil); err != nil {
		t.Fatalf("refresh: %v", err)
	}

	conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	_, raw, err := conn.ReadMessage()
	if err != nil {
		t.Fatalf("read ws: %v", err)
	}

	var msg struct {
		Topic string `json:"topic"`
		Data  struct {
			Snapshot struct {
				ProviderID string `json:"provider_id"`
				Status     string `json:"status"`
			} `json:"snapshot"`
			Stale bool `json:"stale"`
		} `json:"data"`
	}
	if err := json.Unmarshal(raw, &msg); err != nil {
		t.Fatalf("unmarshal %s: %v", raw, err)
	}
	if msg.Topic != "quota.mock" {
		t.Fatalf("topic = %s, want quota.mock", msg.Topic)
	}
	if msg.Data.Snapshot.ProviderID != "mock" || msg.Data.Snapshot.Status != "ok" {
		t.Fatalf("payload wrong: %s", raw)
	}
}

// Hub 直接发布 → 客户端收到任意主题（komari 路径复用同一机制）。
func TestEventsWebSocketCustomTopic(t *testing.T) {
	hub := events.NewHub()
	handler := NewHandler(Deps{Events: hub})
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)

	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http") + "/api/v1/events"
	conn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	time.Sleep(100 * time.Millisecond)
	go func() {
		for i := 0; i < 20; i++ {
			hub.Publish("komari.updated", map[string]int{"n": 1})
			time.Sleep(50 * time.Millisecond)
		}
	}()

	conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	for {
		_, raw, err := conn.ReadMessage()
		if err != nil {
			t.Fatalf("read ws: %v", err)
		}
		if strings.Contains(string(raw), "komari.updated") {
			return // 收到即通过
		}
	}
}
