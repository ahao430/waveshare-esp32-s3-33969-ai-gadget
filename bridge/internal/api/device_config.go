// device_config.go — 设备运行配置（Web 配置 + 设备回写，双向一份存储）。
//
//	GET /api/v1/device/config → {sub_refresh_interval_s, brightness?, volume?}
//	PUT /api/v1/device/config   部分更新（缺省字段保留原值）：
//	  sub_refresh_interval_s 60~3600（1~60 分钟）
//	  brightness 10~100（AMOLED 下限 10）
//	  volume 0~100
//
// 设备侧 remote config 两段式（ui_remote_config_prefetch/refresh）拉取后
// 应用；状态栏快捷面板/设置页改动经 ds_config_persist 防抖回写
// （brightness/volume 跨重启持久化，2026-11-16）。
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

func deref(p *int) int {
	if p == nil {
		return 0
	}
	return *p
}

type deviceCfg struct {
	SubRefreshIntervalS int `json:"sub_refresh_interval_s"`
	// 指针 + omitempty：nil=未设置（GET 省略）；volume 0=静音是合法
	// 已存值，必须能区分"没存过"与 0（int+omitempty 会把 0 吞掉）。
	Brightness *int `json:"brightness,omitempty"`
	Volume     *int `json:"volume,omitempty"`
}

// PUT 部分更新 DTO：指针区分"未提供"与 0。
type deviceCfgPut struct {
	SubRefreshIntervalS *int `json:"sub_refresh_interval_s"`
	Brightness          *int `json:"brightness"`
	Volume              *int `json:"volume"`
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
		var body deviceCfgPut
		if err := json.NewDecoder(io.LimitReader(r.Body, 4*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if body.SubRefreshIntervalS == nil && body.Brightness == nil && body.Volume == nil {
			writeErr(w, logger, http.StatusBadRequest, "empty_body",
				"至少提供 sub_refresh_interval_s / brightness / volume 之一")
			return
		}
		if body.SubRefreshIntervalS != nil &&
			(*body.SubRefreshIntervalS < 60 || *body.SubRefreshIntervalS > 3600) {
			writeErr(w, logger, http.StatusBadRequest, "bad_interval",
				"sub_refresh_interval_s 须为 60~3600（1~60 分钟）")
			return
		}
		if body.Brightness != nil && (*body.Brightness < 10 || *body.Brightness > 100) {
			writeErr(w, logger, http.StatusBadRequest, "bad_brightness",
				"brightness 须为 10~100")
			return
		}
		if body.Volume != nil && (*body.Volume < 0 || *body.Volume > 100) {
			writeErr(w, logger, http.StatusBadRequest, "bad_volume",
				"volume 须为 0~100")
			return
		}
		/* 合并语义：未提供的字段保留已存值（设备回写只带亮度/音量时
		 * 不能清掉 Web 配的订阅间隔，反之亦然） */
		cfg := readCfg()
		if body.SubRefreshIntervalS != nil {
			cfg.SubRefreshIntervalS = *body.SubRefreshIntervalS
		}
		if body.Brightness != nil {
			cfg.Brightness = body.Brightness
		}
		if body.Volume != nil {
			cfg.Volume = body.Volume
		}
		raw, _ := json.Marshal(cfg)
		if err := db.SaveSetting(storage.SettingKeyDeviceConfig, string(raw)); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("device config saved", "sub_refresh_interval_s", cfg.SubRefreshIntervalS,
			"brightness", deref(cfg.Brightness), "volume", deref(cfg.Volume))
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true})
	})
}