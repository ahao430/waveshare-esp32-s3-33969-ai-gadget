package api

// 应用配置域 e2e：bots（机器人 CRUD+probe+迁移）、keys（按键映射下发）、
// monitor（Komari 配置→动态源）、zcode（配置+会话 mock/远端）。

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"

	"gadget-bridge/internal/quota"
	"gadget-bridge/internal/quota/mock"
	"gadget-bridge/internal/security"
	"gadget-bridge/internal/storage"
)

func newTestMasterKey(t *testing.T) []byte {
	t.Helper()
	key, err := security.GenerateMasterKey()
	if err != nil {
		t.Fatal(err)
	}
	mk, err := security.ParseMasterKey(key)
	if err != nil {
		t.Fatal(err)
	}
	return mk
}

// newConfigServer 装配带 storage.DB 的 API（内存库）。
func newConfigServer(t *testing.T, masterKey []byte) (*httptest.Server, *storage.DB) {
	t.Helper()
	db, err := storage.Open("")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	reg := quota.NewRegistry()
	if err := reg.Register(mock.New()); err != nil {
		t.Fatal(err)
	}
	h := NewHandler(Deps{
		Registry:        reg,
		Store:           quota.NewStore(),
		SubscriptionsDB: db,
		MasterKey:       masterKey,
	})
	srv := httptest.NewServer(h)
	t.Cleanup(srv.Close)
	return srv, db
}

func doJSON(t *testing.T, method, url, body string) (int, map[string]any) {
	t.Helper()
	var rdr *bytes.Reader
	if body == "" {
		rdr = bytes.NewReader(nil)
	} else {
		rdr = bytes.NewReader([]byte(body))
	}
	req, _ := http.NewRequest(method, url, rdr)
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var out map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&out)
	return resp.StatusCode, out
}

// ---- bots ----

func TestBotsCRUDAndProbe(t *testing.T) {
	mk := newTestMasterKey(t)
	srv, _ := newConfigServer(t, mk)

	// 假 AstrBot：校验 Bearer + 返回 SSE 片段
	bot := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/chat" {
			http.NotFound(w, r)
			return
		}
		if got := r.Header.Get("Authorization"); got != "Bearer abk-test" {
			http.Error(w, "unauthorized", 401)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = w.Write([]byte("data: {\"result\":\"pong\"}\n\n"))
	}))
	t.Cleanup(bot.Close)

	// 空 → 有
	code, out := doJSON(t, "POST", srv.URL+"/api/v1/bots",
		`{"label":"客厅机器人","base_url":"`+bot.URL+`","token":"abk-test"}`)
	if code != 200 {
		t.Fatalf("create bot: %d %v", code, out)
	}
	id, _ := out["id"].(string)
	if !strings.HasPrefix(id, "bot-") {
		t.Fatalf("want bot- prefix, got %q", id)
	}

	code, out = doJSON(t, "GET", srv.URL+"/api/v1/bots", "")
	if code != 200 {
		t.Fatalf("list: %d", code)
	}
	bots, _ := out["bots"].([]any)
	if len(bots) != 1 {
		t.Fatalf("want 1 bot, got %v", out)
	}
	b0 := bots[0].(map[string]any)
	if b0["label"] != "客厅机器人" || b0["has_token"] != true {
		t.Fatalf("bot view wrong: %v", b0)
	}
	if _, leaked := b0["token"]; leaked {
		t.Fatal("token 不得回显")
	}

	// probe（用已保存 id；假服务验证凭据链路）
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/bots/probe", `{"id":"`+id+`"}`)
	if code != 200 || out["ok"] != true || !strings.Contains(out["reply"].(string), "pong") {
		t.Fatalf("probe: %d %v", code, out)
	}

	// 错误 token 的 probe → ok=false
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/bots/probe",
		`{"base_url":"`+bot.URL+`","token":"abk-wrong"}`)
	if code != 200 || out["ok"] != false {
		t.Fatalf("probe should fail: %d %v", code, out)
	}

	// 删除幂等
	code, _ = doJSON(t, "DELETE", srv.URL+"/api/v1/bots/"+id, "")
	if code != 200 {
		t.Fatalf("delete: %d", code)
	}
	_, out = doJSON(t, "GET", srv.URL+"/api/v1/bots", "")
	if n := len(out["bots"].([]any)); n != 0 {
		t.Fatalf("want 0 after delete, got %d", n)
	}
}

// 旧版 astrbot 订阅 → 打开即迁移到 bots。
func TestAstrBotSubscriptionMigratesToBots(t *testing.T) {
	path := filepath.Join(t.TempDir(), "gadget.db")
	mk := newTestMasterKey(t)

	db, err := storage.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := db.SaveSubscription("bot-legacy", "astrbot", "旧机器人",
		"http://10.0.0.5:6185", "abk-old", mk); err != nil {
		t.Fatal(err)
	}
	db.Close()

	// 重新打开触发迁移
	db2, err := storage.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer db2.Close()
	bots, err := db2.ListBots()
	if err != nil || len(bots) != 1 {
		t.Fatalf("migrated bots: %v %v", bots, err)
	}
	if bots[0].ID != "bot-legacy" || bots[0].BaseURL != "http://10.0.0.5:6185" || !bots[0].HasToken {
		t.Fatalf("migrated row wrong: %+v", bots[0])
	}
	subs, _ := db2.ListSubscriptions()
	if len(subs) != 0 {
		t.Fatalf("astrbot 订阅应已迁走: %v", subs)
	}
	// 迁移后再打开（幂等）
	db2.Close()
	db3, err := storage.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer db3.Close()
	if bots2, _ := db3.ListBots(); len(bots2) != 1 {
		t.Fatalf("migration not idempotent: %v", bots2)
	}
}

