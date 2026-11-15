// settings.go — 应用配置 KV（ZCode 远程 / Komari 监控 / 按键映射等）。
// value 为 JSON 文本；含密钥的配置由调用方加密后再存（如 enc_token 字段），
// 明文密钥绝不进本表。
package storage

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"gadget-bridge/internal/security"
)

// SaveSetting 覆盖写一条配置（value 为 JSON 文本）。
func (d *DB) SaveSetting(key, value string) error {
	if _, err := d.sql.Exec(
		`INSERT INTO app_settings (key, value, updated_at) VALUES (?, ?, ?)
		 ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at`,
		key, value, time.Now().Unix()); err != nil {
		return fmt.Errorf("storage: save setting %s: %w", key, err)
	}
	return nil
}

// GetSetting 读取配置原文；不存在返回 ErrNotFound。
func (d *DB) GetSetting(key string) (string, error) {
	var value string
	err := d.sql.QueryRow(`SELECT value FROM app_settings WHERE key = ?`, key).Scan(&value)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrNotFound
	}
	if err != nil {
		return "", fmt.Errorf("storage: get setting %s: %w", key, err)
	}
	return value, nil
}

// DeleteSetting 删除配置（幂等）。
func (d *DB) DeleteSetting(key string) error {
	if _, err := d.sql.Exec(`DELETE FROM app_settings WHERE key = ?`, key); err != nil {
		return fmt.Errorf("storage: delete setting %s: %w", key, err)
	}
	return nil
}

// 常用配置键。
const (
	SettingKeyZCode     = "zcode"      // ZCode 远程 {base_url, enc_token}
	SettingKeyMonitor   = "monitor"    // Komari 监控 {base_url, enc_token}
	SettingKeyKeys      = "keys"       // 按键映射 {pwr_short, boot_short, key3_short, key3_long}
	SettingKeyWeather   = "weather"    // 天气 {mode: auto|manual, city}
	SettingKeyLauncher  = "launcher"   // 桌面排布 {order: [app key 列表]；不在列表=隐藏}
	SettingKeyAstrBot   = "astrbot"    // AstrBot 公用连接 {base_url, enc_token}（机器人实例共用）
	SettingKeyVoiceApps = "voice_apps" // 各 App 语音偏好 {"muse":{"provider","voice"},"xiaozhi":{...}}
	SettingKeyMuse      = "muse"       // Muse 连接 {node_id, noise_host, api_url, proxy_url, enc_creds}
	// chat_active_bot：AstrBot 对话 App 的"当前机器人"实例 id（多实例
	// 切换，设备端点标题切换/Web 设为默认都写这里；空/失效回落第一个）
	SettingKeyChatActiveBot = "chat_active_bot"
)

// SaveChatActiveBot 保存当前机器人实例 id（空串=清除，回落第一个）。
func (d *DB) SaveChatActiveBot(id string) error {
	if id == "" {
		return d.DeleteSetting(SettingKeyChatActiveBot)
	}
	return d.SaveSetting(SettingKeyChatActiveBot, id)
}

// SavedChatActiveBot 读保存的 id（未保存返回空串，不解析回退）。
func (d *DB) SavedChatActiveBot() string {
	id, _ := d.GetSetting(SettingKeyChatActiveBot)
	return id
}

// SaveAstrBotConn 保存 AstrBot 公用连接（token 加密；空串=清除）。
func (d *DB) SaveAstrBotConn(baseURL, token string, masterKey []byte) error {
	enc := ""
	if token != "" {
		if masterKey == nil {
			return security.ErrNoMasterKey
		}
		var err error
		enc, err = security.Encrypt(token, masterKey)
		if err != nil {
			return fmt.Errorf("storage: encrypt astrbot token: %w", err)
		}
	}
	raw, _ := json.Marshal(map[string]string{"base_url": baseURL, "enc_token": enc})
	return d.SaveSetting(SettingKeyAstrBot, string(raw))
}

// GetAstrBotConn 读取公用连接；未配置时兼容旧版（取 bots 表第一条的
// 地址/token，让升级前创建的机器人继续可用）。
func (d *DB) GetAstrBotConn(masterKey []byte) (baseURL, token string) {
	if raw, err := d.GetSetting(SettingKeyAstrBot); err == nil && raw != "" {
		var cfg struct {
			BaseURL  string `json:"base_url"`
			EncToken string `json:"enc_token"`
		}
		if json.Unmarshal([]byte(raw), &cfg) == nil && cfg.BaseURL != "" {
			if cfg.EncToken != "" && masterKey != nil {
				if t, derr := security.Decrypt(cfg.EncToken, masterKey); derr == nil {
					token = t
				}
			}
			return cfg.BaseURL, token
		}
	}
	if list, err := d.ListBots(); err == nil && len(list) > 0 {
		if _, b, t, _, _, _, err := d.GetBot(list[0].ID, masterKey); err == nil {
			return b, t
		}
	}
	return "", ""
}

// VoicePref 单个 App 的语音合成偏好（provider 空=自动）。
type VoicePref struct {
	Provider string `json:"provider,omitempty"` // deepseek|edge|""
	Voice    string `json:"voice,omitempty"`    // 音色 id
}

// GetVoiceApps 读取 App 级语音偏好（未配置返回空 map）。
func (d *DB) GetVoiceApps() map[string]VoicePref {
	out := map[string]VoicePref{}
	raw, err := d.GetSetting(SettingKeyVoiceApps)
	if err != nil || raw == "" {
		return out
	}
	_ = json.Unmarshal([]byte(raw), &out)
	return out
}

// SaveVoiceApps 覆盖写 App 级语音偏好。
func (d *DB) SaveVoiceApps(m map[string]VoicePref) error {
	raw, err := json.Marshal(m)
	if err != nil {
		return err
	}
	return d.SaveSetting(SettingKeyVoiceApps, string(raw))
}
