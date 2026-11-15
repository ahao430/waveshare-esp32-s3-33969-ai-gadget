// codingplan_test.go — 两种站点形状的解析回归（形状来自 AIVault
// additional_providers.dart / newapi.dart 的契约，不真网）。
package codingplan

import (
	"testing"
)

const kimiShape = `{
  "usage": {"limit": "100", "remaining": "62.5", "resetTime": "2026-11-16T00:00:00Z"},
  "limits": [
    {"window": {"duration": 300, "timeUnit": "MINUTES"},
     "detail": {"limit": "5", "remaining": "2", "resetTime": "2026-11-15T18:00:00Z"}},
    {"window": {"duration": 7, "timeUnit": "DAYS"}, "detail": {"limit": "1"}}
  ],
  "user": {"membership": {"level": "LEVEL_INTERMEDIATE"}}
}`

const newapiShape = `{"success": true, "data": {"used_quota": 2500000}}`

func TestParseUsagesShape(t *testing.T) {
	snap, ok := parseUsages([]byte(kimiShape), "gpt", "a1")
	if !ok {
		t.Fatal("kimi shape should parse")
	}
	if snap.PlanName != "pro" {
		t.Fatalf("plan = %q", snap.PlanName)
	}
	if len(snap.Windows) != 2 {
		t.Fatalf("windows = %d", len(snap.Windows))
	}
	var w5h, weekly *struct {
		id string
		remaining, limit float64
	}
	for _, w := range snap.Windows {
		switch w.ID {
		case "5h":
			if *w.Remaining != 2 || *w.Limit != 5 {
				t.Fatalf("5h = %v/%v", *w.Remaining, *w.Limit)
			}
			if w.ResetAt == nil || w.ResetAt.UTC().Hour() != 18 {
				t.Fatalf("5h reset = %v", w.ResetAt)
			}
			w5h = &struct {
				id               string
				remaining, limit float64
			}{"5h", *w.Remaining, *w.Limit}
		case "weekly":
			if *w.Remaining != 62.5 || *w.Limit != 100 {
				t.Fatalf("weekly = %v/%v", *w.Remaining, *w.Limit)
			}
			weekly = &struct {
				id               string
				remaining, limit float64
			}{"weekly", *w.Remaining, *w.Limit}
		}
	}
	if w5h == nil || weekly == nil {
		t.Fatal("missing windows")
	}
}

func TestParseNewapiShape(t *testing.T) {
	snap, ok := parseNewapiSelf([]byte(newapiShape), "claude", "a1")
	if !ok {
		t.Fatal("newapi shape should parse")
	}
	if len(snap.Credits) != 1 || *snap.Credits[0].Balance != 5.0 {
		t.Fatalf("credits = %+v", snap.Credits)
	}
}

func TestParseUnknownShapeFails(t *testing.T) {
	if _, ok := parseUsages([]byte(`{"foo": 1}`), "mimo", "a"); ok {
		t.Fatal("unknown shape must not parse as usages")
	}
	if _, ok := parseNewapiSelf([]byte(`{"success": false}`), "mimo", "a"); ok {
		t.Fatal("success=false must not parse")
	}
}
