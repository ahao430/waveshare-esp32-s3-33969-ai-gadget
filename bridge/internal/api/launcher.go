// launcher.go — 桌面主页排布配置（用户 2026-11-14：后台设置应用排布）。
// 模型：order = App 排列顺序（一屏 4 个自动分页）；**固件内置 App 始终
// 显示**——排布只决定先后，不决定存在。catalog 里有、order 里没有的 key
// 追加到末尾。旧版「不在列表 = 隐藏」语义已废弃（2026-11-16 用户反馈：
// 文件 App 是内置功能，不该靠远端配置才出现）。
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
	{"weather", "天气"}, {"pomodoro", "番茄钟"}, {"files", "文件"}, {"settings", "设置"}, {"music", "音乐"}, {"reader", "小说"}, {"photo", "图片"},
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

// visibleOrder 把保存的 order 展开为完整排布：
//  1. 过滤未知/已下线 key 与重复项
//  2. catalog 中缺的 key = 内置但未排布过的 App，追加到末尾（永不隐藏）
//
// 兼容旧格式（含已废弃的 version/hidden 字段）：只读 order，忽略其余。
func visibleOrder(saved []string) []string {
	seen := make(map[string]bool, len(launcherCatalog))
	out := make([]string, 0, len(launcherCatalog))
	for _, k := range saved {
		if !validLauncherKey(k) || seen[k] {
			continue
		}
		seen[k] = true
		out = append(out, k)
	}
	for _, a := range launcherCatalog {
		if !seen[a.Key] {
			out = append(out, a.Key)
		}
	}
	return out
}

func serveLauncher(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	readOrder := func() []string {
		if raw, err := db.GetSetting(storage.SettingKeyLauncher); err == nil {
			var saved struct {
				Order []string `json:"order"`
			}
			if json.Unmarshal([]byte(raw), &saved) == nil && len(saved.Order) > 0 {
				if out := visibleOrder(saved.Order); len(out) > 0 {
					return out
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
		// 归一化保存：完整排布（未列出的内置 App 补末尾），
		// 旧版本遗留的 hidden/version 字段自然被丢弃。
		full := visibleOrder(body.Order)
		raw, _ := json.Marshal(map[string]any{"order": full})
		if err := db.SaveSetting(storage.SettingKeyLauncher, string(raw)); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("launcher layout saved", "apps", len(full))
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true, "order": full})
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
