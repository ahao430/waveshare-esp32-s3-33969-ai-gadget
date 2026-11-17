package api

// writeJSON 定长回归：真机事故（2026-11-17）——流式 Encoder 让 >2048B
// 的 JSON 响应自动 chunked，设备端 http_get 不解块导致订阅页全体消失。
// 必须起真 HTTP 服务断言（httptest.Recorder 不会触发分块降级）。
import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestWriteJSON_NoChunkedOver2KB(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, nil, http.StatusOK, map[string]any{
			"padding": strings.Repeat("x", 4096), // 远超 Go 内部 2048B 缓冲
		})
	}))
	defer srv.Close()

	resp, err := http.Get(srv.URL)
	if err != nil {
		t.Fatalf("get: %v", err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)

	if resp.TransferEncoding != nil {
		t.Fatalf("响应被分块了：Transfer-Encoding=%v（设备端解不了）", resp.TransferEncoding)
	}
	if resp.ContentLength <= 0 || int(resp.ContentLength) != len(body) {
		t.Fatalf("Content-Length=%d 与实体 %d 不符", resp.ContentLength, len(body))
	}
	if !strings.HasPrefix(string(body), "{\n  \"padding\"") {
		t.Fatalf("响应体异常：%.40s", body)
	}
}
