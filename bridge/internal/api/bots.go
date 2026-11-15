// bots.go — 大肥鱼机器人（AstrBot）管理：公用连接 + 多实例。
//
// 结构（用户 2026-11 反馈调整）：
//   - AstrBot 地址与 Token 是**公用配置**（app_settings.astrbot，加密存）；
//   - 每个机器人实例只存 名称 + AstrBot 配置文件（人设/模型套餐），
//     可选 provider/model 覆盖；对话请求经 BotResolver 现读 DB 即时生效。
//
// 路由：
//
//	GET/PUT /api/v1/astrbot/config   公用连接（token 只写不回）
//	POST    /api/v1/astrbot/probe    用公用连接测试（可带 config_id）
//	GET     /api/v1/astrbot/configs  用公用连接拉配置文件列表
//	GET/POST /api/v1/bots            实例列表/新建或更新（label+config_id）
//	DELETE  /api/v1/bots/{id}
//	POST    /api/v1/bots/probe       测试某实例（公用连接 + 该实例 config）
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

func serveBots(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	// ---- AstrBot 公用连接 ----

	mux.HandleFunc("GET /api/v1/astrbot/config", func(w http.ResponseWriter, r *http.Request) {
		baseURL, token := db.GetAstrBotConn(d.MasterKey)
		writeJSON(w, logger, http.StatusOK, map[string]any{
			"base_url":  baseURL,
			"has_token": token != "",
		})
	})

	mux.HandleFunc("PUT /api/v1/astrbot/config", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			BaseURL string  `json:"base_url"`
			Token   *string `json:"token"` // nil=保留；""=清除；非空=更新
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if body.BaseURL == "" {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "base_url 必填")
			return
		}
		_, oldToken := db.GetAstrBotConn(d.MasterKey)
		token := oldToken
		if body.Token != nil {
			token = *body.Token
		}
		if err := db.SaveAstrBotConn(body.BaseURL, token, d.MasterKey); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("astrbot conn saved") // 不记地址/token
		writeJSON(w, logger, http.StatusOK, map[string]bool{"ok": true})
	})

	mux.HandleFunc("POST /api/v1/astrbot/probe", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			ConfigID string `json:"config_id"`
		}
		_ = json.NewDecoder(io.LimitReader(r.Body, 8*1024)).Decode(&body)
		baseURL, token := db.GetAstrBotConn(d.MasterKey)
		if baseURL == "" {
			writeJSON(w, logger, http.StatusOK, map[string]any{
				"ok": false, "error": "先在上方保存 AstrBot 地址与 Token",
			})
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
		defer cancel()
		reply, err := ai.ProbeAstrBot(ctx, "conn", baseURL, token, body.ConfigID, "", "")
		if err != nil {
			writeJSON(w, logger, http.StatusOK, map[string]any{"ok": false, "error": err.Error()})
			return
		}
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true, "reply": reply})
	})

	mux.HandleFunc("GET /api/v1/astrbot/configs", func(w http.ResponseWriter, r *http.Request) {
		baseURL, token := db.GetAstrBotConn(d.MasterKey)
		if baseURL == "" {
			writeJSON(w, logger, http.StatusOK, map[string]any{
				"ok": false, "error": "先保存 AstrBot 地址与 Token",
			})
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

	// ---- 机器人实例（名称 + 配置文件） ----

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
			ID          string `json:"id"`
			Label       string `json:"label"`
			ConfigID    string `json:"config_id"`
			Provider    string `json:"provider"`
			Model       string `json:"model"`
			TTSProvider string `json:"tts_provider"` // deepseek|edge|''=自动
			TTSVoice    string `json:"tts_voice"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if body.Label == "" {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "名称必填")
			return
		}
		id := body.ID
		if id == "" {
			id = "bot-" + randomID()
		}
		// 实例不再存地址/token（公用连接见 astrbot config）；编辑已有
		// 实例时保留旧值，避免把升级前的兼容数据清掉（曾踩：更新机器人
		// 导致公用连接回填失效、AstrBot 断连）
		legacyBase, legacyToken := "", ""
		if body.ID != "" {
			if _, b, t, _, _, _, err := db.GetBot(body.ID, d.MasterKey); err == nil {
				legacyBase, legacyToken = b, t
			}
		}
		if err := db.SaveBot(id, body.Label, legacyBase, legacyToken,
			body.ConfigID, body.Provider, body.Model,
			body.TTSProvider, body.TTSVoice, d.MasterKey); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("bot saved", "id", id, "label", body.Label, "config", body.ConfigID)
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

	// 测试某实例：公用连接 + 该实例的配置文件/provider/model
	mux.HandleFunc("POST /api/v1/bots/probe", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			ID string `json:"id"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 8*1024)).Decode(&body); err != nil || body.ID == "" {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "want id")
			return
		}
		_, cfg, prov, model, err := db.GetBotConfig(body.ID, d.MasterKey)
		if err != nil {
			writeErr(w, logger, http.StatusNotFound, "not_found", "robot not found")
			return
		}
		baseURL, token := db.GetAstrBotConn(d.MasterKey)
		if baseURL == "" {
			writeJSON(w, logger, http.StatusOK, map[string]any{
				"ok": false, "error": "先配置 AstrBot 公用地址与 Token",
			})
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
		defer cancel()
		reply, err := ai.ProbeAstrBot(ctx, body.ID, baseURL, token, cfg, prov, model)
		if err != nil {
			writeJSON(w, logger, http.StatusOK, map[string]any{"ok": false, "error": err.Error()})
			return
		}
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true, "reply": reply})
	})
}
