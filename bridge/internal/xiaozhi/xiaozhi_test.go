// xiaozhi_test.go — 离线单测：OTA 报文/响应解析、编解码往返、身份稳定性。
// 真网冒烟走 cmd/xiaozhi-probe（需用户在控制台输激活码，无法 CI 化）。
package xiaozhi

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestBuildOTARequestFields(t *testing.T) {
	s := buildOTARequest("02:11:22:33:44:55", "uuid-1")
	var raw map[string]any
	if err := json.Unmarshal([]byte(s), &raw); err != nil {
		t.Fatalf("json: %v", err)
	}
	if raw["version"].(float64) != 2 {
		t.Fatalf("version = %v", raw["version"])
	}
	if raw["mac_address"] != "02:11:22:33:44:55" || raw["uuid"] != "uuid-1" {
		t.Fatalf("identity fields: %v/%v", raw["mac_address"], raw["uuid"])
	}
	// 不 advertise MCP：服务端会按声明下发 mcp 初始化
	feats := raw["protocol"].(map[string]any)["features"].(map[string]any)
	if feats["mcp"] != false {
		t.Fatalf("mcp must be false: %v", feats)
	}
}

func TestOTAResponseParse(t *testing.T) {
	// 真实官方响应结构（2026-01 抓包样例，XIAOZHI_PROTOCOL §5.1）
	var out otaResponse
	s := `{"mqtt":{"endpoint":"mqtt.xiaozhi.me"},"websocket":{"url":"wss://api.tenclass.net/xiaozhi/v1/","token":"tk"},"server_time":{"timestamp":1768454488884},"activation":{"code":"539000","message":"xiaozhi.me\n539000","challenge":"c"}}`
	if err := json.Unmarshal([]byte(s), &out); err != nil {
		t.Fatalf("json: %v", err)
	}
	if out.Websocket.URL == "" || out.Websocket.Token != "tk" {
		t.Fatalf("websocket fields: %+v", out.Websocket)
	}
	if out.Activation == nil || out.Activation.Code != "539000" {
		t.Fatalf("activation: %+v", out.Activation)
	}

	// 绑定后：无 activation 块
	var bound otaResponse
	if err := json.Unmarshal([]byte(`{"websocket":{"url":"wss://x/","token":"t2"}}`), &bound); err != nil {
		t.Fatal(err)
	}
	if bound.Activation != nil {
		t.Fatalf("bound response must have no activation")
	}
}

func TestOpusRoundTrip(t *testing.T) {
	enc, err := NewEncoder()
	if err != nil {
		t.Skipf("libopus unavailable: %v", err)
	}
	dec, err := NewDecoder()
	if err != nil {
		t.Skipf("libopus unavailable: %v", err)
	}
	// 440Hz 正弦 3 帧（60ms × 3）
	frame := make([]int16, opusFrame)
	for i := range frame {
		frame[i] = int16(12000 * sin01(float64(i) / float64(opusSampleRate)))
	}
	var gotFrames int
	for range 3 {
		packet, err := enc.Encode(frame)
		if err != nil {
			t.Fatalf("encode: %v", err)
		}
		if len(packet) == 0 || len(packet) > opusMaxBuf {
			t.Fatalf("packet size %d", len(packet))
		}
		pcm, err := dec.Decode(packet)
		if err != nil {
			t.Fatalf("decode: %v", err)
		}
		if len(pcm) != opusFrame {
			t.Fatalf("decoded %d samples, want %d", len(pcm), opusFrame)
		}
		gotFrames++
	}
	if gotFrames != 3 {
		t.Fatalf("frames = %d", gotFrames)
	}
}

func TestNewDeviceIDStableFormat(t *testing.T) {
	id := NewDeviceID()
	if !strings.HasPrefix(id, "02:") || len(id) != 17 {
		t.Fatalf("device id format: %q", id)
	}
	if id == NewDeviceID() {
		t.Fatal("device id must be random")
	}
	u := newUUID()
	if len(u) != 36 || u[8] != '-' || u[13] != '-' || u[18] != '-' || u[23] != '-' {
		t.Fatalf("uuid format: %q", u)
	}
}

func sin01(x float64) float64 {
	// 440Hz @16k
	const freq = 440.0
	s := 2 * 3.141592653589793 * freq * x
	// 简易正弦（避免引 math 到测试外的依赖差异——math 本包可用，直接算了）
	return sinImpl(s)
}

func sinImpl(x float64) float64 {
	// 泰勒展开足够测试精度
	x = x - float64(int(x/(2*3.141592653589793)))*(2*3.141592653589793)
	if x > 3.141592653589793 {
		x -= 2 * 3.141592653589793
	}
	if x < -3.141592653589793 {
		x += 2 * 3.141592653589793
	}
	x2 := x * x
	return x * (1 - x2/6*(1-x2/20*(1-x2/42)))
}
