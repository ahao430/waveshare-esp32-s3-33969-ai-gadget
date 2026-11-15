// xiaozhi.go — 官方 xiaozhi.me 服务（Bridge 模拟设备，ADR-005）的 Web 配套端点。
//
//	GET    /api/v1/xiaozhi/status   状态（配置/绑定/最近错误；不回 token）
//	POST   /api/v1/xiaozhi/connect  跑 OTA：未绑定返回 6 位激活码
//	POST   /api/v1/xiaozhi/activate 查询绑定结果（Web 3s 轮询）
//	DELETE /api/v1/xiaozhi/config   清除身份（换设备/重来）
package api

import (
	"log/slog"
	"net/http"

	"gadget-bridge/internal/storage"
	"gadget-bridge/internal/xiaozhi"
)

// serveXiaozhi 挂载小智官方服务端点（Deps.Xiaozhi 非 nil 时）。
func serveXiaozhi(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	mux.HandleFunc("GET /api/v1/xiaozhi/status", func(w http.ResponseWriter, r *http.Request) {
		creds := d.SubscriptionsDB.GetXiaozhiCreds(d.MasterKey)
		writeJSON(w, logger, http.StatusOK, map[string]any{
			"configured":  creds.DeviceID != "",
			"bound":       creds.Bound,
			"ready":       creds.DeviceID != "" && creds.ClientID != "" && creds.WsURL != "" && creds.Bound,
			"device_id":   creds.DeviceID,
			"ota_url":     creds.OTAURL,
			"last_error":  d.Xiaozhi.LastError(),
			"ws_host":     hostOf(creds.WsURL),
		})
	})

	mux.HandleFunc("POST /api/v1/xiaozhi/connect", func(w http.ResponseWriter, r *http.Request) {
		st := d.Xiaozhi.Connect(r.Context())
		writeJSON(w, logger, http.StatusOK, st)
	})

	mux.HandleFunc("POST /api/v1/xiaozhi/activate", func(w http.ResponseWriter, r *http.Request) {
		bound := d.Xiaozhi.Activate(r.Context())
		creds := d.SubscriptionsDB.GetXiaozhiCreds(d.MasterKey)
		writeJSON(w, logger, http.StatusOK, map[string]any{
			"bound": bound && creds.Bound,
		})
	})

	mux.HandleFunc("DELETE /api/v1/xiaozhi/config", func(w http.ResponseWriter, r *http.Request) {
		if err := d.SubscriptionsDB.DeleteXiaozhiCreds(); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "delete_failed", err.Error())
			return
		}
		logger.Info("xiaozhi config deleted")
		writeJSON(w, logger, http.StatusOK, map[string]bool{"ok": true})
	})
}

// LoadXiaozhiCreds storage → xiaozhi.Credentials（装配层用）。
func LoadXiaozhiCreds(db *storage.DB, masterKey []byte) xiaozhi.Credentials {
	c := db.GetXiaozhiCreds(masterKey)
	return xiaozhi.Credentials{
		DeviceID: c.DeviceID,
		ClientID: c.ClientID,
		OTAURL:   c.OTAURL,
		WsURL:    c.WsURL,
		WsToken:  c.WsToken,
		Bound:    c.Bound,
	}
}

// SaveXiaozhiCredsToDB xiaozhi.Credentials → storage。
func SaveXiaozhiCredsToDB(db *storage.DB, masterKey []byte, c xiaozhi.Credentials) error {
	return db.SaveXiaozhiCreds(storage.XiaozhiCreds{
		DeviceID: c.DeviceID,
		ClientID: c.ClientID,
		OTAURL:   c.OTAURL,
		WsURL:    c.WsURL,
		WsToken:  c.WsToken,
		Bound:    c.Bound,
	}, masterKey)
}

// hostOf wss://host/path → host（展示用；失败返回原串）。
func hostOf(u string) string {
	for _, p := range []string{"wss://", "ws://", "https://", "http://"} {
		if len(u) > len(p) && u[:len(p)] == p {
			rest := u[len(p):]
			for i := 0; i < len(rest); i++ {
				if rest[i] == '/' || rest[i] == '?' {
					return rest[:i]
				}
			}
			return rest
		}
	}
	return u
}
