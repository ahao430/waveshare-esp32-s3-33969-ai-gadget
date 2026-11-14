// weather.go — 天气数据 + 定位配置 API（Open-Meteo 免费无 Key）。
// 设备：GET /api/v1/weather（Bridge 缓存，命中即回）。
// Web：GET/POST /api/v1/weather/config（auto=IP 定位 / manual=手动城市）。
package api

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"

	"gadget-bridge/internal/storage"
)

// weatherCfg 落库结构。
type weatherCfg struct {
	Mode string `json:"mode"` // auto|manual
	City string `json:"city,omitempty"`
}

func serveWeather(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	readCfg := func() weatherCfg {
		c := weatherCfg{Mode: "auto"} // 默认自动 IP 定位
		if raw, err := db.GetSetting(storage.SettingKeyWeather); err == nil {
			var saved weatherCfg
			if json.Unmarshal([]byte(raw), &saved) == nil && (saved.Mode == "auto" || saved.Mode == "manual") {
				c = saved
			}
		}
		if c.Mode != "manual" && c.Mode != "auto" {
			c.Mode = "auto"
		}
		return c
	}

	mux.HandleFunc("GET /api/v1/weather/config", func(w http.ResponseWriter, r *http.Request) {
		c := readCfg()
		view := map[string]any{
			"mode": c.Mode,
			"city": c.City,
		}
		if d.Weather != nil { // 当前实际定位（缓存）供预览
			if loc := d.Weather.Location(); loc != "" {
				view["location"] = loc
			}
		}
		writeJSON(w, logger, http.StatusOK, view)
	})

	mux.HandleFunc("POST /api/v1/weather/config", func(w http.ResponseWriter, r *http.Request) {
		var body weatherCfg
		if err := json.NewDecoder(io.LimitReader(r.Body, 8*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if body.Mode != "auto" && body.Mode != "manual" {
			writeErr(w, logger, http.StatusBadRequest, "bad_mode", "mode 必须为 auto 或 manual")
			return
		}
		if body.Mode == "manual" && body.City == "" {
			writeErr(w, logger, http.StatusBadRequest, "bad_city", "manual 模式需要 city")
			return
		}
		raw, _ := json.Marshal(body)
		if err := db.SaveSetting(storage.SettingKeyWeather, string(raw)); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		// 保存后立即按新配置刷新（配置页预览即生效）
		if d.Weather != nil {
			go d.Weather.Refresh(r.Context())
		}
		logger.Info("weather config saved", "mode", body.Mode)
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true})
	})

	if d.Weather != nil {
		mux.HandleFunc("GET /api/v1/weather", func(w http.ResponseWriter, r *http.Request) {
			view := d.Weather.Get(r.Context(), r.URL.Query().Get("refresh") == "1")
			writeJSON(w, logger, http.StatusOK, view)
		})
	}
}
