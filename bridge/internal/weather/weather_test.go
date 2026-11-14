package weather

// e2e：假 ip/geo/forecast 端点（httptest）→ auto/manual 两模式 + 缓存 +
// Describe 中文映射。端点经 Client 私有字段注入（同包测试）。

import (
	"context"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
)

func newTestClient(ip, geo, forecast *httptest.Server) *Client {
	c := NewClient()
	c.ipURL = ip.URL
	c.ipFallback = ip.URL // 回退同端点（本测试只测主路径）
	c.geoURL = geo.URL
	c.forecastURL = forecast.URL
	return c
}

const forecastResp = `{
  "current": {"temperature_2m": 21.4, "relative_humidity_2m": 62,
              "apparent_temperature": 20.1, "weather_code": 61, "wind_speed_10m": 12.3},
  "daily": {
    "time": ["2026-11-14","2026-11-15","2026-11-16","2026-11-17","2026-11-18"],
    "weather_code": [0, 3, 61, 71, 95],
    "temperature_2m_max": [23, 20, 17, 5, 25],
    "temperature_2m_min": [12, 11, 9, -2, 14]
  }
}`

func TestAutoModeIPToForecast(t *testing.T) {
	var ipHits, fcHits atomic.Int32
	ip := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ipHits.Add(1)
		_, _ = w.Write([]byte(`{"status":"success","latitude":39.9,"longitude":116.4,"city":"北京市","regionName":"北京市"}`))
	}))
	fc := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fcHits.Add(1)
		if r.URL.Query().Get("forecast_days") != "5" {
			t.Errorf("want forecast_days=5, got %q", r.URL.Query().Get("forecast_days"))
		}
		_, _ = w.Write([]byte(forecastResp))
	}))
	geo := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		t.Error("auto 模式不应调 geocoding")
	}))
	t.Cleanup(func() { ip.Close(); fc.Close(); geo.Close() })

	svc := NewService(newTestClient(ip, geo, fc),
		func() ResolveConfig { return ResolveConfig{Mode: "auto"} }, nil)
	svc.Refresh(context.Background())

	v := svc.Get(context.Background(), false)
	if v.Snapshot == nil {
		t.Fatalf("snapshot nil, err=%s", v.LastError)
	}
	if v.Snapshot.Location != "北京" { // TrimCity 去掉"市"
		t.Fatalf("location=%q want 北京", v.Snapshot.Location)
	}
	if v.Snapshot.Current.Desc != "小雨" || v.Snapshot.Current.Category != "rain" {
		t.Fatalf("current desc wrong: %+v", v.Snapshot.Current)
	}
	if len(v.Snapshot.Daily) != 5 || v.Snapshot.Daily[3].Desc != "小雪" {
		t.Fatalf("daily wrong: %+v", v.Snapshot.Daily)
	}
	// 缓存：再 Get 不触发新抓取
	before := fcHits.Load()
	_ = svc.Get(context.Background(), false)
	if fcHits.Load() != before {
		t.Fatal("cache TTL 内不应重新抓取")
	}
	if ipHits.Load() == 0 {
		t.Fatal("auto 模式应调 IP 定位")
	}
}

func TestManualModeCityGeocoding(t *testing.T) {
	ip := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		t.Error("manual 模式不应调 IP 定位")
	}))
	geo := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Query().Get("name") != "上海" {
			t.Errorf("geocode name=%q", r.URL.Query().Get("name"))
		}
		_, _ = w.Write([]byte(`{"results":[{"latitude":31.2,"longitude":121.5,"name":"上海","admin1":"上海市","country":"中国"}]}`))
	}))
	fc := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(forecastResp))
	}))
	t.Cleanup(func() { ip.Close(); geo.Close(); fc.Close() })

	svc := NewService(newTestClient(ip, geo, fc),
		func() ResolveConfig { return ResolveConfig{Mode: "manual", City: "上海"} }, nil)
	svc.Refresh(context.Background())

	v := svc.Get(context.Background(), false)
	if v.Snapshot == nil || v.Snapshot.Location != "上海市·上海" {
		t.Fatalf("manual location wrong: %+v", v.Snapshot)
	}
	if v.Snapshot.Mode != "manual" {
		t.Fatalf("mode=%s", v.Snapshot.Mode)
	}
}

func TestLocateFailureKeepsStale(t *testing.T) {
	ip := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"status":"fail"}`))
	}))
	geo := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"results":[]}`))
	}))
	fc := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	t.Cleanup(func() { ip.Close(); geo.Close(); fc.Close() })

	svc := NewService(newTestClient(ip, geo, fc),
		func() ResolveConfig { return ResolveConfig{Mode: "auto"} }, nil)
	svc.Refresh(context.Background())
	v := svc.Get(context.Background(), false)
	if v.Snapshot != nil || !v.Stale {
		t.Fatalf("want nil+stale, got %+v", v)
	}
}

func TestDescribeUnknownCode(t *testing.T) {
	if d, c := Describe(9999); d != "多云" || c != "cloud" {
		t.Fatalf("unknown code fallback wrong: %s %s", d, c)
	}
}
