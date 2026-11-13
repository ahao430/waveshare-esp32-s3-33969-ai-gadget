package security

import (
	"strings"
	"testing"
)

func TestMasterKeyParse(t *testing.T) {
	k, err := ParseMasterKey("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff")
	if err != nil || len(k) != 32 {
		t.Fatalf("parse: %v len=%d", err, len(k))
	}
	if _, err := ParseMasterKey(""); err == nil {
		t.Fatal("空串应报 missing")
	}
	if _, err := ParseMasterKey("zz"); err == nil {
		t.Fatal("非 hex 应报错")
	}
	if _, err := ParseMasterKey("aabb"); err == nil {
		t.Fatal("长度不足应报错")
	}
}

func TestEncryptDecryptRoundtrip(t *testing.T) {
	key, _ := ParseMasterKey("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff")
	plain := "sk-secret-api-key-字母-key_123"

	enc, err := Encrypt(plain, key)
	if err != nil {
		t.Fatalf("encrypt: %v", err)
	}
	if strings.Contains(enc, "sk-secret") {
		t.Fatal("密文泄漏明文子串")
	}
	got, err := Decrypt(enc, key)
	if err != nil || got != plain {
		t.Fatalf("roundtrip: %v %q", err, got)
	}

	// 每次加密 nonce 不同
	enc2, _ := Encrypt(plain, key)
	if enc2 == enc {
		t.Fatal("两次加密密文相同（nonce 未随机）")
	}

	// 篡改检测
	bad := []byte(enc)
	bad[len(bad)-1] ^= 0x01
	if _, err := Decrypt(string(bad), key); err == nil {
		t.Fatal("篡改密文应解密失败")
	}

	// 错误密钥
	key2, _ := ParseMasterKey("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")
	if _, err := Decrypt(enc, key2); err == nil {
		t.Fatal("错误密钥应失败")
	}
}
