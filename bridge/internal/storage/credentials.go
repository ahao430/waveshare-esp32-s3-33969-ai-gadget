// credentials.go — Provider 凭据 CRUD（加密落库；PLAN §15.3）。
// 明文 Key 只在 Encrypt 前 / Decrypt 后存在于内存，不进日志、不回显 API。
package storage

import (
	"database/sql"
	"errors"
	"fmt"
	"time"

	"gadget-bridge/internal/security"
)

// ErrNotFound 凭据不存在。
var ErrNotFound = errors.New("credential_not_found")

// CredentialInfo 是列表视图（无明文 Key）。
type CredentialInfo struct {
	ProviderID string    `json:"provider_id"`
	Label      string    `json:"account_label"`
	UpdatedAt  time.Time `json:"updated_at"`
}

// SaveCredential 加密后 upsert。
// masterKey 为 nil（未配置 BRIDGE_MASTER_KEY）时返回 ErrNoMasterKey ——
// 凭据持久化整体禁用，fail-safe。
func (d *DB) SaveCredential(providerID, label, apiKey string, masterKey []byte) error {
	if masterKey == nil {
		return security.ErrNoMasterKey
	}
	enc, err := security.Encrypt(apiKey, masterKey)
	if err != nil {
		return fmt.Errorf("storage: encrypt: %w", err)
	}
	_, err = d.sql.Exec(
		`INSERT INTO provider_credentials (provider_id, account_label, encrypted, updated_at)
		 VALUES (?, ?, ?, ?)
		 ON CONFLICT(provider_id) DO UPDATE SET
		   account_label=excluded.account_label, encrypted=excluded.encrypted,
		   updated_at=excluded.updated_at`,
		providerID, label, enc, time.Now().Unix())
	if err != nil {
		return fmt.Errorf("storage: save credential %s: %w", providerID, err)
	}
	return nil
}

// GetCredential 解密读取；ErrNotFound 表示未配置。
func (d *DB) GetCredential(providerID string, masterKey []byte) (label, apiKey string, err error) {
	if masterKey == nil {
		return "", "", security.ErrNoMasterKey
	}
	var enc string
	err = d.sql.QueryRow(
		`SELECT account_label, encrypted FROM provider_credentials WHERE provider_id = ?`,
		providerID).Scan(&label, &enc)
	if errors.Is(err, sql.ErrNoRows) {
		return "", "", ErrNotFound
	}
	if err != nil {
		return "", "", fmt.Errorf("storage: get credential: %w", err)
	}
	plain, err := security.Decrypt(enc, masterKey)
	if err != nil {
		return "", "", fmt.Errorf("storage: decrypt credential %s: %w", providerID, err)
	}
	return label, plain, nil
}

// DeleteCredential 删除（不存在也返回成功，幂等）。
func (d *DB) DeleteCredential(providerID string) error {
	_, err := d.sql.Exec(`DELETE FROM provider_credentials WHERE provider_id = ?`, providerID)
	if err != nil {
		return fmt.Errorf("storage: delete credential: %w", err)
	}
	return nil
}

// ListCredentials 列表（无明文）。
func (d *DB) ListCredentials() ([]CredentialInfo, error) {
	rows, err := d.sql.Query(
		`SELECT provider_id, account_label, updated_at FROM provider_credentials ORDER BY provider_id`)
	if err != nil {
		return nil, fmt.Errorf("storage: list credentials: %w", err)
	}
	defer rows.Close()
	var out []CredentialInfo
	for rows.Next() {
		var c CredentialInfo
		var ts int64
		if err := rows.Scan(&c.ProviderID, &c.Label, &ts); err != nil {
			return nil, err
		}
		c.UpdatedAt = time.Unix(ts, 0)
		out = append(out, c)
	}
	return out, rows.Err()
}
