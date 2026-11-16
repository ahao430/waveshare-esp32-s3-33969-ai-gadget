// launcher.go — 桌面主页排布配置（用户 2026-11-14：后台设置应用排布）。
// 模型：order = 可见 App 的 key 有序列表（一屏 4 个自动分页）；
// 不在列表里的 App = 隐藏（设备桌面不显示，后台可恢复）。
package api

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"

	"gadget-bridge/internal/storage"
)

// launcherCatalog 设备 App 目录（key 与固件 app_def_t.key 一一对应）。
// 顺序 = 出厂默认排布。
var launcherCatalog = []struct {
	Key  string `json:"key"`
	Name string `json:"name"`
}{
	{"chat", "AstrBot"}, {"xiaozhi", "小智"}, {"muse", "Muse"}, {"sub", "订阅"},
	{"weather", "天气"}, {"files", "文件"}, {"settings", "设置"}, {"music", "音乐"},
}

func defaultLauncherOrder() []string {
	out := make([]string, 0, len(launcherCatalog))
	for _, a := range launcherCatalog {
		out = append(out, a.Key)
	}
	return out
}

func validLauncherKey(k string) bool {
	for _, a := range launcherCatalog {
		if a.Key == k {
			return true
		}
	}
	return false
}

func serveLauncher(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	readOrder := func() []string {
		if raw, err := db.GetSetting(storage.SettingKeyLauncher); err == nil {
			var saved struct {
				Order []string `json:"order"`
			}
			if json.Unmarshal([]byte(raw), &saved) == nil && len(saved.Order) > 0 {
				// 过滤已下线/未知 key（2026-11-16 删 zcode/monitor 后：
				// 旧排布仍按过滤结果生效，不整单回落出厂重置用户自定义）。
				filtered := make([]string, 0, len(saved.Order))
				seen := map[string]bool{}
				for _, k := range saved.Order {
					if !validLauncherKey(k) || seen[k] {
						continue
					}
					seen[k] = true
					filtered = append(filtered, k)
				}
				if len(filtered) > 0 {
					return filtered
				}
			}
		}
		return defaultLauncherOrder() // 未配置/损坏回落出厂排布
	}

	mux.HandleFunc("GET /api/v1/launcher", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, logger, http.StatusOK, map[string]any{
			"order":     readOrder(),
			"catalog":   launcherCatalog,
			"page_size": 4, // 一屏 4 个（原厂范式）
		})
	})

	mux.HandleFunc("POST /api/v1/launcher", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Order []string `json:"order"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 8*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if err := launcherOrderValid(body.Order); err != "" {
			writeErr(w, logger, http.StatusBadRequest, "bad_order", err)
			return
		}
		raw, _ := json.Marshal(map[string]any{"order": body.Order})
		if err := db.SaveSetting(storage.SettingKeyLauncher, string(raw)); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("launcher layout saved", "apps", len(body.Order))
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true, "order": body.Order})
	})
}

// launcherOrderValid 返回空串=合法；否则错误信息。
func launcherOrderValid(order []string) string {
	if len(order) == 0 {
		return "order 不能为空（至少保留一个应用）"
	}
	seen := map[string]bool{}
	for _, k := range order {
		if !validLauncherKey(k) {
			return "未知应用 key：" + k
		}
		if seen[k] {
			return "重复应用 key：" + k
		}
		seen[k] = true
	}
	return ""
}
