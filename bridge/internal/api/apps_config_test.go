package api

// 应用配置域 e2e：bots（机器人 CRUD+probe+迁移）、keys（按键映射下发）、
// monitor（Komari 配置→动态源）、zcode（配置+会话 mock/远端）。

import (
	"bytes"
	"encoding/json"
	"io"
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
	srv, db := newConfigServerWith(t, masterKey, nil)
	return srv, db
}

// newConfigServerWith 额外注入 Deps（如 Weather）。
func newConfigServerWith(t *testing.T, masterKey []byte, extra func(*Deps)) (*httptest.Server, *storage.DB) {
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
	d := Deps{
		Registry:        reg,
		Store:           quota.NewStore(),
		SubscriptionsDB: db,
		MasterKey:       masterKey,
	}
	if extra != nil {
		extra(&d)
	}
	srv := httptest.NewServer(NewHandler(d))
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

	var lastChatBody map[string]any
	// 假 AstrBot：校验 Bearer + 返回 SSE 片段；记录请求体供 config 断言
	bot := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/v1/chat/configs" {
			if r.Header.Get("Authorization") != "Bearer abk-test" {
				http.Error(w, "unauthorized", 401)
				return
			}
			_, _ = w.Write([]byte(`{"status":"ok","configs":[
				{"id":"default","name":"默认配置","is_default":true},
				{"id":"conf-maid","name":"女仆人设","is_default":false}]}`))
			return
		}
		if r.URL.Path != "/api/v1/chat" {
			http.NotFound(w, r)
			return
		}
		if got := r.Header.Get("Authorization"); got != "Bearer abk-test" {
			http.Error(w, "unauthorized", 401)
			return
		}
		body, _ := io.ReadAll(r.Body)
		_ = json.Unmarshal(body, &lastChatBody)
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = w.Write([]byte("data: {\"result\":\"pong\"}\n\n"))
	}))
	t.Cleanup(bot.Close)

	// 1) 公用连接：地址 + Token（实例共用）
	code, out := doJSON(t, "PUT", srv.URL+"/api/v1/astrbot/config",
		`{"base_url":"`+bot.URL+`","token":"abk-test"}`)
	if code != 200 {
		t.Fatalf("save conn: %d %v", code, out)
	}
	code, out = doJSON(t, "GET", srv.URL+"/api/v1/astrbot/config", "")
	if code != 200 || out["has_token"] != true || out["base_url"] != bot.URL {
		t.Fatalf("conn view: %d %v", code, out)
	}

	// 2) 实例只带 名称 + 配置文件（+ 可选 provider/model 覆盖）
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/bots",
		`{"label":"客厅机器人","config_id":"conf-maid","provider":"openai","model":"gpt-4o"}`)
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
	if b0["label"] != "客厅机器人" || b0["config_id"] != "conf-maid" {
		t.Fatalf("bot view wrong: %v", b0)
	}
	if _, leaked := b0["token"]; leaked {
		t.Fatal("token 不得回显")
	}

	// 3) 公用连接拉配置文件列表
	code, out = doJSON(t, "GET", srv.URL+"/api/v1/astrbot/configs", "")
	if code != 200 || out["ok"] != true {
		t.Fatalf("configs: %d %v", code, out)
	}
	if cfgs, _ := out["configs"].([]any); len(cfgs) != 2 {
		t.Fatalf("want 2 configs, got %v", out["configs"])
	}

	// 4) 实例 probe：公用连接 + 该实例 config/provider/model
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/bots/probe", `{"id":"`+id+`"}`)
	if code != 200 || out["ok"] != true || !strings.Contains(out["reply"].(string), "pong") {
		t.Fatalf("probe: %d %v", code, out)
	}
	if lastChatBody["config_id"] != "conf-maid" ||
		lastChatBody["selected_provider"] != "openai" ||
		lastChatBody["selected_model"] != "gpt-4o" {
		t.Fatalf("chat payload missing overrides: %v", lastChatBody)
	}

	// 5) 编辑（名称/配置）后 probe 仍通
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/bots",
		`{"id":"`+id+`","label":"客厅机器人改名","config_id":"conf-maid"}`)
	if code != 200 {
		t.Fatalf("edit: %d %v", code, out)
	}
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/bots/probe", `{"id":"`+id+`"}`)
	if code != 200 || out["ok"] != true {
		t.Fatalf("probe after edit: %d %v", code, out)
	}

	// 6) 错误 token 的公用连接 → probe 失败
	_, _ = doJSON(t, "PUT", srv.URL+"/api/v1/astrbot/config",
		`{"base_url":"`+bot.URL+`","token":"abk-wrong"}`)
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/bots/probe", `{"id":"`+id+`"}`)
	if code != 200 || out["ok"] != false {
		t.Fatalf("probe should fail with wrong token: %d %v", code, out)
	}

	// 7) 配置列表 403 → 明确提示 config scope
	bot2 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "forbidden", 403)
	}))
	t.Cleanup(bot2.Close)
	_, _ = doJSON(t, "PUT", srv.URL+"/api/v1/astrbot/config", `{"base_url":"`+bot2.URL+`"}`)
	_, out = doJSON(t, "GET", srv.URL+"/api/v1/astrbot/configs", "")
	if out["ok"] != false || !strings.Contains(out["error"].(string), "config") {
		t.Fatalf("403 hint wrong: %v", out)
	}

	// 8) 删除幂等
	code, _ = doJSON(t, "DELETE", srv.URL+"/api/v1/bots/"+id, "")
	if code != 200 {
		t.Fatalf("delete: %d", code)
	}
	_, out = doJSON(t, "GET", srv.URL+"/api/v1/bots", "")
	if n := len(out["bots"].([]any)); n != 0 {
		t.Fatalf("want 0 after delete, got %d", n)
	}
}

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

