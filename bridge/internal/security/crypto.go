// Package security 提供凭据静态加密（PLAN §15.3：敏感凭据加密存储，
// 密钥由部署环境提供，不提交 Git）。
//
// 方案：AES-256-GCM（标准库）。密文格式 `v1:<b64 nonce>:<b64 ct>`，
// 版本前缀支持将来密钥轮换。主密钥 32 字节，经 BRIDGE_MASTER_KEY
// 环境变量以 hex 注入；缺失时凭据持久化功能整体禁用（fail-safe 默认）。
package security

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
)

var (
	ErrNoMasterKey   = errors.New("master_key_missing")
	ErrBadMasterKey  = errors.New("master_key_invalid")
	ErrBadCiphertext = errors.New("ciphertext_invalid")
)

const ciphertextVersion = "v1"

// ParseMasterKey 把 hex 编码的环境变量解析为 32 字节 AES-256 密钥。
func ParseMasterKey(hexKey string) ([]byte, error) {
	if hexKey == "" {
		return nil, ErrNoMasterKey
	}
	key, err := hex.DecodeString(strings.TrimSpace(hexKey))
	if err != nil {
		return nil, fmt.Errorf("%w: not hex: %v", ErrBadMasterKey, err)
	}
	if len(key) != 32 {
		return nil, fmt.Errorf("%w: want 32 bytes (64 hex chars), got %d", ErrBadMasterKey, len(key))
	}
	return key, nil
}

// GenerateMasterKey 生成随机 32 字节密钥的 hex 串（部署引导用）。
func GenerateMasterKey() (string, error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

// Encrypt 明文 → `v1:nonce:ct`（nonce 与密文均 base64 raw-url）。
func Encrypt(plain string, key []byte) (string, error) {
	block, err := aes.NewCipher(key)
	if err != nil {
		return "", err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return "", err
	}
	nonce := make([]byte, gcm.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		return "", err
	}
	ct := gcm.Seal(nil, nonce, []byte(plain), nil)
	return ciphertextVersion + ":" +
		base64.RawURLEncoding.EncodeToString(nonce) + ":" +
		base64.RawURLEncoding.EncodeToString(ct), nil
}

// Decrypt `v1:nonce:ct` → 明文。密文被篡改或密钥不匹配时报错
// （GCM auth；用 subtle.ConstantTimeCompare 做版本前缀比较，防时序侧信道）。
func Decrypt(enc string, key []byte) (string, error) {
	parts := strings.Split(enc, ":")
	if len(parts) != 3 ||
		subtle.ConstantTimeCompare([]byte(parts[0]), []byte(ciphertextVersion)) != 1 {
		return "", ErrBadCiphertext
	}
	nonce, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return "", fmt.Errorf("%w: nonce: %v", ErrBadCiphertext, err)
	}
	ct, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return "", fmt.Errorf("%w: ct: %v", ErrBadCiphertext, err)
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return "", err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return "", err
	}
	plain, err := gcm.Open(nil, nonce, ct, nil)
	if err != nil {
		return "", fmt.Errorf("%w: auth failed: %v", ErrBadCiphertext, err)
	}
	return string(plain), nil
}
