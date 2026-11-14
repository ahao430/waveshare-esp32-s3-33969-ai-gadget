// settings.go — 应用配置 KV（ZCode 远程 / Komari 监控 / 按键映射等）。
// value 为 JSON 文本；含密钥的配置由调用方加密后再存（如 enc_token 字段），
// 明文密钥绝不进本表。
package storage

import (
	"database/sql"
	"errors"
	"fmt"
	"time"
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
	SettingKeyZCode   = "zcode"   // ZCode 远程 {base_url, enc_token}
	SettingKeyMonitor = "monitor" // Komari 监控 {base_url, enc_token}
	SettingKeyKeys    = "keys"    // 按键映射 {pwr_short, boot_short, key3_short, key3_long}
	SettingKeyWeather = "weather" // 天气 {mode: auto|manual, city}
	SettingKeyLauncher = "launcher" // 桌面排布 {order: [app key 列表]；不在列表=隐藏}
)
