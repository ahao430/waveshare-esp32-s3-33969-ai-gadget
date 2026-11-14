// zcode.go — ZCode 远程会话源配置（PLAN §5.7 P1；Web 后台先行）。
// 契约（对接自建/远端 ZCode 服务）：GET {base_url}/api/v1/sessions，
// Bearer token，返回 {"sessions":[{id,agent,project,status,
// pending_confirm,completed,failed,last_active}]}（顶层数组也接受）。
// 未配置时 /sessions 返回 mock（source:"mock"，设备端演示用）。
package api

import (
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"time"

	"gadget-bridge/internal/security"
	"gadget-bridge/internal/storage"
)

// zcodeCfg 落库结构。
type zcodeCfg struct {
	BaseURL   string `json:"base_url"`
	EncToken  string `json:"enc_token,omitempty"`
	UpdatedAt int64  `json:"updated_at,omitempty"`
}

// zcodeSession 设备端展示字段（PLAN §5.7）。
type zcodeSession struct {
	ID             string    `json:"id"`
	Agent          string    `json:"agent"`
	Project        string    `json:"project"`
	Status         string    `json:"status"` // running|waiting_confirm|idle|failed
	PendingConfirm int       `json:"pending_confirm"`
	Completed      int       `json:"completed"`
	Failed         int       `json:"failed"`
	LastActive     time.Time `json:"last_active"`
}

func serveZcode(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	readCfg := func() zcodeCfg {
		var c zcodeCfg
		if raw, err := db.GetSetting(storage.SettingKeyZCode); err == nil {
			_ = json.Unmarshal([]byte(raw), &c)
		}
		return c
	}

	mux.HandleFunc("GET /api/v1/zcode/config", func(w http.ResponseWriter, r *http.Request) {
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

	mux.HandleFunc("POST /api/v1/zcode/config", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			BaseURL string `json:"base_url"`
			Token   string `json:"token"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if body.BaseURL == "" {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "base_url 必填；清空请用 DELETE")
			return
		}
		c := zcodeCfg{BaseURL: body.BaseURL, UpdatedAt: time.Now().Unix()}
		if body.Token != "" {
			enc, err := security.Encrypt(body.Token, d.MasterKey)
			if err != nil {
				writeErr(w, logger, http.StatusInternalServerError, "encrypt_failed", err.Error())
				return
			}
			c.EncToken = enc
		}
		raw, _ := json.Marshal(c)
		if err := db.SaveSetting(storage.SettingKeyZCode, string(raw)); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("zcode config saved")
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true})
	})

	mux.HandleFunc("DELETE /api/v1/zcode/config", func(w http.ResponseWriter, r *http.Request) {
		if err := db.DeleteSetting(storage.SettingKeyZCode); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "delete_failed", err.Error())
			return
		}
		logger.Info("zcode config cleared")
		writeJSON(w, logger, http.StatusOK, map[string]bool{"deleted": true})
	})

	// 会话列表：配置了远程源则透传抓取；未配置返回 mock（演示）。
	mux.HandleFunc("GET /api/v1/zcode/sessions", func(w http.ResponseWriter, r *http.Request) {
		c := readCfg()
		if c.BaseURL == "" {
			writeJSON(w, logger, http.StatusOK, map[string]any{
				"source": "mock", "fetched_at": time.Now().UTC(),
				"sessions": mockZcodeSessions(),
			})
			return
		}
		sessions, err := fetchZcodeSessions(r, c, d.MasterKey)
		if err != nil {
			writeJSON(w, logger, http.StatusOK, map[string]any{
				"source": "zcode", "error": err.Error(),
			})
			return
		}
		writeJSON(w, logger, http.StatusOK, map[string]any{
			"source": "zcode", "fetched_at": time.Now().UTC(), "sessions": sessions,
		})
	})
}

// fetchZcodeSessions 按契约抓取远端会话（错误已脱敏为短码）。
func fetchZcodeSessions(r *http.Request, c zcodeCfg, masterKey []byte) ([]zcodeSession, error) {
	token := ""
	if c.EncToken != "" {
		t, err := security.Decrypt(c.EncToken, masterKey)
		if err != nil {
			return nil, fmt.Errorf("token_decrypt_failed")
		}
		token = t
	}
	req, err := http.NewRequestWithContext(r.Context(), http.MethodGet,
		trimRightSlash(c.BaseURL)+"/api/v1/sessions", nil)
	if err != nil {
		return nil, fmt.Errorf("bad_base_url")
	}
	req.Header.Set("Accept", "application/json")
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := (&http.Client{Timeout: 15 * time.Second}).Do(req)
	if err != nil {
		return nil, fmt.Errorf("fetch_failed")
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("http_%d", resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, fmt.Errorf("read_failed")
	}
	var wrapped struct {
		Sessions []zcodeSession `json:"sessions"`
	}
	if err := json.Unmarshal(body, &wrapped); err != nil || wrapped.Sessions == nil {
		var arr []zcodeSession
		if err2 := json.Unmarshal(body, &arr); err2 != nil {
			return nil, fmt.Errorf("unsupported_response")
		}
		wrapped.Sessions = arr
	}
	return wrapped.Sessions, nil
}

func trimRightSlash(s string) string {
	for len(s) > 0 && s[len(s)-1] == '/' {
		s = s[:len(s)-1]
	}
	return s
}

// mockZcodeSessions 演示数据（设备端未配置时的占位体验）。
func mockZcodeSessions() []zcodeSession {
	now := time.Now().UTC()
	return []zcodeSession{
		{ID: "sess-7f2a", Agent: "GLM-5.3", Project: "waveshare-gadget", Status: "waiting_confirm",
			PendingConfirm: 1, Completed: 12, Failed: 0, LastActive: now.Add(-2 * time.Minute)},
		{ID: "sess-b31c", Agent: "GLM-5.3", Project: "bridge-refactor", Status: "running",
			PendingConfirm: 0, Completed: 5, Failed: 1, LastActive: now.Add(-26 * time.Minute)},
	}
}