// ---- launcher 布局 ----

func TestLauncherLayout(t *testing.T) {
	mk := newTestMasterKey(t)
	srv, _ := newConfigServer(t, mk)

	// 默认 = 出厂顺序（8 个全可见）
	code, out := doJSON(t, "GET", srv.URL+"/api/v1/launcher", "")
	if code != 200 {
		t.Fatalf("get: %d", code)
	}
	order, _ := out["order"].([]any)
	if len(order) != 9 || order[0] != "chat" || order[8] != "music" {
		t.Fatalf("default order wrong: %v", order)
	}
	if cat, _ := out["catalog"].([]any); len(cat) != 9 {
		t.Fatalf("catalog wrong: %v", cat)
	}

	// 重排：5 个提到前面；其余内置 App 自动追加到末尾（不隐藏）
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/launcher",
		`{"order":["settings","weather","chat","sub","music"]}`)
	if code != 200 {
		t.Fatalf("post: %d %v", code, out)
	}
	_, out = doJSON(t, "GET", srv.URL+"/api/v1/launcher", "")
	order, _ = out["order"].([]any)
	if len(order) != 9 || order[0] != "settings" || order[4] != "music" {
		t.Fatalf("order not persisted/normalized: %v", order)
	}

	// 非法：未知 key / 重复 / 空
	if code, _ = doJSON(t, "POST", srv.URL+"/api/v1/launcher", `{"order":["chat","nope"]}`); code != 400 {
		t.Fatalf("unknown key want 400, got %d", code)
	}
	if code, _ = doJSON(t, "POST", srv.URL+"/api/v1/launcher", `{"order":["chat","chat"]}`); code != 400 {
		t.Fatalf("dup key want 400, got %d", code)
	}
	if code, _ = doJSON(t, "POST", srv.URL+"/api/v1/launcher", `{"order":[]}`); code != 400 {
		t.Fatalf("empty want 400, got %d", code)
	}

	// 损坏存储回落出厂（db 句柄直写坏 JSON）
	srv2, db2 := newConfigServer(t, mk)
	if err := db2.SaveSetting(storage.SettingKeyLauncher, "{broken"); err != nil {
		t.Fatal(err)
	}
	_, out = doJSON(t, "GET", srv2.URL+"/api/v1/launcher", "")
	if order, _ = out["order"].([]any); len(order) != 9 || order[0] != "chat" {
		t.Fatalf("corrupt setting should fall back to default, got %v", order)
	}
}

// 2026-11-16 删 zcode/monitor 后：旧排布里的已下线 key 被过滤，
// 而不是整单回落出厂重置用户自定义（设备端 defs 匹配本来就跳过未知 key）。
// 同批新增的 files 不在旧排布里：排布只决定顺序，缺的内置 App 追加到
// 末尾——否则设备开机先全量注册（文件出现一下）、联网拉到旧排布重建后
// 消失（真机反馈回归）。
func TestLauncherLegacyOrderMigrates(t *testing.T) {
	mk := newTestMasterKey(t)
	srv, db := newConfigServer(t, mk)
	if err := db.SaveSetting(storage.SettingKeyLauncher,
		`{"order":["chat","zcode","settings","monitor"]}`); err != nil {
		t.Fatal(err)
	}
	_, out := doJSON(t, "GET", srv.URL+"/api/v1/launcher", "")
	order, _ := out["order"].([]any)
	if len(order) != 9 || order[0] != "chat" || order[1] != "settings" {
		t.Fatalf("legacy order should filter retired keys then append new, got %v", order)
	}
	has := map[string]bool{}
	for _, k := range order {
		has[k.(string)] = true
	}
	if has["zcode"] || has["monitor"] {
		t.Fatalf("retired keys should be gone, got %v", order)
	}
	if !has["files"] {
		t.Fatalf("files (new since saved layout) should be appended, got %v", order)
	}
}

