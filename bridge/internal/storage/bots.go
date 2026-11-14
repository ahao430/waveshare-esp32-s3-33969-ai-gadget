// bots.go — 大肥鱼机器人实例（AstrBot 对接，用户 2026-11-14：后台创建多个）。
// 独立于订阅（订阅=额度查看；机器人=对话上游），token 加密落库、不回显。
package storage

import (
	"database/sql"
	"errors"
	"fmt"
	"time"

	"gadget-bridge/internal/security"
)

// Bot 一条机器人配置（列表视图无明文 token）。
type Bot struct {
	ID        string    `json:"id"`
	Label     string    `json:"label"`
	BaseURL   string    `json:"base_url"`
	HasToken  bool      `json:"has_token"`
	UpdatedAt time.Time `json:"updated_at"`
}

// SaveBot 加密后 upsert（token 空串 = 清空凭据）。
func (d *DB) SaveBot(id, label, baseURL, token string, masterKey []byte) error {
	if masterKey == nil {
		return security.ErrNoMasterKey
	}
	enc := ""
	if token != "" {
		var err error
		enc, err = security.Encrypt(token, masterKey)
		if err != nil {
			return fmt.Errorf("storage: encrypt bot token: %w", err)
		}
	}
	_, err := d.sql.Exec(
		`INSERT INTO bots (id, label, base_url, encrypted, updated_at)
		 VALUES (?, ?, ?, ?, ?)
		 ON CONFLICT(id) DO UPDATE SET
		   label=excluded.label, base_url=excluded.base_url,
		   encrypted=excluded.encrypted, updated_at=excluded.updated_at`,
		id, label, baseURL, enc, time.Now().Unix())
	if err != nil {
		return fmt.Errorf("storage: save bot %s: %w", id, err)
	}
	return nil
}

// GetBot 解密读取（token 仅供 Bridge 内部调用 AstrBot，不出 API）。
func (d *DB) GetBot(id string, masterKey []byte) (label, baseURL, token string, err error) {
	if masterKey == nil {
		return "", "", "", security.ErrNoMasterKey
	}
	var enc string
	err = d.sql.QueryRow(
		`SELECT label, base_url, encrypted FROM bots WHERE id = ?`, id).
		Scan(&label, &baseURL, &enc)
	if errors.Is(err, sql.ErrNoRows) {
		return "", "", "", ErrNotFound
	}
	if err != nil {
		return "", "", "", fmt.Errorf("storage: get bot %s: %w", id, err)
	}
	if enc == "" {
		return label, baseURL, "", nil // 无凭据：合法（本地未鉴权部署）
	}
	token, derr := security.Decrypt(enc, masterKey)
	if derr != nil {
		return "", "", "", fmt.Errorf("storage: decrypt bot token: %w", derr)
	}
	return label, baseURL, token, nil
}

// DeleteBot 删除（幂等）。
func (d *DB) DeleteBot(id string) error {
	if _, err := d.sql.Exec(`DELETE FROM bots WHERE id = ?`, id); err != nil {
		return fmt.Errorf("storage: delete bot %s: %w", id, err)
	}
	return nil
}

// ListBots 列表（无明文 token），按创建顺序稳定排列（id 排序）。
func (d *DB) ListBots() ([]Bot, error) {
	rows, err := d.sql.Query(
		`SELECT id, label, base_url, encrypted, updated_at FROM bots ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("storage: list bots: %w", err)
	}
	defer rows.Close()
	out := []Bot{}
	for rows.Next() {
		var b Bot
		var enc string
		var ts int64
		if err := rows.Scan(&b.ID, &b.Label, &b.BaseURL, &enc, &ts); err != nil {
			return nil, err
		}
		b.HasToken = enc != ""
		b.UpdatedAt = time.Unix(ts, 0)
		out = append(out, b)
	}
	return out, rows.Err()
}
