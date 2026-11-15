package voice

import (
	"os"
	"testing"
)

// 离线转码回归：24kHz MP3 fixture（"你好，这是设备语音链路测试。"，
// 真网合成产物）→ 16k PCM。锁三件事：解码不报错、时长合理（≈1.6s
// ±25%）、有信号非静音。采样率变换正确性由"时长=样本数/16k"间接锁定
// （若率错，同样字节量下时长会偏移 50%，超出容差）。
func TestEdgeMp3ToPCM16k(t *testing.T) {
	raw, err := os.ReadFile("testdata/sample_24k.mp3")
	if err != nil {
		t.Skipf("fixture missing: %v", err)
	}
	pcm, err := edgeMp3ToPCM16k(raw)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	// fixture = "你好，这是设备语音链路测试。" ≈ 3.26s（48kbps MP3
	// 19584 字节推算）；容差 ±20% 锁定 LSF 双倍补偿不回归
	dur := float64(len(pcm)) / 16000.0
	if dur < 2.6 || dur > 3.9 {
		t.Fatalf("duration out of range: %.2fs (%d samples)", dur, len(pcm))
	}
	var peak int16
	for i, v := range pcm {
		if i%997 == 0 {
			if v > peak || -v > peak {
				if v > 0 {
					peak = v
				} else {
					peak = -v
				}
			}
		}
	}
	if peak < 1000 {
		t.Fatalf("silent audio: peak=%d", peak)
	}
}

// 重采样纯函数：恒等率（16k→16k）、24k→16k 长度比与边界。
func TestResampleTo16k(t *testing.T) {
	if got := resampleTo16k([]int16{1, 2, 3}, 16000); len(got) != 3 {
		t.Fatalf("identity resample changed length: %d", len(got))
	}
	src := make([]int16, 24000) // 1s @24k
	for i := range src {
		src[i] = int16(i % 1000)
	}
	got := resampleTo16k(src, 24000)
	if len(got) != 16000 {
		t.Fatalf("24k->16k length = %d, want 16000", len(got))
	}
	// 边界：from<=0 原样返回
	if got := resampleTo16k(src, 0); len(got) != len(src) {
		t.Fatal("invalid rate should return src unchanged")
	}
}
