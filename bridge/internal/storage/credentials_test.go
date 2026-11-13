package storage

import (
	"errors"
	"path/filepath"
	"testing"

	"gadget-bridge/internal/security"
)

var testKey, _ = security.ParseMasterKey("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff")

func TestCredentialRoundtrip(t *testing.T) {
	db, err := Open(filepath.Join(t.TempDir(), "g.db"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer db.Close()

	if err := db.SaveCredential("zhipu", "主账号", "sk-abc123", testKey); err != nil {
		t.Fatalf("save: %v", err)
	}
	label, key, err := db.GetCredential("zhipu", testKey)
	if err != nil || label != "主账号" || key != "sk-abc123" {
		t.Fatalf("get: %v %q %q", err, label, key)
	}

	// upsert 覆盖
	if err := db.SaveCredential("zhipu", "新标签", "sk-new", testKey); err != nil {
		t.Fatalf("save2: %v", err)
	}
	_, key, _ = db.GetCredential("zhipu", testKey)
	_ = key
	if key != "sk-new" {
		t.Fatalf("upsert 未覆盖: %q", key)
	}

	// 列表无明文
	list, err := db.ListCredentials()
	if err != nil || len(list) != 1 || list[0].ProviderID != "zhipu" || list[0].Label != "新标签" {
		t.Fatalf("list: %v %+v", err, list)
	}

	// 删除幂等
	if err := db.DeleteCredential("zhipu"); err != nil {
		t.Fatalf("delete: %v", err)
	}
	if err := db.DeleteCredential("zhipu"); err != nil {
		t.Fatalf("delete idempotent: %v", err)
	}
	if _, _, err := db.GetCredential("zhipu", testKey); !errors.Is(err, ErrNotFound) {
		t.Fatalf("删除后应 NotFound, got %v", err)
	}
}

func TestCredentialNoMasterKey(t *testing.T) {
	db, _ := Open(filepath.Join(t.TempDir(), "g.db"))
	defer db.Close()
	if err := db.SaveCredential("zhipu", "l", "k", nil); err == nil {
		t.Fatal("无 master key 保存应拒绝")
	}
	if _, _, err := db.GetCredential("zhipu", nil); err == nil {
		t.Fatal("无 master key 读取应拒绝")
	}
}
