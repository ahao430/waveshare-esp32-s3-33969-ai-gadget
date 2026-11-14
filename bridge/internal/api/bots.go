// bots.go — 大肥鱼机器人（AstrBot 实例）CRUD + 连通性测试。
// Web 后台创建多个机器人；对话请求经 BotResolver 现读 DB（增删即时生效）。
package api

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"time"

	"gadget-bridge/internal/ai"
)

// serveBots 挂机器人路由（Deps.SubscriptionsDB 非 nil 时—— bots 与订阅
// 共用同一个 *storage.DB）。
func serveBots(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	mux.HandleFunc("GET /api/v1/bots", func(w http.ResponseWriter, r *http.Request) {
		list, err := db.ListBots()
		if err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "list_failed", err.Error())
			return
		}
		writeJSON(w, logger, http.StatusOK, map[string]any{"bots": list})
	})

	mux.HandleFunc("POST /api/v1/bots", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			ID       string  `json:"id"`
			Label    string  `json:"label"`
			BaseURL  string  `json:"base_url"`
			Token    *string `json:"token"` // nil=保留原值（编辑留空）；""=清除；非空=新值
			ConfigID string  `json:"config_id"`
			Provider string  `json:"provider"`
			Model    string  `json:"model"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 64*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if body.Label == "" || body.BaseURL == "" {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "label 和 base_url 必填（base_url 为 AstrBot 部署地址）")
			return
		}
		id := body.ID
		if id == "" {
			id = "bot-" + randomID()
		}
		token := ""
		if body.Token != nil {
			token = *body.Token
		} else if id != "" { // 编辑未带 token：读旧值
			if _, _, t, _, _, _, err := db.GetBot(id, d.MasterKey); err == nil {
				token = t
			}
		}
		if err := db.SaveBot(id, body.Label, body.BaseURL, token,
			body.ConfigID, body.Provider, body.Model, d.MasterKey); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		// 日志只记 id/label，不记 base_url 里的凭据段与 token。
		logger.Info("bot saved", "id", id, "label", body.Label)
		writeJSON(w, logger, http.StatusOK, map[string]any{"id": id})
	})

	mux.HandleFunc("DELETE /api/v1/bots/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := r.PathValue("id")
		if err := db.DeleteBot(id); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "delete_failed", err.Error())
			return
		}
		logger.Info("bot deleted", "id", id)
		writeJSON(w, logger, http.StatusOK, map[string]bool{"deleted": true})
	})

	// 拉取 AstrBot 配置文件列表（= 人设/模型套餐；GET {base}/api/v1/chat/configs）。
	// Key 需勾选 config 权限，403 时给出明确提示。
	mux.HandleFunc("POST /api/v1/bots/configs", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			ID      string `json:"id"`
			BaseURL string `json:"base_url"`
			Token   string `json:"token"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		baseURL, token := body.BaseURL, body.Token
		if body.ID != "" {
			_, b, t, _, _, _, err := db.GetBot(body.ID, d.MasterKey)
			if err == nil {
				if baseURL == "" {
					baseURL = b
				}
				if token == "" {
					token = t
				}
			}
		}
		if baseURL == "" {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "want id 或 base_url")
			return
		}
		configs, err := ai.FetchAstrBotConfigs(r.Context(), baseURL, token)
		if err != nil {
			writeJSON(w, logger, http.StatusOK, map[string]any{
				"ok": false, "error": err.Error(),
			})
			return
		}
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true, "configs": configs})
	})

	// 连通性测试：直接对 body 里的 base_url/token 发一条 ping（未保存也可先测）。
	mux.HandleFunc("POST /api/v1/bots/probe", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			ID      string `json:"id"`       // 已保存机器人（读库取凭据）
			BaseURL string `json:"base_url"` // 或直接给地址+token（未保存先测）
			Token   string `json:"token"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		baseURL, token := body.BaseURL, body.Token
		configID, provider, model := "", "", ""
		if body.ID != "" {
			_, b, t, c, pv, m, err := db.GetBot(body.ID, d.MasterKey)
			if err != nil {
				writeErr(w, logger, http.StatusNotFound, "not_found", "robot not found")
				return
			}
			if baseURL == "" {
				baseURL = b
			}
			if token == "" {
				token = t
			}
			configID, provider, model = c, pv, m
		}
		if baseURL == "" {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "want id 或 base_url")
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
		defer cancel()
		reply, err := ai.ProbeAstrBot(ctx, body.ID, baseURL, token, configID, provider, model)
		if err != nil {
			writeJSON(w, logger, http.StatusOK, map[string]any{"ok": false, "error": err.Error()})
			return
		}
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true, "reply": reply})
	})
}
