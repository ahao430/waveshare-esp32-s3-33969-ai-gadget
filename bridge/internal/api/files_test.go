package api

import (
	"bytes"
	"io"
	"net/http"
	"net/url"
	"testing"
)

// 设备文件互通（ADR-010）契约：上传（含中文名）→ 列表 → 下载 → 删除，
// 以及非法文件名拒绝（路径穿越）。
func TestDeviceFiles(t *testing.T) {
	mk := newTestMasterKey(t)
	dir := t.TempDir()
	srv, _ := newConfigServerWith(t, mk, func(d *Deps) { d.FilesDir = dir })

	// 空列表
	code, out := doJSON(t, "GET", srv.URL+"/api/v1/files", "")
	if code != 200 {
		t.Fatalf("list: %d %v", code, out)
	}
	if n := len(out["files"].([]any)); n != 0 {
		t.Fatalf("want empty, got %d", n)
	}

	// 上传（中文名，设备端会 percent 编码）
	name := "测试 文件.txt"
	payload := []byte("hello gadget")
	resp, err := http.Post(srv.URL+"/api/v1/files/"+url.PathEscape(name),
		"application/octet-stream", bytes.NewReader(payload))
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("upload: %d", resp.StatusCode)
	}

	// 列表可见
	_, out = doJSON(t, "GET", srv.URL+"/api/v1/files", "")
	files := out["files"].([]any)
	if len(files) != 1 || files[0].(map[string]any)["name"] != name {
		t.Fatalf("list after upload: %v", files)
	}

	// 下载内容一致 + 显式 Content-Length（设备端不解 chunked）
	dl, err := http.Get(srv.URL + "/api/v1/files/" + url.PathEscape(name))
	if err != nil {
		t.Fatal(err)
	}
	defer dl.Body.Close()
	got, _ := io.ReadAll(dl.Body)
	if dl.StatusCode != 200 || !bytes.Equal(got, payload) {
		t.Fatalf("download: %d body=%q", dl.StatusCode, got)
	}
	if dl.ContentLength != int64(len(payload)) {
		t.Fatalf("chunked? Content-Length=%d", dl.ContentLength)
	}

	// 非法文件名：%2e%2e（".."）与带斜杠 → 400
	for _, bad := range []string{"%2e%2e", "a%2fb"} {
		req, _ := http.NewRequest("POST", srv.URL+"/api/v1/files/"+bad, bytes.NewReader([]byte("x")))
		r, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		r.Body.Close()
		if r.StatusCode != 400 && r.StatusCode != 404 {
			t.Fatalf("bad name %q want 4xx, got %d", bad, r.StatusCode)
		}
	}

	// 删除后读 404
	req, _ := http.NewRequest("DELETE", srv.URL+"/api/v1/files/"+url.PathEscape(name), nil)
	dr, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	dr.Body.Close()
	if dr.StatusCode != 200 {
		t.Fatalf("delete: %d", dr.StatusCode)
	}
	if dl2, err := http.Get(srv.URL + "/api/v1/files/" + url.PathEscape(name)); err == nil {
		dl2.Body.Close()
		if dl2.StatusCode != 404 {
			t.Fatalf("want 404 after delete, got %d", dl2.StatusCode)
		}
	}
}
