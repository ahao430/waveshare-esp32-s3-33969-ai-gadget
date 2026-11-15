// active_bot_test.go — "当前机器人"解析回归（多实例切换的核心契约）：
//   未设置 → 第一个；设置有效 → 用它；设置指向已删除 → 回落第一个；
//   TTS 偏好跟随当前实例；删除当前实例会清设置。
package storage

import (
	"path/filepath"
	"testing"
)

func openTestDB(t *testing.T) *DB {
	t.Helper()
	db, err := Open(filepath.Join(t.TempDir(), "gadget.db"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

func seedBots(t *testing.T, db *DB) {
	t.Helper()
	key := []byte("0123456789abcdef0123456789abcdef") // SaveBot 要 masterKey
	// id 排序决定"第一个"（ListBots ORDER BY id）：bot-a < bot-b
	if err := db.SaveBot("bot-a", "大肥鱼", "", "", "", "", "", "edge", "zh-CN-XiaoxiaoNeural", key); err != nil {
		t.Fatalf("save a: %v", err)
	}
	if err := db.SaveBot("bot-b", "小助手", "", "", "", "", "", "deepseek", "tide", key); err != nil {
		t.Fatalf("save b: %v", err)
	}
}

func TestActiveBotFallbackAndSwitch(t *testing.T) {
	db := openTestDB(t)

	if _, _, ok := db.ActiveBot(); ok {
		t.Fatal("empty db should have no active bot")
	}

	seedBots(t, db)
	id, label, ok := db.ActiveBot()
	if !ok || id != "bot-a" || label != "大肥鱼" {
		t.Fatalf("default active = %s/%s ok=%v", id, label, ok)
	}

	// TTS 跟随第一个
	if p, v := db.DefaultBotTTS(); p != "edge" || v != "zh-CN-XiaoxiaoNeural" {
		t.Fatalf("tts fallback = %s/%s", p, v)
	}

	// 切换 → ActiveBot 与 TTS 都跟随
	if err := db.SaveChatActiveBot("bot-b"); err != nil {
		t.Fatalf("save active: %v", err)
	}
	id, label, ok = db.ActiveBot()
	if !ok || id != "bot-b" || label != "小助手" {
		t.Fatalf("active after switch = %s/%s", id, label)
	}
	if p, v := db.DefaultBotTTS(); p != "deepseek" || v != "tide" {
		t.Fatalf("tts after switch = %s/%s", p, v)
	}

	// 设置指向已删除 → 回落第一个（设置本身由 DeleteBot 清）
	if err := db.SaveChatActiveBot("bot-gone"); err != nil {
		t.Fatalf("save stale: %v", err)
	}
	if id, _, _ := db.ActiveBot(); id != "bot-a" {
		t.Fatalf("stale setting should fall back to bot-a, got %s", id)
	}
}

func TestDeleteActiveBotClearsSetting(t *testing.T) {
	db := openTestDB(t)
	seedBots(t, db)
	if err := db.SaveChatActiveBot("bot-b"); err != nil {
		t.Fatalf("save active: %v", err)
	}
	if err := db.DeleteBot("bot-b"); err != nil {
		t.Fatalf("delete: %v", err)
	}
	if got := db.SavedChatActiveBot(); got != "" {
		t.Fatalf("setting should be cleared, got %q", got)
	}
	if id, _, ok := db.ActiveBot(); !ok || id != "bot-a" {
		t.Fatalf("active after delete = %s ok=%v", id, ok)
	}
}
