// Package weather — 天气数据源（PLAN §5 天气 App，用户 2026-11-14 需求）。
// 上游 Open-Meteo（免费、无需 API Key）：
//   - 定位：auto=IP 定位（ip-api.com 中文 / ipwho.is 回退）；manual=城市名
//     经 Open-Meteo Geocoding 转坐标。
//   - 天气：api.open-meteo.com/v1/forecast（current + 5 日 daily）。
//
// Bridge 缓存 10 分钟并后台保温（设备拉取永远命中缓存，不阻塞 UI）。
package weather

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// ErrNoLocation 定位失败（IP 服务不可达 / 城市名解析不到）。
var ErrNoLocation = errors.New("location_failed")

// Current 实况。
type Current struct {
	TempC    float64 `json:"temp_c"`
	FeelsC   float64 `json:"feels_c"`
	Humidity int     `json:"humidity"` // %
	WindKmh  int     `json:"wind_kmh"` // km/h
	Code     int     `json:"code"`     // WMO weather code
	Desc     string  `json:"desc"`     // 中文描述（Bridge 侧翻译）
	Category string  `json:"category"` // sun|cloud|fog|rain|snow|thunder
}

// Day 一天预报。
type Day struct {
	Date string  `json:"date"` // YYYY-MM-DD
	Code int     `json:"code"`
	Desc string  `json:"desc"`
	Tmax float64 `json:"t_max"`
	Tmin float64 `json:"t_min"`
}

// Snapshot 一次完整抓取。
type Snapshot struct {
	Location  string    `json:"location"`
	Mode      string    `json:"mode"` // auto|manual
	FetchedAt time.Time `json:"fetched_at"`
	Current   Current   `json:"current"`
	Daily     []Day     `json:"daily"`
}

// Client 抓取 Open-Meteo / IP 定位（URL 可注入，测试用 httptest）。
type Client struct {
	ipURL       string
	ipFallback  string
	geoURL      string
	forecastURL string
	hc          *http.Client
}

// NewClient 默认端点（全部免费无 Key）。不带 fields 过滤（实测显式
// 列字段名会被部分服务忽略/回落全量，全量响应更稳）。
func NewClient() *Client {
	return &Client{
		ipURL:       "http://ip-api.com/json/?lang=zh-CN",
		ipFallback:  "https://ipwho.is/",
		geoURL:      "https://geocoding-api.open-meteo.com/v1/search",
		forecastURL: "https://api.open-meteo.com/v1/forecast",
		hc:          &http.Client{Timeout: 15 * time.Second},
	}
}

// NewClientWithEndpoints 自定义端点构造（测试注入 httptest；部署侧
// 代理同理）。传空串回落默认端点。
func NewClientWithEndpoints(ip, ipFallback, geo, forecast string) *Client {
	c := NewClient()
	if ip != "" {
		c.ipURL = ip
	}
	if ipFallback != "" {
		c.ipFallback = ipFallback
	}
	if geo != "" {
		c.geoURL = geo
	}
	if forecast != "" {
		c.forecastURL = forecast
	}
	return c
}

func (c *Client) getJSON(ctx context.Context, rawURL string, out any) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, rawURL, nil)
	if err != nil {
		return err
	}
	req.Header.Set("Accept", "application/json")
	resp, err := c.hc.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("http_%d", resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return err
	}
	return json.Unmarshal(body, out)
}

// hasCJK 判断是否含中日韩字符（IP 库的 city 常为英文，region 为中文）。
func hasCJK(s string) bool {
	for _, r := range s {
		if r >= 0x4E00 && r <= 0x9FFF {
			return true
		}
	}
	return false
}

// pickName city/region 中优先取中文名（展示更友好），否则 city。
func pickName(city, region string) string {
	if city != "" && hasCJK(city) {
		return city
	}
	if region != "" && hasCJK(region) {
		return region
	}
	if city != "" {
		return city
	}
	return region
}

// LocateByIP 自动定位（主 ip-api，中文 region；失败回退 ipwho.is）。
// 两服务字段名分别为 latitude/longitude（实测 lat/lon 短名不可靠）。
func (c *Client) LocateByIP(ctx context.Context) (lat, lon float64, name string, err error) {
	var r struct {
		Status  string  `json:"status"`
		Lat     float64 `json:"latitude"`
		Lon     float64 `json:"longitude"`
		City    string  `json:"city"`
		Region  string  `json:"regionName"`
		Success *bool   `json:"success"` // ipwho.is 用 success 布尔
	}
	if e := c.getJSON(ctx, c.ipURL, &r); e == nil && r.Lat != 0 && r.Lon != 0 {
		return r.Lat, r.Lon, pickName(r.City, r.Region), nil
	}
	r = struct {
		Status  string  `json:"status"`
		Lat     float64 `json:"latitude"`
		Lon     float64 `json:"longitude"`
		City    string  `json:"city"`
		Region  string  `json:"regionName"`
		Success *bool   `json:"success"`
	}{}
	if e := c.getJSON(ctx, c.ipFallback, &r); e == nil && r.Lat != 0 && r.Lon != 0 {
		return r.Lat, r.Lon, pickName(r.City, r.Region), nil
	}
	return 0, 0, "", ErrNoLocation
}

