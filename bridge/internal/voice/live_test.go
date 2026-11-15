// live_test.go — 真网冒烟（默认跳过；GADGET_VOICE_LIVE=1 时跑）：
//
//	TestEdgeTTSLive：合成中文短句 → 校验 16k PCM 形态（长度、非静音）
//	TestZhipuASRLive：转写本地 WAV → 校验非空文本（需 ZHIPU_API_KEY）
//
// CI 与 go test ./... 不打真网；验证记录进提交信息。
package voice

import (
	"context"
	"os"
	"testing"
	"time"
)

func liveEnabled(t *testing.T) {
	t.Helper()
	if os.Getenv("GADGET_VOICE_LIVE") != "1" {
		t.Skip("set GADGET_VOICE_LIVE=1 to run live smoke")
	}
}

func TestEdgeTTSLive(t *testing.T) {
	liveEnabled(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	pcm, err := edgeSynthesize(ctx, "你好，这是设备语音链路的真实语音测试。", "")
	if err != nil {
		t.Fatalf("synthesize: %v", err)
	}
	// 18 字 ≈ 4~6s @16k16bit → 128~192KB
	if len(pcm) < 16000 {
		t.Fatalf("suspiciously short: %d samples", len(pcm))
	}
	var peak int16
	for i, v := range pcm {
		if i%997 == 0 { // 抽样即可
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
		t.Fatalf("audio is silent (peak=%d)", peak)
	}
	t.Logf("ok: %d samples (~%.1fs), peak %d", len(pcm), float64(len(pcm))/16000, peak)

	// 可选落盘人工试听：GADGET_VOICE_LIVE_OUT=<file>
	if p := os.Getenv("GADGET_VOICE_LIVE_OUT"); p != "" {
		if err := os.WriteFile(p, wrapWav(pcm), 0644); err != nil {
			t.Fatalf("write: %v", err)
		}
	}
}

func TestZhipuASRLive(t *testing.T) {
	liveEnabled(t)
	key := os.Getenv("ZHIPU_API_KEY")
	if key == "" {
		t.Skip("set ZHIPU_API_KEY to run asr live smoke")
	}
	p := os.Getenv("GADGET_ASR_PCM")
	if p == "" {
		t.Skip("set GADGET_ASR_PCM=<16k mono pcm file>")
	}
	pcm, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("read pcm: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	text, err := zhipuTranscribe(ctx, key, "", pcm)
	if err != nil {
		t.Fatalf("transcribe: %v", err)
	}
	if text == "" {
		t.Fatal("empty transcript")
	}
	t.Logf("transcript: %s", text)
}
