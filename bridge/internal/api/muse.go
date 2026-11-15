// muse.go — Muse 连接配置（Web 后台）：配对凭据加密落库 + 连通性测试。
//
// 凭据来自"一次性配对"（官方固件配对后从设备 NVS 导出）：
//
//	access_token / refresh_token / node_id / noise_host / api_url
//
// 明文只进 Bridge（AES-256-GCM），响应只回 has_creds；与项目硬约束一致
// （云端密钥不进固件/日志/Git）。
package api

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"time"

	"gadget-bridge/internal/muse"
	"gadget-bridge/internal/security"
	"gadget-bridge/internal/storage"
)

// museCfg 落库结构（EncCreds=加密后的 Credentials JSON）。
type museCfg struct {
	NodeID    string `json:"node_id"`
	NoiseHost string `json:"noise_host"`
	APIURL    string `json:"api_url"`
	ProxyURL  string `json:"proxy_url"`
	EncCreds  string `json:"enc_creds,omitempty"`
	UpdatedAt int64  `json:"updated_at,omitempty"`
}

// LoadMuseCredentials 读配置并解密（Bridge 启动装配 + 每次请求现读）。
func LoadMuseCredentials(db *storage.DB, masterKey []byte) muse.Credentials {
	var c museCfg
	if raw, err := db.GetSetting(storage.SettingKeyMuse); err == nil {
		_ = json.Unmarshal([]byte(raw), &c)
	}
	creds := muse.Credentials{
		NodeID: c.NodeID, NoiseHost: c.NoiseHost, APIURL: c.APIURL, ProxyURL: c.ProxyURL,
	}
	if c.EncCreds != "" {
		if plain, err := security.Decrypt(c.EncCreds, masterKey); err == nil {
			_ = json.Unmarshal([]byte(plain), &creds)
			// 非密文字段以显式配置为准（允许只改 host/proxy）
			if c.NodeID != "" {
				creds.NodeID = c.NodeID
			}
			if c.NoiseHost != "" {
				creds.NoiseHost = c.NoiseHost
			}
			if c.APIURL != "" {
				creds.APIURL = c.APIURL
			}
			if c.ProxyURL != "" {
				creds.ProxyURL = c.ProxyURL
			}
		}
	}
	return creds
}

// serveMuseConfig 挂 Muse 配置路由。
func serveMuseConfig(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	db := d.SubscriptionsDB

	readCfg := func() museCfg {
		var c museCfg
		if raw, err := db.GetSetting(storage.SettingKeyMuse); err == nil {
			_ = json.Unmarshal([]byte(raw), &c)
		}
		return c
	}

	mux.HandleFunc("GET /api/v1/muse/config", func(w http.ResponseWriter, r *http.Request) {
		c := readCfg()
		view := map[string]any{
			"node_id":    c.NodeID,
			"noise_host": c.NoiseHost,
			"api_url":    c.APIURL,
			"proxy_url":  c.ProxyURL,
			"has_creds":  c.EncCreds != "",
		}
		if c.UpdatedAt != 0 {
			view["updated_at"] = time.Unix(c.UpdatedAt, 0)
		}
		writeJSON(w, logger, http.StatusOK, view)
	})

	// PUT 保存：creds 里至少 access_token + node_id；其余字段可选。
	mux.HandleFunc("PUT /api/v1/muse/config", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			NodeID    string `json:"node_id"`
			NoiseHost string `json:"noise_host"`
			APIURL    string `json:"api_url"`
			ProxyURL  string `json:"proxy_url"`
			Creds     *struct {
				AccessToken  string `json:"access_token"`
				RefreshToken string `json:"refresh_token"`
			} `json:"creds"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 32*1024)).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		c := readCfg()
		c.UpdatedAt = time.Now().Unix()
		if body.NodeID != "" {
			c.NodeID = body.NodeID
		}
		if body.NoiseHost != "" {
			c.NoiseHost = body.NoiseHost
		}
		if body.APIURL != "" {
			c.APIURL = body.APIURL
		}
		if body.ProxyURL != "" {
			c.ProxyURL = body.ProxyURL
		}
		if body.Creds != nil && (body.Creds.AccessToken != "" || body.Creds.RefreshToken != "") {
			creds := muse.Credentials{
				AccessToken:  body.Creds.AccessToken,
				RefreshToken: body.Creds.RefreshToken,
				NodeID:       c.NodeID,
				NoiseHost:    c.NoiseHost,
				APIURL:       c.APIURL,
				ProxyURL:     c.ProxyURL,
			}
			if creds.NodeID == "" || creds.AccessToken == "" {
				writeErr(w, logger, http.StatusBadRequest, "bad_request",
					"需要 node_id 与 access_token（配对后从设备 NVS 导出）")
				return
			}
			plain, _ := json.Marshal(creds)
			enc, err := security.Encrypt(string(plain), d.MasterKey)
			if err != nil {
				writeErr(w, logger, http.StatusInternalServerError, "encrypt_failed", err.Error())
				return
			}
			c.EncCreds = enc
		}
		raw, _ := json.Marshal(c)
		if err := db.SaveSetting(storage.SettingKeyMuse, string(raw)); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("muse config saved") // 不记 token/node_id
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true})
	})

	mux.HandleFunc("DELETE /api/v1/muse/config", func(w http.ResponseWriter, r *http.Request) {
		if err := db.DeleteSetting(storage.SettingKeyMuse); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "delete_failed", err.Error())
			return
		}
		logger.Info("muse config deleted")
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true})
	})

	// 连通性测试：fetch_vms + 一次 Noise 握手（不发消息）。
	mux.HandleFunc("POST /api/v1/muse/probe", func(w http.ResponseWriter, r *http.Request) {
		creds := LoadMuseCredentials(db, d.MasterKey)
		if !creds.Ready() {
			writeErr(w, logger, http.StatusServiceUnavailable, "not_configured",
				"未配置 Muse 凭据（POST /api/v1/muse/config）")
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 45*time.Second)
		defer cancel()
		vms, err := muse.FetchVMs(ctx, creds)
		if err != nil {
			writeJSON(w, logger, http.StatusOK, map[string]any{"ok": false, "step": "fetch_vms", "error": err.Error()})
			return
		}
		if len(vms) == 0 {
			writeJSON(w, logger, http.StatusOK, map[string]any{"ok": false, "step": "fetch_vms", "error": "账号下没有 VM"})
			return
		}
		vm := vms[0]
		for _, v := range vms {
			if v.IsDefault {
				vm = v
			}
		}
		conn, err := muse.Dial(ctx, muse.Config{
			NoiseHost: creds.NoiseHost, VMID: vm.ID, Token: vm.AuthToken,
			ProxyURL: creds.ProxyURL,
		})
		if err != nil {
			writeJSON(w, logger, http.StatusOK, map[string]any{"ok": false, "step": "noise", "vm_id": vm.ID, "error": err.Error()})
			return
		}
		conn.Close()
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true, "vm_id": vm.ID, "vms": len(vms)})
	})
}
