// subscriptions.go — 订阅实例配置 API（Web 后台 CRUD）+ 聚合快照端点。
// 配置存 SQLite（token 加密）；聚合按订阅实例逐个实时抓取（并发 3、
// 超时 fetchTimeout），失败在条目内报 error 字段（不失败整请求）。
package api

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"sync"
	"time"

	"gadget-bridge/internal/quota"
	"gadget-bridge/internal/storage"
	"log/slog"
)

// subEntry 聚合端点的单条订阅视图。
type subEntry struct {
	ID       string               `json:"id"`
	Kind     string               `json:"kind"`
	Label    string               `json:"label"`
	Snapshot *quota.QuotaSnapshot `json:"snapshot,omitempty"`
	Error    string               `json:"error,omitempty"`
}

// fetchOne 抓取单条订阅（kind → Registry provider，token/base_url 从配置）。
func fetchOne(ctx context.Context, d Deps, sub storage.Subscription) subEntry {
	entry := subEntry{ID: sub.ID, Kind: sub.Kind, Label: sub.Label}
	p, ok := d.Registry.Get(sub.Kind)
	if !ok {
		entry.Error = "unknown_kind"
		return entry
	}
	_, _, baseURL, token, err := d.SubscriptionsDB.GetSubscription(sub.ID, d.MasterKey)
	if err != nil {
		entry.Error = "read_failed"
		return entry
	}
	// 实例未单独存 Key 时回落凭据表同 kind 的 Key（凭据表由保存实例时
	// 投影维护，也承载语音 ASR 的动态解析——两层一把钥匙）
	if token == "" && d.CredentialsDB != nil {
		if _, key, cerr := d.CredentialsDB.GetCredential(sub.Kind, d.MasterKey); cerr == nil {
			token = key
		}
	}
	acc := quota.AccountRef{
		ID: sub.ID, DisplayName: sub.Label, APIKey: token,
		Extra: map[string]string{"base_url": baseURL},
	}
	sctx, cancel := context.WithTimeout(ctx, fetchTimeout)
	defer cancel()
	snap, ferr := p.FetchQuota(sctx, acc)
	if ferr != nil {
		entry.Error = ferr.Error()
		return entry
	}
	entry.Snapshot = snap
	return entry
}

// serveSubscriptionsCRUD 挂 CRUD 路由。

// serveSubscriptions 挂全部订阅路由（Deps.SubscriptionsDB 非 nil 时）。
func serveSubscriptions(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	mux.HandleFunc("GET /api/v1/subscriptions", func(w http.ResponseWriter, r *http.Request) {
		list, err := db.ListSubscriptions()
		if err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "list_failed", err.Error())
			return
		}
		writeJSON(w, logger, http.StatusOK, map[string]any{"subscriptions": list})
	})

	mux.HandleFunc("POST /api/v1/subscriptions", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			ID      string `json:"id"`
			Kind    string `json:"kind"`
			Label   string `json:"label"`
			BaseURL string `json:"base_url"`
			Token   string `json:"token"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 64*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if !storage.ValidKind(body.Kind) {
			writeErr(w, logger, http.StatusBadRequest, "bad_kind", "want zhipu|kimi|opencode|newapi|deepseek|gpt|claude|minimax|mimo|mock（机器人请用 /api/v1/bots）")
			return
		}
		id := body.ID
		if id == "" {
			id = "sub-" + randomID()
		}
		// 编辑语义：id 已存在且 token 留空 → 保留旧 Key（Web 表单
		// 「留空=不修改」，与机器人实例编辑一致）
		token := body.Token
		if token == "" && body.ID != "" {
			if _, _, _, old, gerr := db.GetSubscription(id, d.MasterKey); gerr == nil {
				token = old
			}
		}
		if err := db.SaveSubscription(id, body.Kind, body.Label, body.BaseURL, token, d.MasterKey); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		// Key 投影到凭据表：订阅实例是唯一编辑入口，但语音 ASR/智谱预热
		// 仍从凭据表动态解析（resolveAccount）——实例里改 Key 两处同步。
		if token != "" && d.CredentialsDB != nil {
			if err := d.CredentialsDB.SaveCredential(body.Kind, body.Label, token, d.MasterKey); err != nil {
				logger.Warn("credential projection failed", "kind", body.Kind, "err", err)
			}
		}
		logger.Info("subscription saved", "id", id, "kind", body.Kind, "label", body.Label)
		writeJSON(w, logger, http.StatusOK, map[string]any{"id": id})
	})

	mux.HandleFunc("DELETE /api/v1/subscriptions/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := r.PathValue("id")
		if err := db.DeleteSubscription(id); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "delete_failed", err.Error())
			return
		}
		logger.Info("subscription deleted", "id", id)
		writeJSON(w, logger, http.StatusOK, map[string]bool{"deleted": true})
	})

	// 聚合快照：所有订阅实例逐个实时抓取（并发 3）。
	mux.HandleFunc("GET /api/v1/quota/subscriptions", func(w http.ResponseWriter, r *http.Request) {
		list, err := db.ListSubscriptions()
		if err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "list_failed", err.Error())
			return
		}
		entries := make([]subEntry, len(list))
		sem := make(chan struct{}, 3)
		var wg sync.WaitGroup
		for i, sub := range list {
			wg.Add(1)
			go func(i int, sub storage.Subscription) {
				defer wg.Done()
				sem <- struct{}{}
				entries[i] = fetchOne(r.Context(), d, sub)
				<-sem
			}(i, sub)
		}
		wg.Wait()
		writeJSON(w, logger, http.StatusOK, map[string]any{
			"subscriptions": entries, "fetched_at": time.Now().UTC(),
		})
	})
}

// randomID 生成 8 字节 hex 随机 id。
func randomID() string {
	b := make([]byte, 4)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}
