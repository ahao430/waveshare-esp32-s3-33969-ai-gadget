package komari

import (
	"context"
	"math/rand"
	"time"
)

// MockSource 提供确定性的伪随机服务器队列（开发/演示）。
type MockSource struct{}

func NewMockSource() *MockSource { return &MockSource{} }

func (m *MockSource) Name() string { return "mock" }

type mockServerSpec struct {
	name     string
	baseLoad float64
}

var mockFleet = []mockServerSpec{
	{"hk-01", 22}, {"hk-02", 48}, {"jp-01", 15}, {"us-01", 66}, {"de-01", 35}, {"sg-01", 80},
}

func (m *MockSource) Fetch(ctx context.Context) (*Snapshot, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	seed := time.Now().Unix() / 30 // 30 秒粒度，避免每次剧烈跳变
	servers := make([]Server, 0, len(mockFleet))
	for i, spec := range mockFleet {
		online := (seed+int64(i))%7 != 0 // 偶尔离线一台
		cpu := spec.baseLoad + float64(seed%11) - 5
		if cpu < 1 {
			cpu = 1
		}
		mem := spec.baseLoad + 18 + float64(seed%9)
		disk := float64(28 + (i*13)%55)
		up := int64(120_000 + (seed+int64(i*997))%3_000_000)
		down := int64(500_000 + (seed+int64(i*577))%8_000_000)
		uptime := int64(86400*(3+i) + int(seed%3600))

		s := Server{
			UUID:       fmt_uuid(i),
			Name:       spec.name,
			Online:     online,
			CPUPct:     &cpu,
			MemPct:     &mem,
			DiskPct:    &disk,
			NetUpBps:   &up,
			NetDownBps: &down,
			UptimeS:    &uptime,
			LastReport: time.Now(),
		}
		servers = append(servers, s)
	}
	return &Snapshot{FetchedAt: time.Now(), Source: "mock", Servers: servers}, nil
}

func fmt_uuid(i int) string {
	letters := []byte("0123456789abcdef")
	b := make([]byte, 8)
	src := rand.Int63() //nolint:gosec // mock 数据无需密码学随机
	for j := range b {
		b[j] = letters[(src>>uint(j*4))&0xf]
	}
	return string(b) + "-0" + string(rune('0'+i))
}
