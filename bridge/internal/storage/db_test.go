package storage

import (
	"fmt"
	"path/filepath"
	"testing"
	"time"

	"gadget-bridge/internal/quota"
)

func TestSaveAndReload(t *testing.T) {
	path := filepath.Join(t.TempDir(), "gadget.db")

	func() {
		db, err := Open(path)
		if err != nil {
			t.Fatalf("open: %v", err)
		}
		defer db.Close()

		now := time.Now().UTC().Truncate(time.Second) // JSON 往返丢纳秒
		snap := &quota.QuotaSnapshot{
			ProviderID: "mock",
			AccountID:  "default",
			FetchedAt:  now,
			Status:     quota.StatusOK,
			Windows: []quota.QuotaWindow{{
				ID:          "5h",
				Label:       "5 小时窗口",
				UsedPercent: ptr(42.5),
			}},
		}
		view := quota.StoredSnapshot{Snapshot: snap, FetchedAt: now}
		if err := db.SaveStored("mock", view); err != nil {
			t.Fatalf("save: %v", err)
		}
		// 再写一条 stale 错误态，验证 roundtrip
		errView := quota.StoredSnapshot{
			Snapshot:  snap,
			Stale:     true,
			LastError: "http_status_500",
			FetchedAt: now,
		}
		if err := db.SaveStored("mock", errView); err != nil {
			t.Fatalf("save err view: %v", err)
		}
	}()

	db2, err := Open(path)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer db2.Close()

	loaded, errs := db2.LoadAll()
	if len(errs) > 0 {
		t.Fatalf("load errors: %v", errs)
	}
	view, ok := loaded["mock"]
	if !ok {
		t.Fatal("mock snapshot 未恢复")
	}
	if !view.Stale || view.LastError != "http_status_500" {
		t.Fatalf("stale 语义丢失: %+v", view)
	}
	if view.Snapshot == nil || len(view.Snapshot.Windows) != 1 {
		t.Fatalf("snapshot 丢失: %+v", view.Snapshot)
	}
	if w := view.Snapshot.Windows[0]; w.UsedPercent == nil || *w.UsedPercent != 42.5 {
		t.Fatalf("窗口数据丢失: %+v", w)
	}
}

func TestLoadAllSkipsCorruptRow(t *testing.T) {
	db, err := Open(filepath.Join(t.TempDir(), "gadget.db"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer db.Close()
	if _, err := db.sql.Exec(`INSERT INTO snapshot_cache VALUES ('bad', '{not json', 0)`); err != nil {
		t.Fatalf("seed: %v", err)
	}
	loaded, errs := db2Load(db)
	if len(errs) == 0 {
		t.Fatal("损坏行应产生错误")
	}
	if _, exists := loaded["bad"]; exists {
		t.Fatal("损坏行不应出现在结果中")
	}
}

func db2Load(db *DB) (map[string]quota.StoredSnapshot, []error) {
	return db.LoadAll()
}

func ptr(v float64) *float64 { return &v }

func TestSubscriptionOrder(t *testing.T) {
	db, err := Open(filepath.Join(t.TempDir(), "gadget.db"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer db.Close()
	mk := []byte("0123456789abcdef0123456789abcdef")

	for _, id := range []string{"sub-a", "sub-b", "sub-c"} {
		if err := db.SaveSubscription(id, "mock", id, "", "tok", mk); err != nil {
			t.Fatalf("save %s: %v", id, err)
		}
	}
	// 新建顺序 = sort 1..3；编辑不改序（仍按 sort 而非 updated_at）
	if err := db.SaveSubscription("sub-b", "mock", "B2", "", "tok2", mk); err != nil {
		t.Fatalf("edit: %v", err)
	}
	got := ids(db.ListSubscriptions)
	if fmt.Sprint(got) != "[sub-a sub-b sub-c]" {
		t.Fatalf("insert order: %v", got)
	}
	// 用户排序：c,b,a
	if err := db.SetSubscriptionOrder([]string{"sub-c", "sub-b", "sub-a"}); err != nil {
		t.Fatalf("order: %v", err)
	}
	got = ids(db.ListSubscriptions)
	if fmt.Sprint(got) != "[sub-c sub-b sub-a]" {
		t.Fatalf("user order: %v", got)
	}
}

func ids(list func() ([]Subscription, error)) []string {
	subs, err := list()
	if err != nil {
		panic(err)
	}
	out := []string{}
	for _, s := range subs {
		out = append(out, s.ID)
	}
	return out
}
