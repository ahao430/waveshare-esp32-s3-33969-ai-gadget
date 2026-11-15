// preview_test.go — Web 后台音色试听（POST /api/v1/voice/preview）契约：
// 参数透传、WAV 响应、失败原因回显、不支持试听的引擎 501。
package voice

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// stubPreview 同时实现 Interactor 与 PreviewInteractor。
type stubPreview struct {
	gotProvider, gotVoice, gotText string
	wav                            []byte
	err                            error
}

func (s *stubPreview) Transcribe(context.Context, []byte) (string, error) { return "", nil }
func (s *stubPreview) Respond(context.Context, string) (string, error)    { return "", nil }
func (s *stubPreview) Synthesize(context.Context, string) ([]byte, error) { return nil, nil }

func (s *stubPreview) Preview(_ context.Context, provider, voice, text string) ([]byte, error) {
	s.gotProvider, s.gotVoice, s.gotText = provider, voice, text
	return s.wav, s.err
}

func postPreview(t *testing.T, h *Handler, body string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, "/api/v1/voice/preview", strings.NewReader(body))
	rec := httptest.NewRecorder()
	h.ServePreview(rec, req)
	return rec
}

func TestServePreviewOK(t *testing.T) {
	wav := wrapWav([]int16{1, 2, 3})
	stub := &stubPreview{wav: wav}
	rec := postPreview(t, NewHandler(stub), `{"provider":"edge","voice":"zh-CN-YunxiNeural"}`)

	if rec.Code != http.StatusOK {
		t.Fatalf("code = %d, body=%s", rec.Code, rec.Body.String())
	}
	if ct := rec.Header().Get("Content-Type"); ct != "audio/wav" {
		t.Fatalf("content-type = %q", ct)
	}
	if got := rec.Body.Bytes(); string(got) != string(wav) {
		t.Fatalf("body mismatch (%d bytes)", len(got))
	}
	if stub.gotProvider != "edge" || stub.gotVoice != "zh-CN-YunxiNeural" {
		t.Fatalf("params = %q/%q", stub.gotProvider, stub.gotVoice)
	}
}

func TestServePreviewErrorReason(t *testing.T) {
	stub := &stubPreview{err: errors.New("火山 TTS 未配置")}
	rec := postPreview(t, NewHandler(stub), `{"provider":"volcengine"}`)

	if rec.Code != http.StatusBadGateway {
		t.Fatalf("code = %d", rec.Code)
	}
	var body struct {
		Error   string `json:"error"`
		Message string `json:"message"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("json: %v (%s)", err, rec.Body.String())
	}
	if body.Error != "preview_failed" || !strings.Contains(body.Message, "未配置") {
		t.Fatalf("body = %+v", body)
	}
}

func TestServePreviewUnsupported(t *testing.T) {
	// MockInteractor 没有 Preview 能力 → 501（不是 500）
	rec := postPreview(t, NewHandler(NewMockInteractor()), `{}`)
	if rec.Code != http.StatusNotImplemented {
		t.Fatalf("code = %d", rec.Code)
	}
}

// TestPreviewOrder 自动档试听顺序：音色形态优先，其次 Key 可用性。
func TestPreviewOrder(t *testing.T) {
	t.Setenv("TTS_PROVIDER", "")

	edgeOnly := NewRealInteractor(ASRKeys{}, "", nil)
	if got := edgeOnly.previewOrder(""); len(got) != 1 || got[0] != "edge" {
		t.Fatalf("no key/voice order = %v", got)
	}
	if got := edgeOnly.previewOrder("BV001_streaming"); len(got) != 1 || got[0] != "volcengine" {
		t.Fatalf("BV order = %v", got)
	}
	if got := edgeOnly.previewOrder("zh-CN-XiaoxiaoNeural"); len(got) != 1 || got[0] != "edge" {
		t.Fatalf("Neural order = %v", got)
	}

	withDS := NewRealInteractor(ASRKeys{Deepseek: func() string { return "k" }}, "", nil)
	if got := withDS.previewOrder(""); len(got) != 2 || got[0] != "deepseek" || got[1] != "edge" {
		t.Fatalf("deepseek key order = %v", got)
	}
}
