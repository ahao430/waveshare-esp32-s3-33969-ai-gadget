// device_config.go — 设备运行配置下发（Web 配置 → 设备启动时拉取应用）。
//
//	GET /api/v1/device/config → {sub_refresh_interval_s}
//	PUT /api/v1/device/config   body 同形（校验 60~3600 秒 = 1~60 分钟）
//
// 设备侧 remote config 两段式（ui_remote_config_prefetch/refresh）拉取后
// 调 ds_config_set_refresh_interval，订阅页据此定时自动刷新。
package api

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"

	"gadget-bridge/internal/storage"
)

// defaultSubRefreshIntervalS 订阅自动刷新默认 15 分钟（PLAN §6.3 档位）。
const defaultSubRefreshIntervalS = 900

type deviceCfg struct {
	SubRefreshIntervalS int `json:"sub_refresh_interval_s"`
}

func serveDeviceConfig(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	readCfg := func() deviceCfg {
		c := deviceCfg{SubRefreshIntervalS: defaultSubRefreshIntervalS}
		if raw, err := db.GetSetting(storage.SettingKeyDeviceConfig); err == nil {
			var saved deviceCfg
			if json.Unmarshal([]byte(raw), &saved) == nil && saved.SubRefreshIntervalS > 0 {
				c = saved
			}
		}
		return c
	}

	mux.HandleFunc("GET /api/v1/device/config", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, logger, http.StatusOK, readCfg())
	})

	mux.HandleFunc("PUT /api/v1/device/config", func(w http.ResponseWriter, r *http.Request) {
		var body deviceCfg
		if err := json.NewDecoder(io.LimitReader(r.Body, 4*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if body.SubRefreshIntervalS < 60 || body.SubRefreshIntervalS > 3600 {
			writeErr(w, logger, http.StatusBadRequest, "bad_interval",
				"sub_refresh_interval_s 须为 60~3600（1~60 分钟）")
			return
		}
		raw, _ := json.Marshal(body)
		if err := db.SaveSetting(storage.SettingKeyDeviceConfig, string(raw)); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("device config saved", "sub_refresh_interval_s", body.SubRefreshIntervalS)
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true})
	})
}