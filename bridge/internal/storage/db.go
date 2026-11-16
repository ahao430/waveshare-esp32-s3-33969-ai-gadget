// Package storage 提供额度快照的 SQLite 持久化（PLAN §15.3）。
// 纯 Go 驱动（modernc.org/sqlite），无 cgo，便于交叉编译。
// Phase 3 范围：快照缓存持久化；凭据加密存储仍为 TODO。
package storage

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"

	_ "modernc.org/sqlite" // 注册 sqlite 驱动

	"gadget-bridge/internal/quota"
)

const schema = `
CREATE TABLE IF NOT EXISTS snapshot_cache (
    provider_id  TEXT PRIMARY KEY,
    payload      TEXT NOT NULL,            -- StoredSnapshot JSON
    updated_at   INTEGER NOT NULL          -- 写入时间（unix 秒）
);
CREATE TABLE IF NOT EXISTS provider_credentials (
    provider_id  TEXT PRIMARY KEY,
    account_label TEXT NOT NULL DEFAULT '',
    encrypted    TEXT NOT NULL,            -- security.Encrypt 格式
    updated_at   INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS subscriptions (
    id          TEXT PRIMARY KEY,          -- 订阅实例 id（user-facing）
    kind        TEXT NOT NULL,             -- zhipu|kimi|opencode|newapi|mock
    label       TEXT NOT NULL,
    base_url    TEXT NOT NULL DEFAULT '',  -- 覆盖该 kind 的默认端点
    encrypted   TEXT NOT NULL DEFAULT '',  -- token（security.Encrypt；空=无凭据）
    sort        INTEGER NOT NULL DEFAULT 0,-- 用户排序（0=未排，按 id 兜底）
    updated_at  INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS bots (          -- 大肥鱼机器人（AstrBot 实例）
    id          TEXT PRIMARY KEY,          -- bot-xxxx
    label       TEXT NOT NULL,
    base_url    TEXT NOT NULL,             -- AstrBot 部署地址
    encrypted   TEXT NOT NULL DEFAULT '',  -- abk_ token（加密；空=无鉴权）
    config_id   TEXT NOT NULL DEFAULT '',  -- AstrBot 配置文件（人设/模型套餐）
    provider    TEXT NOT NULL DEFAULT '',  -- 覆盖 provider id（可选）
    model       TEXT NOT NULL DEFAULT '',  -- 覆盖模型名（可选）
    updated_at  INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS app_settings (  -- 应用配置 KV（keys/外观/launcher 等）
    key         TEXT PRIMARY KEY,
    value       TEXT NOT NULL,             -- JSON；密钥字段加密后存
    updated_at  INTEGER NOT NULL
);
`

// DB 是快照持久层。
type DB struct {
	sql *sql.DB
}

// Open 打开（必要时创建）数据库并执行迁移。path 为空时用内存库（仅测试）。
func Open(path string) (*DB, error) {
	dsn := "file::memory:?cache=shared"
	if path != "" {
		if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
			return nil, fmt.Errorf("storage: mkdir: %w", err)
		}
		dsn = "file:" + path + "?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)"
	}
	sqlDB, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("storage: open: %w", err)
	}
	// SQLite 单写者：串行化避免 database-locked
	sqlDB.SetMaxOpenConns(1)
	if _, err := sqlDB.Exec(schema); err != nil {
		sqlDB.Close()
		return nil, fmt.Errorf("storage: migrate: %w", err)
	}
	if err := migrateAstrBotToBots(sqlDB); err != nil {
		sqlDB.Close()
		return nil, fmt.Errorf("storage: migrate astrbot->bots: %w", err)
	}
	ensureColumn(sqlDB, "bots", "config_id", "TEXT NOT NULL DEFAULT ''")
	ensureColumn(sqlDB, "bots", "provider", "TEXT NOT NULL DEFAULT ''")
	ensureColumn(sqlDB, "bots", "model", "TEXT NOT NULL DEFAULT ''")
	ensureColumn(sqlDB, "bots", "tts_provider", "TEXT NOT NULL DEFAULT ''") // deepseek|edge|''=自动
	ensureColumn(sqlDB, "bots", "tts_voice", "TEXT NOT NULL DEFAULT ''")    // 音色 id/名字
	ensureColumn(sqlDB, "subscriptions", "sort", "INTEGER NOT NULL DEFAULT 0") // 用户排序（2026-11-16）
	return &DB{sql: sqlDB}, nil
}

