// keys.go — 三物理键短按/长按映射配置（POWER_MANAGEMENT §13.4）。
// 可配置：BOOT 短按、KEY3 短按+长按。
// 固定不可改（2026-11-17 用户拍板）：PWR 短按=息屏/点亮、PWR 长按=关机
// （PMU 硬件）、BOOT 长按=配网页。PWRShort 字段仅为 API 兼容保留，
// POST 一律忽略、GET 恒返回存储值（设备/模拟器均不消费）。
// 设备启动时 GET /api/v1/keys 拉取（与主题同机制：Web 下发优先）。
package api

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"

	"gadget-bridge/internal/storage"
)

// Keymap 设备端消费的映射（值域见 keyActions）。
type Keymap struct {
	PWRShort  string `json:"pwr_short"`
	BOOTShort string `json:"boot_short"`
	KEY3Short string `json:"key3_short"`
	KEY3Long  string `json:"key3_long"`
}

// defaultKeymap 与设备出厂行为一致（POWER_MANAGEMENT §13.4 默认映射）。
var defaultKeymap = Keymap{PWRShort: "home", BOOTShort: "back", KEY3Short: "app", KEY3Long: "ptt"}

// keyActions 每个槽位的合法动作（pwr_short 已锁定为固件固定行为，不在表内）。
var keyActions = map[string][]string{
	"boot_short": {"back", "home", "none"},
	"key3_short": {"app", "back", "home", "none"},
	"key3_long":  {"ptt", "none"},
}

// actionNames 中文标签（Web 下拉框）。
var actionNames = map[string]string{
	"home": "回到首页", "back": "返回（上滑同效）", "none": "无操作",
	"app": "交给当前应用", "ptt": "语音说话（按住说、松手发）",
}

func contains(xs []string, v string) bool {
	for _, x := range xs {
		if x == v {
			return true
		}
	}
	return false
}

func serveKeys(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	readKeymap := func() Keymap {
		km := defaultKeymap
		if raw, err := db.GetSetting(storage.SettingKeyKeys); err == nil {
			var saved Keymap
			if json.Unmarshal([]byte(raw), &saved) == nil {
				if saved.PWRShort != "" {
					km.PWRShort = saved.PWRShort
				}
				if saved.BOOTShort != "" {
					km.BOOTShort = saved.BOOTShort
				}
				if saved.KEY3Short != "" {
					km.KEY3Short = saved.KEY3Short
				}
				if saved.KEY3Long != "" {
					km.KEY3Long = saved.KEY3Long
				}
			}
		}
		return km
	}

	mux.HandleFunc("GET /api/v1/keys", func(w http.ResponseWriter, r *http.Request) {
		km := readKeymap()
		writeJSON(w, logger, http.StatusOK, map[string]any{
			"keymap": km,
			// 固定项：设备端不消费，供 Web 展示"不可修改"。
			"fixed": map[string]string{
				"pwr_short": "息屏/点亮（固定，不可修改）",
				"pwr_long":  "关机（PMU 硬件行为，不可修改）",
				"boot_long": "打开配网页（固定，不可修改）",
			},
			"actions": keyActions,
			"labels":  actionNames,
		})
	})

	mux.HandleFunc("POST /api/v1/keys", func(w http.ResponseWriter, r *http.Request) {
		var body Keymap
		if err := json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		for slot, allowed := range keyActions {
			v := ""
			switch slot {
			case "boot_short":
				v = body.BOOTShort
			case "key3_short":
				v = body.KEY3Short
			case "key3_long":
				v = body.KEY3Long
			}
			if v == "" {
				v = defaultKeymap.slotDefault(slot)
			}
			if !contains(allowed, v) {
				writeErr(w, logger, http.StatusBadRequest, "bad_action",
					slot+" 不支持动作 "+v+"（可选："+joinNames(allowed)+"）")
				return
			}
		}
		km := Keymap{ // 空槽回落默认，存完整值
			PWRShort:  readKeymap().PWRShort, // 已锁定：忽略 body，恒保留存储值
			BOOTShort: orDefault(body.BOOTShort, defaultKeymap.BOOTShort),
			KEY3Short: orDefault(body.KEY3Short, defaultKeymap.KEY3Short),
			KEY3Long:  orDefault(body.KEY3Long, defaultKeymap.KEY3Long),
		}
		raw, _ := json.Marshal(km)
		if err := db.SaveSetting(storage.SettingKeyKeys, string(raw)); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("keymap saved",
			"pwr", km.PWRShort, "boot", km.BOOTShort, "key3s", km.KEY3Short, "key3l", km.KEY3Long)
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true, "keymap": km})
	})
}

func (k Keymap) slotDefault(slot string) string {
	switch slot {
	case "boot_short":
		return k.BOOTShort
	case "key3_short":
		return k.KEY3Short
	case "key3_long":
		return k.KEY3Long
	default:
		return k.PWRShort
	}
}

func orDefault(v, def string) string {
	if v == "" {
		return def
	}
	return v
}

func joinNames(vs []string) string {
	out := ""
	for i, v := range vs {
		if i > 0 {
			out += " / "
		}
		out += actionNames[v]
	}
	return out
}