// ---- keys ----

func TestKeysConfig(t *testing.T) {
	mk := newTestMasterKey(t)
	srv, _ := newConfigServer(t, mk)

	code, out := doJSON(t, "GET", srv.URL+"/api/v1/keys", "")
	if code != 200 {
		t.Fatalf("get: %d", code)
	}
	km := out["keymap"].(map[string]any)
	if km["pwr_short"] != "home" || km["boot_short"] != "back" ||
		km["key3_short"] != "app" || km["key3_long"] != "ptt" {
		t.Fatalf("default keymap wrong: %v", km)
	}
	if _, ok := out["fixed"]; !ok {
		t.Fatal("want fixed long-press info")
	}

	// 修改 + 回读
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/keys",
		`{"pwr_short":"none","boot_short":"home","key3_short":"back","key3_long":"none"}`)
	if code != 200 {
		t.Fatalf("post: %d %v", code, out)
	}
	_, out = doJSON(t, "GET", srv.URL+"/api/v1/keys", "")
	km = out["keymap"].(map[string]any)
	if km["boot_short"] != "home" || km["key3_long"] != "none" {
		t.Fatalf("keymap not persisted: %v", km)
	}

	// 非法动作拒绝（pwr_short 无 back）
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/keys", `{"pwr_short":"back"}`)
	if code != 400 {
		t.Fatalf("want 400, got %d %v", code, out)
	}

	// 恢复默认
	code, _ = doJSON(t, "POST", srv.URL+"/api/v1/keys", `{}`)
	if code != 200 {
		t.Fatalf("reset: %d", code)
	}
}

// ---- monitor ----

func TestMonitorConfigDynamicSource(t *testing.T) {
	mk := newTestMasterKey(t)
	srv, _ := newConfigServer(t, mk)

	// 未配置 → mock 源（nodes 端点走 Deps.Komari；这里直接构造动态源验证）
	nodes := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/nodes" {
			http.NotFound(w, r)
			return
		}
		_, _ = w.Write([]byte(`[{"uuid":"n1","name":"vps-1","cpu":23.5,"online":true}]`))
	}))
	t.Cleanup(nodes.Close)

	// 配置保存 + token 不回显
	code, out := doJSON(t, "POST", srv.URL+"/api/v1/monitor/config",
		`{"base_url":"`+nodes.URL+`","token":"sec-token"}`)
	if code != 200 {
		t.Fatalf("save monitor: %d %v", code, out)
	}
	code, out = doJSON(t, "GET", srv.URL+"/api/v1/monitor/config", "")
	if code != 200 || out["base_url"] != nodes.URL || out["has_token"] != true {
		t.Fatalf("monitor view: %v", out)
	}
	if _, leaked := out["token"]; leaked {
		t.Fatal("token 不得回显")
	}

	// 删除 → 回到未配置
	code, _ = doJSON(t, "DELETE", srv.URL+"/api/v1/monitor/config", "")
	if code != 200 {
		t.Fatalf("delete monitor: %d", code)
	}
	_, out = doJSON(t, "GET", srv.URL+"/api/v1/monitor/config", "")
	if out["base_url"] != "" {
		t.Fatalf("want empty after delete: %v", out)
	}
}

// ---- zcode ----

func TestZcodeConfigAndSessions(t *testing.T) {
	mk := newTestMasterKey(t)
	srv, _ := newConfigServer(t, mk)

	// 未配置 → mock 会话
	code, out := doJSON(t, "GET", srv.URL+"/api/v1/zcode/sessions", "")
	if code != 200 || out["source"] != "mock" {
		t.Fatalf("mock sessions: %d %v", code, out)
	}
	if n := len(out["sessions"].([]any)); n == 0 {
		t.Fatal("mock sessions empty")
	}

	// 配置远端 → 透传
	remote := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer zk-1" {
			http.Error(w, "unauthorized", 401)
			return
		}
		_, _ = w.Write([]byte(`{"sessions":[{"id":"s1","agent":"GLM","project":"demo",
			"status":"running","pending_confirm":0,"completed":3,"failed":0}]}`))
	}))
	t.Cleanup(remote.Close)

	if code, _ = doJSON(t, "POST", srv.URL+"/api/v1/zcode/config",
		`{"base_url":"`+remote.URL+`","token":"zk-1"}`); code != 200 {
		t.Fatalf("save zcode: %d", code)
	}
	code, out = doJSON(t, "GET", srv.URL+"/api/v1/zcode/sessions", "")
	if code != 200 || out["source"] != "zcode" {
		t.Fatalf("remote sessions: %d %v", code, out)
	}
	sessions := out["sessions"].([]any)
	if len(sessions) != 1 || sessions[0].(map[string]any)["project"] != "demo" {
		t.Fatalf("sessions wrong: %v", sessions)
	}

	// token 不回显
	_, out = doJSON(t, "GET", srv.URL+"/api/v1/zcode/config", "")
	if out["has_token"] != true {
		t.Fatalf("zcode config view: %v", out)
	}
	if _, leaked := out["token"]; leaked {
		t.Fatal("token 不得回显")
	}
}
