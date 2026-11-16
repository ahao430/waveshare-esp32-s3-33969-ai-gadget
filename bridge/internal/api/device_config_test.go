package api

// device config 契约：部分更新合并语义（设备回写亮度/音量不清 Web 配的
// 订阅间隔）、volume 0=静音必须能存取（指针字段，omitempty 不吞 0）、
// 各字段域校验。

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func putDevCfg(t *testing.T, srv *httptest.Server, body string) int {
	t.Helper()
	req, err := http.NewRequest(http.MethodPut, srv.URL+"/api/v1/device/config",
		bytes.NewBufferString(body))
	if err != nil {
		t.Fatal(err)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	return resp.StatusCode
}

func getDevCfg(t *testing.T, srv *httptest.Server) map[string]any {
	t.Helper()
	resp, err := http.Get(srv.URL+"/api/v1/device/config")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET status = %d", resp.StatusCode)
	}
	var m map[string]any
	if err := json.NewDecoder(resp.Body).Decode(&m); err != nil {
		t.Fatal(err)
	}
	return m
}

func TestDeviceConfigMergeAndVolumeZero(t *testing.T) {
	srv, _ := newConfigServer(t, newTestMasterKey(t))
	defer srv.Close()

	// 初始：只有默认订阅间隔
	m := getDevCfg(t, srv)
	if m["sub_refresh_interval_s"].(float64) != 900 {
		t.Fatalf("default interval = %v", m["sub_refresh_interval_s"])
	}
	if _, ok := m["brightness"]; ok {
		t.Fatal("brightness 应缺省")
	}

	// 设备回写只带亮度/音量：订阅间隔必须保留（合并，不是替换）
	if c := putDevCfg(t, srv, `{"brightness":70,"volume":40}`); c != 200 {
		t.Fatalf("PUT device fields status = %d", c)
	}
	m = getDevCfg(t, srv)
	if m["sub_refresh_interval_s"].(float64) != 900 {
		t.Fatalf("partial PUT 清掉了订阅间隔: %v", m)
	}
	if m["brightness"].(float64) != 70 || m["volume"].(float64) != 40 {
		t.Fatalf("brightness/volume 未生效: %v", m)
	}

	// Web 只改订阅间隔：亮度/音量必须保留
	if c := putDevCfg(t, srv, `{"sub_refresh_interval_s":1800}`); c != 200 {
		t.Fatalf("PUT interval status = %d", c)
	}
	m = getDevCfg(t, srv)
	if m["sub_refresh_interval_s"].(float64) != 1800 {
		t.Fatalf("interval 未生效: %v", m)
	}
	if m["brightness"].(float64) != 70 || m["volume"].(float64) != 40 {
		t.Fatalf("interval PUT 清掉了 brightness/volume: %v", m)
	}

	// volume 0 = 静音是合法值：必须存得下、GET 必须回得来
	if c := putDevCfg(t, srv, `{"volume":0}`); c != 200 {
		t.Fatalf("PUT volume=0 status = %d", c)
	}
	m = getDevCfg(t, srv)
	if v, ok := m["volume"]; !ok || v.(float64) != 0 {
		t.Fatalf("volume=0 被吞（omitempty 必须用指针）: %v", m)
	}

	// 域校验
	for _, tc := range []struct{ body, want string }{
		{`{}`, "empty"},
		{`{"brightness":5}`, "brightness<10"},
		{`{"brightness":101}`, "brightness>100"},
		{`{"volume":-1}`, "volume<0"},
		{`{"volume":101}`, "volume>100"},
		{`{"sub_refresh_interval_s":30}`, "interval<60"},
		{`{"sub_refresh_interval_s":3601}`, "interval>3600"},
	} {
		if c := putDevCfg(t, srv, tc.body); c != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400", tc.want, c)
		}
	}
}
