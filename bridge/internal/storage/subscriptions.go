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
// 机器人（AstrBot）已独立为 bots 表——订阅只管额度查看。
const (
	SubKindZhipu    = "zhipu"
	SubKindKimi     = "kimi"
	SubKindOpencode = "opencode"
	SubKindNewAPI   = "newapi"
	SubKindMock     = "mock"
	// DeepSeek 官方余额；gpt/claude/minimax/mimo = 中转站 Coding Plan
	// （internal/quota/codingplan：kimi 形状 /usages 与 newapi 余额探测）
	SubKindDeepseek = "deepseek"
	SubKindGPT      = "gpt"
	SubKindClaude   = "claude"
	SubKindMiniMax  = "minimax"
	SubKindMiMo     = "mimo"
)

// ValidKind 检查 kind 是否受支持。
func ValidKind(kind string) bool {
	switch kind {
	case SubKindZhipu, SubKindKimi, SubKindOpencode, SubKindNewAPI, SubKindMock,
		SubKindDeepseek, SubKindGPT, SubKindClaude, SubKindMiniMax, SubKindMiMo:
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
	Sort      int       `json:"-"` // 内部排序值（Web 用 ↑↓ 不消费数值）
}

// SaveSubscription 加密后 upsert（token 空串 = 清空凭据）。
// 新建实例排到末尾（max(sort)+1）；编辑不改序。
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
		`INSERT INTO subscriptions (id, kind, label, base_url, encrypted, sort, updated_at)
		 VALUES (?, ?, ?, ?, ?,
		         (SELECT COALESCE(MAX(sort), 0) + 1 FROM subscriptions), ?)
		 ON CONFLICT(id) DO UPDATE SET
		   kind=excluded.kind, label=excluded.label, base_url=excluded.base_url,
		   encrypted=excluded.encrypted, updated_at=excluded.updated_at`,
		id, kind, label, baseURL, enc, time.Now().Unix())
	if err != nil {
		return fmt.Errorf("storage: save subscription %s: %w", id, err)
	}
	return nil
}

// SetSubscriptionOrder 按传入顺序写 sort=1..n。id 集合与表现有不一致
// 时整体拒绝（防并发删除后的脏写）——由调用方先比对。
func (d *DB) SetSubscriptionOrder(ids []string) error {
	tx, err := d.sql.Begin()
	if err != nil {
		return fmt.Errorf("storage: order tx: %w", err)
	}
	defer tx.Rollback()
	for i, id := range ids {
		if _, err := tx.Exec(
			`UPDATE subscriptions SET sort = ? WHERE id = ?`, i+1, id); err != nil {
			return fmt.Errorf("storage: order %s: %w", id, err)
		}
	}
	return tx.Commit()
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

// ListSubscriptions 列表（无明文 token）。用户排序（sort）优先；
// 未排序存量（全 0）按 id 稳定兜底。
func (d *DB) ListSubscriptions() ([]Subscription, error) {
	rows, err := d.sql.Query(
		`SELECT id, kind, label, base_url, encrypted, sort, updated_at
		 FROM subscriptions ORDER BY sort, id`)
	if err != nil {
		return nil, fmt.Errorf("storage: list subscriptions: %w", err)
	}
	defer rows.Close()
	out := []Subscription{}
	for rows.Next() {
		var s Subscription
		var enc string
		var ts int64
		if err := rows.Scan(&s.ID, &s.Kind, &s.Label, &s.BaseURL, &enc, &s.Sort, &ts); err != nil {
			return nil, err
		}
		s.HasToken = enc != ""
		s.UpdatedAt = time.Unix(ts, 0)
		out = append(out, s)
	}
	return out, rows.Err()
}
