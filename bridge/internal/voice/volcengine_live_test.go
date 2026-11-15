// volcengine_live_test.go — 火山豆包真网冒烟（GADGET_VOICE_LIVE=1 +
// GADGET_VOLC_KEY="AppID:AccessToken"）。TTS 合成 → ASR 回转闭环，
// 与 TestDeepSeekVoiceLive 同一模式。
package voice

import (
	"context"
	"os"
	"testing"
	"time"
)

func TestVolcengineVoiceLive(t *testing.T) {
	liveEnabled(t)
	key := os.Getenv("GADGET_VOLC_KEY")
	if key == "" {
		t.Skip(`set GADGET_VOLC_KEY="AppID:AccessToken" to run volcengine live smoke`)
	}

	// TTS：默认音色 BV001，16k PCM 直出
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	pcm, err := volcengineTTS(ctx, key, "你好，这是火山豆包语音合成测试。", "")
	if err != nil {
		t.Fatalf("tts: %v", err)
	}
	dur := float64(len(pcm)) / 16000
	if dur < 1.0 || dur > 12.0 {
		t.Fatalf("tts duration out of range: %.2fs", dur)
	}
	var peak int16
	for i, v := range pcm {
		if i%997 == 0 && (v > peak || -v > peak) {
			if v > 0 {
				peak = v
			} else {
				peak = -v
			}
		}
	}
	if peak < 1000 {
		t.Fatalf("tts silent: peak=%d", peak)
	}
	t.Logf("tts ok: %.2fs peak=%d", dur, peak)
	if p := os.Getenv("GADGET_VOLC_TTS_OUT"); p != "" {
		if err := os.WriteFile(p, wrapWav(pcm), 0644); err != nil {
			t.Fatalf("write: %v", err)
		}
	}

	// ASR：把合成结果转写回来（语义正确性人工核对；顺带验证
	// bigmodel_nostream 协议与 rate=16000 请求在真网的行为）
	actx, acancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer acancel()
	pcmBytes := make([]byte, len(pcm)*2)
	for i, v := range pcm {
		pcmBytes[i*2] = byte(v)
		pcmBytes[i*2+1] = byte(v >> 8)
	}
	text, err := volcengineTranscribe(actx, key, pcmBytes)
	if err != nil {
		t.Fatalf("asr: %v", err)
	}
	t.Logf("asr ok: %q", text)
}
