package ai

import (
	"io"
	"net/http"
	"testing"
)

func readAll(req *http.Request) ([]byte, error) {
	return io.ReadAll(req.Body)
}

func readAllStr(t *testing.T, r io.Reader) string {
	t.Helper()
	b, err := io.ReadAll(r)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	return string(b)
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
