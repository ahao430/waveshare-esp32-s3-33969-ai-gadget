// voice_apps.go — 各 App 的语音合成偏好（音色）配置。
//
// 设备语音请求带 ?app=chat|xiaozhi|muse|zcode：
//   - chat（大肥鱼/AstrBot）：用「聊天机器人」第一个实例的 TTS 设置
//   - 其他 App：用这里配置的引擎/音色；未配置时回退默认实例
//
// 路由：GET/PUT /api/v1/voice/apps
package api

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"

	"gadget-bridge/internal/storage"
)

func serveVoiceApps(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	mux.HandleFunc("GET /api/v1/voice/apps", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, logger, http.StatusOK, map[string]any{"apps": db.GetVoiceApps()})
	})

	mux.HandleFunc("PUT /api/v1/voice/apps", func(w http.ResponseWriter, r *http.Request) {
		var body map[string]storage.VoicePref
		if err := json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		// 只接受已知 App key，避免脏配置
		clean := map[string]storage.VoicePref{}
		for _, app := range []string{"chat", "xiaozhi", "muse", "zcode"} {
			if p, ok := body[app]; ok {
				clean[app] = p
			}
		}
		if err := db.SaveVoiceApps(clean); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("voice apps saved", "apps", len(clean))
		writeJSON(w, logger, http.StatusOK, map[string]bool{"ok": true})
	})
}
