package xiaozhi

import (
	"crypto/rand"
	"encoding/hex"
)

// newUUID 随机 UUIDv4（小写 8-4-4-4-12；官方仅要求稳定唯一）。
func newUUID() string {
	var b [16]byte
	_, _ = rand.Read(b[:])
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	s := hex.EncodeToString(b[:])
	return s[0:8] + "-" + s[8:12] + "-" + s[12:16] + "-" + s[16:20] + "-" + s[20:32]
}
