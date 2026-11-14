// subscriptions.go — 订阅实例配置（Web 后台可配多个，替代写死的智谱/Kimi）。
// 每条订阅 = {id, kind(适配器类型), label, base_url 覆盖, token(加密)}。
// token 明文只在 Encrypt 前 / Decrypt 后存在于内存，不进日志、不回显。
package storage

import (
	"database/sql"
	"errors"
	"fmt"
	"time"

	"gadget-bridge/internal/security"
)

// 支持的订阅适配器类型（与 quota 包 provider 一一对应）。
const (
	SubKindZhipu    = "zhipu"
	SubKindKimi     = "kimi"
	SubKindOpencode = "opencode"
	SubKindNewAPI   = "newapi"
	SubKindMock     = "mock"
	// SubKindAstrBot AstrBot 机器人实例（大肥鱼对话对接；2026-11-14）。
	SubKindAstrBot = "astrbot"
)

// ValidKind 检查 kind 是否受支持。
func ValidKind(kind string) bool {
	switch kind {
	case SubKindZhipu, SubKindKimi, SubKindOpencode, SubKindNewAPI, SubKindMock, SubKindAstrBot:
		return true
	}
	return false
}

// Subscription 一条订阅配置（列表视图无明文 token）。
type Subscription struct {
	ID        string    `json:"id"`
	Kind      string    `json:"kind"`
	Label     string    `json:"label"`
	BaseURL   string    `json:"base_url,omitempty"`
	HasToken  bool      `json:"has_token"`
	UpdatedAt time.Time `json:"updated_at"`
}

// SaveSubscription 加密后 upsert（token 空串 = 清空凭据）。
func (d *DB) SaveSubscription(id, kind, label, baseURL, token string, masterKey []byte) error {
	if masterKey == nil {
		return security.ErrNoMasterKey
	}
	enc := ""
	if token != "" {
		var err error
		enc, err = security.Encrypt(token, masterKey)
		if err != nil {
			return fmt.Errorf("storage: encrypt subscription token: %w", err)
		}
	}
	_, err := d.sql.Exec(
		`INSERT INTO subscriptions (id, kind, label, base_url, encrypted, updated_at)
		 VALUES (?, ?, ?, ?, ?, ?)
		 ON CONFLICT(id) DO UPDATE SET
		   kind=excluded.kind, label=excluded.label, base_url=excluded.base_url,
		   encrypted=excluded.encrypted, updated_at=excluded.updated_at`,
		id, kind, label, baseURL, enc, time.Now().Unix())
	if err != nil {
		return fmt.Errorf("storage: save subscription %s: %w", id, err)
	}
	return nil
}

// GetSubscription 解密读取（token 仅供 Bridge 内部抓取，不出 API）。
func (d *DB) GetSubscription(id string, masterKey []byte) (kind, label, baseURL, token string, err error) {
	if masterKey == nil {
		return "", "", "", "", security.ErrNoMasterKey
	}
	var enc string
	err = d.sql.QueryRow(
		`SELECT kind, label, base_url, encrypted FROM subscriptions WHERE id = ?`, id).
		Scan(&kind, &label, &baseURL, &enc)
	if errors.Is(err, sql.ErrNoRows) {
		return "", "", "", "", ErrNotFound
	}
	if err != nil {
		return "", "", "", "", fmt.Errorf("storage: get subscription %s: %w", id, err)
	}
	if enc == "" {
		return kind, label, baseURL, "", nil // 无凭据订阅（如 mock）：合法
	}
	token, derr := security.Decrypt(enc, masterKey)
	if derr != nil {
		return "", "", "", "", fmt.Errorf("storage: decrypt subscription token: %w", derr)
	}
	return kind, label, baseURL, token, nil
}

// DeleteSubscription 删除（不存在也成功，幂等）。
func (d *DB) DeleteSubscription(id string) error {
	_, err := d.sql.Exec(`DELETE FROM subscriptions WHERE id = ?`, id)
	if err != nil {
		return fmt.Errorf("storage: delete subscription %s: %w", id, err)
	}
	return nil
}

// ListSubscriptions 列表（无明文 token）。
func (d *DB) ListSubscriptions() ([]Subscription, error) {
	rows, err := d.sql.Query(
		`SELECT id, kind, label, base_url, encrypted, updated_at
		 FROM subscriptions ORDER BY updated_at DESC, id`)
	if err != nil {
		return nil, fmt.Errorf("storage: list subscriptions: %w", err)
	}
	defer rows.Close()
	out := []Subscription{}
	for rows.Next() {
		var s Subscription
		var enc string
		var ts int64
		if err := rows.Scan(&s.ID, &s.Kind, &s.Label, &s.BaseURL, &enc, &ts); err != nil {
			return nil, err
		}
		s.HasToken = enc != ""
		s.UpdatedAt = time.Unix(ts, 0)
		out = append(out, s)
	}
	return out, rows.Err()
}