// LocateByCity 城市名 → 坐标（Open-Meteo Geocoding，中文结果优先）。
func (c *Client) LocateByCity(ctx context.Context, city string) (lat, lon float64, name string, err error) {
	if city == "" {
		return 0, 0, "", ErrNoLocation
	}
	q := url.Values{"name": {city}, "count": {"1"}, "language": {"zh"}}
	var r struct {
		Results []struct {
			Lat     float64 `json:"latitude"`
			Lon     float64 `json:"longitude"`
			Name    string  `json:"name"`
			Admin1  string  `json:"admin1"`
			Country string  `json:"country"`
		} `json:"results"`
	}
	if err = c.getJSON(ctx, c.geoURL+"?"+q.Encode(), &r); err != nil {
		return 0, 0, "", ErrNoLocation
	}
	if len(r.Results) == 0 {
		return 0, 0, "", ErrNoLocation
	}
	g := r.Results[0]
	name = g.Name
	if g.Admin1 != "" && g.Admin1 != g.Name {
		name = g.Admin1 + "·" + g.Name
	}
	return g.Lat, g.Lon, name, nil
}

// Forecast 实况 + 5 日预报。
func (c *Client) Forecast(ctx context.Context, lat, lon float64) (Current, []Day, error) {
	q := url.Values{
		"latitude":      {fmt.Sprintf("%.4f", lat)},
		"longitude":     {fmt.Sprintf("%.4f", lon)},
		"current":       {"temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"},
		"daily":         {"weather_code,temperature_2m_max,temperature_2m_min"},
		"forecast_days": {"5"},
		"timezone":      {"auto"},
	}
	var r struct {
		Current struct {
			Temperature float64 `json:"temperature_2m"`
			Humidity    float64 `json:"relative_humidity_2m"`
			Apparent    float64 `json:"apparent_temperature"`
			Code        int     `json:"weather_code"`
			Wind        float64 `json:"wind_speed_10m"`
		} `json:"current"`
		Daily struct {
			Time []string  `json:"time"`
			Code []int     `json:"weather_code"`
			Tmax []float64 `json:"temperature_2m_max"`
			Tmin []float64 `json:"temperature_2m_min"`
		} `json:"daily"`
	}
	if err := c.getJSON(ctx, c.forecastURL+"?"+q.Encode(), &r); err != nil {
		return Current{}, nil, err
	}
	if r.Daily.Time == nil {
		return Current{}, nil, fmt.Errorf("unsupported_response")
	}
	cur := Current{
		TempC:    r.Current.Temperature,
		FeelsC:   r.Current.Apparent,
		Humidity: int(r.Current.Humidity + 0.5),
		WindKmh:  int(r.Current.Wind + 0.5),
		Code:     r.Current.Code,
	}
	cur.Desc, cur.Category = Describe(r.Current.Code)
	days := make([]Day, 0, len(r.Daily.Time))
	for i := range r.Daily.Time {
		if i >= len(r.Daily.Code) || i >= len(r.Daily.Tmax) {
			break
		}
		d := Day{
			Date: r.Daily.Time[i], Code: r.Daily.Code[i],
			Tmax: r.Daily.Tmax[i], Tmin: r.Daily.Tmin[i],
		}
		d.Desc, _ = Describe(d.Code)
		days = append(days, d)
	}
	return cur, days, nil
}

// ---- WMO weather code → 中文 ----

type codeDesc struct {
	desc string
	cat  string
}

var codeTable = map[int]codeDesc{
	0: {"晴", "sun"}, 1: {"晴间少云", "sun"}, 2: {"局部多云", "cloud"}, 3: {"阴", "cloud"},
	45: {"有雾", "fog"}, 48: {"冻雾", "fog"},
	51: {"毛毛雨", "rain"}, 53: {"毛毛雨", "rain"}, 55: {"浓毛毛雨", "rain"},
	56: {"冻毛毛雨", "rain"}, 57: {"冻毛毛雨", "rain"},
	61: {"小雨", "rain"}, 63: {"中雨", "rain"}, 65: {"大雨", "rain"},
	66: {"冻雨", "rain"}, 67: {"冻雨", "rain"},
	71: {"小雪", "snow"}, 73: {"中雪", "snow"}, 75: {"大雪", "snow"}, 77: {"雪粒", "snow"},
	80: {"小阵雨", "rain"}, 81: {"阵雨", "rain"}, 82: {"强阵雨", "rain"},
	85: {"小阵雪", "snow"}, 86: {"阵雪", "snow"},
	95: {"雷阵雨", "thunder"}, 96: {"雷雨伴冰雹", "thunder"}, 99: {"强雷雨冰雹", "thunder"},
}

// Describe WMO code → 中文描述 + 分类（未知码回落"多云"）。
func Describe(code int) (string, string) {
	if d, ok := codeTable[code]; ok {
		return d.desc, d.cat
	}
	return "多云", "cloud"
}

// TrimCity 去掉定位名里的常见前后缀（"市"），展示更紧凑。
func TrimCity(name string) string {
	return strings.TrimSuffix(strings.TrimSpace(name), "市")
}
