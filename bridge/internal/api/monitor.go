// monitor.go — Komari 服务器监控源配置（Web 后台；此前只能 KOMARI_URL 环境变量）。
// 配置存 app_settings(monitor)；token 加密。抓取经 komari.NewDynamicSource
// 每次现读配置 → Web 保存后下一次刷新（或 ?refresh=1）即生效。
package api

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"time"

	"gadget-bridge/internal/security"
	"gadget-bridge/internal/storage"
)

// monitorCfg 落库结构（EncToken=security.Encrypt 密文，明文不落库）。
type monitorCfg struct {
	BaseURL   string `json:"base_url"`
	EncToken  string `json:"enc_token,omitempty"`
	UpdatedAt int64  `json:"updated_at,omitempty"`
}

// serveMonitorConfig 挂监控配置路由（SubscriptionsDB 即共享 storage.DB）。
func serveMonitorConfig(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	readCfg := func() monitorCfg { // 无配置 = 空值（未配置）
		var c monitorCfg
		if raw, err := db.GetSetting(storage.SettingKeyMonitor); err == nil {
			_ = json.Unmarshal([]byte(raw), &c)
		}
		return c
	}

	mux.HandleFunc("GET /api/v1/monitor/config", func(w http.ResponseWriter, r *http.Request) {
		c := readCfg()
		view := map[string]any{
			"base_url":  c.BaseURL,
			"has_token": c.EncToken != "",
		}
		if c.UpdatedAt != 0 { // 未配置过不带 updated_at（Web 端不显示 1970）
			view["updated_at"] = time.Unix(c.UpdatedAt, 0)
		}
		writeJSON(w, logger, http.StatusOK, view)
	})

	mux.HandleFunc("POST /api/v1/monitor/config", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			BaseURL string `json:"base_url"`
			Token   string `json:"token"` // 空=无鉴权（或未开鉴权的部署）
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if body.BaseURL == "" {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "base_url 必填（Komari 面板地址）；清空请用 DELETE")
			return
		}
		c := monitorCfg{BaseURL: body.BaseURL, UpdatedAt: time.Now().Unix()}
		if body.Token != "" {
			enc, err := security.Encrypt(body.Token, d.MasterKey)
			if err != nil {
				writeErr(w, logger, http.StatusInternalServerError, "encrypt_failed", err.Error())
				return
			}
			c.EncToken = enc
		}
		raw, _ := json.Marshal(c)
		if err := db.SaveSetting(storage.SettingKeyMonitor, string(raw)); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("monitor config saved") // 不记 URL/token
		// 数据源变了：后台立刻重新抓（force），设备下一次刷新就有新数据
		if d.Komari != nil {
			go d.Komari.Nodes(context.Background(), true)
		}
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true})
	})

	mux.HandleFunc("DELETE /api/v1/monitor/config", func(w http.ResponseWriter, r *http.Request) {
		if err := db.DeleteSetting(storage.SettingKeyMonitor); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "delete_failed", err.Error())
			return
		}
		logger.Info("monitor config cleared")
		writeJSON(w, logger, http.StatusOK, map[string]bool{"deleted": true})
	})
}
