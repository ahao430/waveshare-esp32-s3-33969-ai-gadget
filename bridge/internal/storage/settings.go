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
	SettingKeyZCode    = "zcode"    // ZCode 远程 {base_url, enc_token}
	SettingKeyMonitor  = "monitor"  // Komari 监控 {base_url, enc_token}
	SettingKeyKeys     = "keys"     // 按键映射 {pwr_short, boot_short, key3_short, key3_long}
	SettingKeyWeather  = "weather"  // 天气 {mode: auto|manual, city}
	SettingKeyLauncher = "launcher" // 桌面排布 {order: [app key 列表]；不在列表=隐藏}
	SettingKeyAstrBot  = "astrbot"  // AstrBot 公用连接 {base_url, enc_token}（机器人实例共用）
)

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
