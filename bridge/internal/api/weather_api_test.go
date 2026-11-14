package api

// 天气 API e2e：config 保存（auto/manual 校验）→ weather 视图回读。

import (
	"net/http"
	"net/http/httptest"
	"testing"

	"gadget-bridge/internal/weather"
)

func TestWeatherConfigAndFetch(t *testing.T) {
	mk := newTestMasterKey(t)
	ip := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"status":"success","latitude":22.5,"longitude":114.0,"city":"深圳市","regionName":"广东省"}`))
	}))
	fc := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"current":{"temperature_2m":28.2,"relative_humidity_2m":70,"apparent_temperature":30.5,"weather_code":80,"wind_speed_10m":8},"daily":{"time":["2026-11-14"],"weather_code":[80],"temperature_2m_max":[29],"temperature_2m_min":[22]}}`))
	}))
	t.Cleanup(func() { ip.Close(); fc.Close() })

	svc := weather.NewService(
		weather.NewClientWithEndpoints(ip.URL, ip.URL, ip.URL, fc.URL),
		func() weather.ResolveConfig { return weather.ResolveConfig{Mode: "auto"} }, nil)
	svc.Refresh(t.Context())

	srv, _ := newConfigServerWith(t, mk, func(d *Deps) { d.Weather = svc })

	// 默认配置：auto
	code, out := doJSON(t, "GET", srv.URL+"/api/v1/weather/config", "")
	if code != 200 || out["mode"] != "auto" {
		t.Fatalf("default config: %d %v", code, out)
	}

	// 手动城市保存 + 回读
	code, out = doJSON(t, "POST", srv.URL+"/api/v1/weather/config",
		`{"mode":"manual","city":"上海"}`)
	if code != 200 {
		t.Fatalf("save manual: %d %v", code, out)
	}
	_, out = doJSON(t, "GET", srv.URL+"/api/v1/weather/config", "")
	if out["mode"] != "manual" || out["city"] != "上海" {
		t.Fatalf("config view: %v", out)
	}

	// 非法 mode 拒绝；manual 无 city 拒绝
	if code, _ = doJSON(t, "POST", srv.URL+"/api/v1/weather/config", `{"mode":"gps"}`); code != 400 {
		t.Fatalf("bad mode want 400, got %d", code)
	}
	if code, _ = doJSON(t, "POST", srv.URL+"/api/v1/weather/config", `{"mode":"manual"}`); code != 400 {
		t.Fatalf("manual w/o city want 400, got %d", code)
	}

	// 天气视图（缓存命中）
	code, out = doJSON(t, "GET", srv.URL+"/api/v1/weather", "")
	if code != 200 {
		t.Fatalf("weather view: %d", code)
	}
	snap, _ := out["snapshot"].(map[string]any)
	if snap == nil || snap["location"] != "深圳" {
		t.Fatalf("snapshot wrong: %v", out)
	}
	cur, _ := snap["current"].(map[string]any)
	if cur["desc"] != "小阵雨" || cur["temp_c"] != 28.2 {
		t.Fatalf("current wrong: %v", cur)
	}
}