func (d *DB) Close() error { return d.sql.Close() }

// SaveStored 持久化一条 StoredSnapshot（整体 JSON 覆盖写）。
func (d *DB) SaveStored(providerID string, view quota.StoredSnapshot) error {
	payload, err := json.Marshal(view)
	if err != nil {
		return fmt.Errorf("storage: marshal: %w", err)
	}
	_, err = d.sql.Exec(
		`INSERT INTO snapshot_cache (provider_id, payload, updated_at) VALUES (?, ?, ?)
		 ON CONFLICT(provider_id) DO UPDATE SET payload=excluded.payload, updated_at=excluded.updated_at`,
		providerID, string(payload), time.Now().Unix(),
	)
	if err != nil {
		return fmt.Errorf("storage: save %s: %w", providerID, err)
	}
	return nil
}

// migrateAstrBotToBots 一次性迁移：早期版本机器人混在 subscriptions
// （kind='astrbot'）里；机器人独立成 bots 表后，启动时搬过去并删旧行。
// 幂等：无 astrbot 订阅时是空操作。
func migrateAstrBotToBots(sqlDB *sql.DB) error {
	rows, err := sqlDB.Query(
		`SELECT id, label, base_url, encrypted, updated_at FROM subscriptions WHERE kind = 'astrbot'`)
	if err != nil {
		return err
	}
	type row struct {
		id, label, baseURL, enc string
		ts                      int64
	}
	var todo []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.id, &r.label, &r.baseURL, &r.enc, &r.ts); err != nil {
			rows.Close()
			return err
		}
		todo = append(todo, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	for _, r := range todo {
		if _, err := sqlDB.Exec(
			`INSERT INTO bots (id, label, base_url, encrypted, updated_at) VALUES (?, ?, ?, ?, ?)
			 ON CONFLICT(id) DO UPDATE SET label=excluded.label, base_url=excluded.base_url,
			   encrypted=excluded.encrypted, updated_at=excluded.updated_at`,
			r.id, r.label, r.baseURL, r.enc, r.ts); err != nil {
			return err
		}
		if _, err := sqlDB.Exec(`DELETE FROM subscriptions WHERE id = ?`, r.id); err != nil {
			return err
		}
	}
	return nil
}

// ensureColumn 旧库补列（列已存在时报错忽略；幂等）。
func ensureColumn(sqlDB *sql.DB, table, column, decl string) {
	_, err := sqlDB.Exec(fmt.Sprintf("ALTER TABLE %s ADD COLUMN %s %s", table, column, decl))
	if err != nil {
		return // duplicate column 等——已存在即达成目的
	}
}

// LoadAll 恢复全部缓存快照。损坏的行跳过并计入 errors（不整体失败）。
func (d *DB) LoadAll() (map[string]quota.StoredSnapshot, []error) {
	rows, err := d.sql.Query(`SELECT provider_id, payload FROM snapshot_cache`)
	if err != nil {
		return nil, []error{fmt.Errorf("storage: query: %w", err)}
	}
	defer rows.Close()

	out := make(map[string]quota.StoredSnapshot)
	var errs []error
	for rows.Next() {
		var id, payload string
		if err := rows.Scan(&id, &payload); err != nil {
			errs = append(errs, fmt.Errorf("storage: scan: %w", err))
			continue
		}
		var view quota.StoredSnapshot
		if err := json.Unmarshal([]byte(payload), &view); err != nil {
			errs = append(errs, fmt.Errorf("storage: unmarshal %s: %w", id, err))
			continue
		}
		out[id] = view
	}
	if err := rows.Err(); err != nil {
		errs = append(errs, fmt.Errorf("storage: rows: %w", err))
	}
	return out, errs
}
