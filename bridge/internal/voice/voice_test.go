package voice

import (
	"bytes"
	"context"
	"encoding/binary"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestMockPipeline(t *testing.T) {
	eng := NewMockInteractor()

	// ASR stub 轮换
	t1, err := eng.Transcribe(context_bg(), []byte{1, 2, 3})
	if err != nil || t1 == "" {
		t.Fatalf("transcribe: %v %q", err, t1)
	}
	t2, _ := eng.Transcribe(context_bg(), []byte{4})
	if t1 == t2 {
		t.Fatalf("stub 转写应轮换: %q == %q", t1, t2)
	}

	// LLM stub
	reply, err := eng.Respond(context_bg(), t1)
	if err != nil || !strings.Contains(reply, t1) {
		t.Fatalf("respond: %v %q", err, reply)
	}

	// TTS stub：合法 WAV 头
	wav, err := eng.Synthesize(context_bg(), "测试")
	if err != nil {
		t.Fatalf("synthesize: %v", err)
	}
	if len(wav) < 45 || string(wav[0:4]) != "RIFF" || string(wav[8:12]) != "WAVE" {
		t.Fatalf("WAV 头错误: % x", wav[:12])
	}
	if got := binary.LittleEndian.Uint32(wav[24:28]); got != 16000 {
		t.Fatalf("采样率 = %d, want 16000", got)
	}
	if got := binary.LittleEndian.Uint16(wav[22:24]); got != 1 {
		t.Fatalf("声道 = %d, want mono", got)
	}
	dataLen := binary.LittleEndian.Uint32(wav[40:44])
	if int(dataLen) != len(wav)-44 {
		t.Fatalf("data 长度不一致: %d vs %d", dataLen, len(wav)-44)
	}
}

func TestServeInteractE2E(t *testing.T) {
	h := NewHandler(NewMockInteractor())
	srv := httptest.NewServer(http.HandlerFunc(h.ServeInteract))
	t.Cleanup(srv.Close)

	// 1 秒静音 PCM（16k/16bit mono）
	pcm := make([]byte, 32000)
	resp, err := srv.Client().Post(srv.URL, "application/octet-stream", bytes.NewReader(pcm))
	if errs(resp, err) {
		t.Fatalf("post: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		body, _ := io.ReadAll(resp.Body)
		t.Fatalf("status = %d body=%s", resp.StatusCode, body)
	}
	if ct := resp.Header.Get("Content-Type"); ct != "audio/wav" {
		t.Fatalf("content-type = %s", ct)
	}
	if resp.Header.Get("X-Voice-Transcript") == "" || resp.Header.Get("X-Voice-Reply") == "" {
		t.Fatal("响应头缺少 transcript/reply")
	}
	wav, _ := io.ReadAll(resp.Body)
	if len(wav) < 44 || string(wav[0:4]) != "RIFF" {
		t.Fatalf("WAV 内容错误: len=%d head=% x", len(wav), wav[:min(8, len(wav))])
	}
}

func TestServeInteractEmpty(t *testing.T) {
	h := NewHandler(NewMockInteractor())
	srv := httptest.NewServer(http.HandlerFunc(h.ServeInteract))
	t.Cleanup(srv.Close)

	resp, err := srv.Client().Post(srv.URL, "application/octet-stream", bytes.NewReader(nil))
	if err != nil {
		t.Fatalf("post: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("空音频应 400, got %d", resp.StatusCode)
	}
}

func errs(resp *http.Response, err error) bool {
	_ = resp
	return err != nil
}

func context_bg() context.Context { return context.Background() }

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