// 旧版 v2 配置里的 hidden 字段已废弃：内置 App 一律显示，只取 order。
func TestLauncherIgnoresLegacyHidden(t *testing.T) {
	mk := newTestMasterKey(t)
	srv, db := newConfigServer(t, mk)
	if err := db.SaveSetting(storage.SettingKeyLauncher,
		`{"version":2,"order":["settings","chat"],"hidden":["weather","music"]}`); err != nil {
		t.Fatal(err)
	}
	_, out := doJSON(t, "GET", srv.URL+"/api/v1/launcher", "")
	order, _ := out["order"].([]any)
	// settings/chat 原序 → 其余内置 App（含曾标记 hidden 的）全部追加
	if len(order) != 9 || order[0] != "settings" || order[1] != "chat" {
		t.Fatalf("builtin apps must all be visible, got %v", order)
	}
	has := map[string]bool{}
	for _, k := range order {
		has[k.(string)] = true
	}
	if !has["weather"] || !has["music"] || !has["files"] {
		t.Fatalf("legacy hidden must be ignored, got %v", order)
	}
}

// POST 保存归一化为完整排布（内置 App 全部在列、未排的补末尾），
// 与设备端「排布只排序不隐藏」的行为一致。
func TestLauncherSaveNormalizesOrder(t *testing.T) {
	mk := newTestMasterKey(t)
	srv, db := newConfigServer(t, mk)
	if code, out := doJSON(t, "POST", srv.URL+"/api/v1/launcher",
		`{"order":["chat","settings"]}`); code != 200 {
		t.Fatalf("post: %d %v", code, out)
	}
	raw, err := db.GetSetting(storage.SettingKeyLauncher)
	if err != nil {
		t.Fatal(err)
	}
	var saved struct {
		Order []string `json:"order"`
	}
	if err := json.Unmarshal([]byte(raw), &saved); err != nil {
		t.Fatal(err)
	}
	if len(saved.Order) != 9 || saved.Order[0] != "chat" || saved.Order[1] != "settings" {
		t.Fatalf("save should normalize to full order, got %s", raw)
	}
	_, out := doJSON(t, "GET", srv.URL+"/api/v1/launcher", "")
	if order, _ := out["order"].([]any); len(order) != 9 {
		t.Fatalf("all builtin apps should be visible after save, got %v", order)
	}
}

// ---- 当前机器人（多实例切换） ----

func TestChatActiveBotSwitch(t *testing.T) {
	mk := newTestMasterKey(t)
	srv, _ := newConfigServer(t, mk)
	defer srv.Close()

	for _, b := range []struct{ id, label string }{
		{"bot-a", "大肥鱼"}, {"bot-b", "小助手"},
	} {
		body := `{"id":"` + b.id + `","label":"` + b.label + `"}`
		if code, out := doJSON(t, "POST", srv.URL+"/api/v1/bots", body); code != 200 {
			t.Fatalf("create %s: %d %s", b.id, code, out)
		}
	}

	// 未设置：回落第一个，且带 fallback 标记
	code, m := doJSON(t, "GET", srv.URL+"/api/v1/chat/active", "")
	if code != 200 || m["id"] != "bot-a" || m["fallback"] != true {
		t.Fatalf("default active: %d %v", code, m)
	}

	// 切换到 bot-b
	if code, m = doJSON(t, "PUT", srv.URL+"/api/v1/chat/active", `{"id":"bot-b"}`); code != 200 {
		t.Fatalf("switch: %d %v", code, m)
	}
	_, m = doJSON(t, "GET", srv.URL+"/api/v1/chat/active", "")
	if m["id"] != "bot-b" || m["label"] != "小助手" {
		t.Fatalf("after switch: %v", m)
	}
	if _, ok := m["fallback"]; ok {
		t.Fatalf("explicit setting must not be marked fallback: %v", m)
	}

	// 不存在的实例 → 404
	if code, _ = doJSON(t, "PUT", srv.URL+"/api/v1/chat/active", `{"id":"nope"}`); code != 404 {
		t.Fatalf("unknown id should 404, got %d", code)
	}
	// 空 id → 400
	if code, _ = doJSON(t, "PUT", srv.URL+"/api/v1/chat/active", `{}`); code != 400 {
		t.Fatalf("empty id should 400, got %d", code)
	}
}
