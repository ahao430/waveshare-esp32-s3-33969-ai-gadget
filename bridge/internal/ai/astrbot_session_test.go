// astrbot_session_test.go — 会话身份与"新对话"轮换契约：
//   每实例 username/session 独立（用户级记忆不串）；
//   轮换代持久化（Bridge 重启不得退回旧代 → 服务端旧记忆复活）；
//   reset-all 归并到全局代。
package ai

import (
	"path/filepath"
	"strings"
	"testing"

	"gadget-bridge/internal/storage"
)

type memStore struct {
	g   uint64
	per map[string]uint64
}

func (m *memStore) LoadSessionGens() (uint64, map[string]uint64, error) {
	return m.g, m.per, nil
}

func (m *memStore) SaveSessionGens(g uint64, per map[string]uint64) error {
	m.g, m.per = g, per
	return nil
}

// resetSessionState 把包级会话状态归零（测试隔离；生产代码只在启动时惰性加载一次）。
func resetSessionState(s SessionStore) {
	sessMu.Lock()
	sessBase, sessPerBot, sessStore, sessLoaded = 0, map[string]uint64{}, s, false
	sessMu.Unlock()
}

func TestAstrbotIdentityPerBot(t *testing.T) {
	resetSessionState(nil)

	ua, sa := astrbotIdentity("bot-a")
	ub, sb := astrbotIdentity("bot-b")
	if ua == ub || sa == sb {
		t.Fatalf("identity must be per-bot: %q/%q vs %q/%q", ua, sa, ub, sb)
	}
	if !strings.HasPrefix(ua, "gadget-") {
		t.Fatalf("username prefix: %q", ua)
	}
}

func TestResetRotatesUsernameAndSession(t *testing.T) {
	resetSessionState(nil)

	before, _ := astrbotIdentity("bot-a")
	ResetBotSession("bot-a")
	after, _ := astrbotIdentity("bot-a")
	if before == after {
		t.Fatal("username should rotate on reset (user-scoped memory forget)")
	}
	_, sb := astrbotIdentity("bot-a")
	if !strings.Contains(sb, "-1") {
		t.Fatalf("session gen should be 1: %q", sb)
	}
	// 其他实例不受影响
	if _, s := astrbotIdentity("bot-b"); strings.Contains(s, "-1") {
		t.Fatalf("bot-b should stay gen 0: %q", s)
	}
}

func TestSessionGensPersistAcrossRestart(t *testing.T) {
	store := &memStore{}
	resetSessionState(store)

	ResetBotSession("bot-a")
	ResetBotSession("bot-a")
	wantUser, wantSess := astrbotIdentity("bot-a")

	// 模拟 Bridge 重启：包状态清空，仅从 store 恢复
	resetSessionState(store)
	gotUser, gotSess := astrbotIdentity("bot-a")
	if gotUser != wantUser || gotSess != wantSess {
		t.Fatalf("gens must survive restart: got %q/%q want %q/%q",
			gotUser, gotSess, wantUser, wantSess)
	}
}

func TestResetAllMergesPerBotGens(t *testing.T) {
	resetSessionState(nil)

	ResetBotSession("bot-a") // per-bot gen 1
	ResetBotSession("")      // 全局：归并到 global=1
	if len(sessPerBot) != 0 {
		t.Fatalf("per-bot gens should be merged into global: %v", sessPerBot)
	}
	_, sa := astrbotIdentity("bot-a")
	_, sb := astrbotIdentity("bot-b")
	if !strings.Contains(sa, "-1") || !strings.Contains(sb, "-1") {
		t.Fatalf("all bots should be gen 1: %q %q", sa, sb)
	}
}

func TestStorageSessionGensRoundTrip(t *testing.T) {
	db, err := storage.Open(filepath.Join(t.TempDir(), "gadget.db"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer db.Close()

	if g, per := db.GetAstrbotSessionGens(); g != 0 || per != nil {
		t.Fatalf("empty db should be zero gens: %d/%v", g, per)
	}
	if err := db.SaveAstrbotSessionGens(3, map[string]uint64{"bot-x": 2}); err != nil {
		t.Fatalf("save: %v", err)
	}
	g, per := db.GetAstrbotSessionGens()
	if g != 3 || per["bot-x"] != 2 {
		t.Fatalf("round trip: %d/%v", g, per)
	}
}
