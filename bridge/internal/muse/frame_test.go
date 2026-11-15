// frame_test.go — 字节级一致性：golden 向量由上游 C++ 编解码器生成。
//
// 生成方法（可复现）：用 esp32/components/noise_core 的
// ServiceCodec.cpp/TransportFrameCodec.cpp 编译一个 oracle 程序，编码与
// 下面用例相同的结构并打印十六进制。Go 实现必须逐字节一致——这是
// "我们真的在和 Muse 说同一种话"的最硬证据。
package muse

import (
	"bytes"
	"encoding/hex"
	"testing"
)

func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatalf("bad hex: %v", err)
	}
	return b
}

// TestServiceFrameGolden 对齐上游 EncodeApplicationRequestFrame / 各帧编码。
func TestServiceFrameGolden(t *testing.T) {
	// 上游 oracle（stream 7, POST /chat/stream, 1 header, body, end_body）
	const wantRequest = "080712410a04504f5354120c2f636861742f73747265616d1a200a0c436f6e74656e742d5479706512106170706c69636174696f6e2f6a736f6e22077b2261223a317d2801"
	f := &ServiceFrame{
		StreamID: 7,
		Kind:     KindRequest,
		Request: &AppRequest{
			Verb:    "POST",
			Path:    "/chat/stream",
			Headers: []Header{{"Content-Type", "application/json"}},
			Body:    []byte(`{"a":1}`),
			EndBody: true,
		},
	}
	if got := f.Marshal(); !bytes.Equal(got, mustHex(t, wantRequest)) {
		t.Fatalf("request frame mismatch\n got %x\nwant %s", got, wantRequest)
	}

	// body chunk: stream 3, "abc", end false
	const wantChunk = "080322050a03616263"
	cf := &ServiceFrame{StreamID: 3, Kind: KindBodyChunk,
		Chunk: &BodyChunk{Data: []byte("abc")}}
	if got := cf.Marshal(); !bytes.Equal(got, mustHex(t, wantChunk)) {
		t.Fatalf("body chunk mismatch\n got %x\nwant %s", got, wantChunk)
	}

	// reset: stream 3, code 3 (protocol error), reason boom
	const wantReset = "08032a0808031204626f6f6d"
	rf := &ServiceFrame{StreamID: 3, Kind: KindReset,
		Reset: &Reset{Code: ResetProtocol, Reason: "boom"}}
	if got := rf.Marshal(); !bytes.Equal(got, mustHex(t, wantReset)) {
		t.Fatalf("reset mismatch\n got %x\nwant %s", got, wantReset)
	}

	// transport: chunk 1, index 0, total 1, payload xyz
	const wantTransport = "08011801220378797a"
	tf := &TransportFrame{ChunkID: 1, Index: 0, Total: 1, Payload: []byte("xyz")}
	if got := tf.Marshal(); !bytes.Equal(got, mustHex(t, wantTransport)) {
		t.Fatalf("transport mismatch\n got %x\nwant %s", got, wantTransport)
	}

	// 请求信封：service=Daemon(0) 省略字段1，payload 包在字段2 里
	const wantEnvelope = "1245080712410a04504f5354120c2f636861742f73747265616d1a200a0c436f6e74656e742d5479706512106170706c69636174696f6e2f6a736f6e22077b2261223a317d2801"
	env := MarshalRequestEnvelope(ServiceDaemon, f.Marshal())
	if !bytes.Equal(env, mustHex(t, wantEnvelope)) {
		t.Fatalf("envelope mismatch\n got %x\nwant %s", env, wantEnvelope)
	}
}

// TestServiceFrameRoundTrip 编解码往返（含响应帧与未知字段跳过）。
func TestServiceFrameRoundTrip(t *testing.T) {
	resp := &ServiceFrame{
		StreamID: 42,
		Kind:     KindResponse,
		Response: &AppResponse{
			Status:  200,
			Headers: []Header{{"Content-Type", "application/x-ndjson"}},
			Body:    []byte("hello"),
			EndBody: false,
		},
	}
	got, err := UnmarshalServiceFrame(resp.Marshal())
	if err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if got.StreamID != 42 || got.Kind != KindResponse || got.Response.Status != 200 {
		t.Fatalf("envelope fields: %+v", got)
	}
	if len(got.Response.Headers) != 1 || got.Response.Headers[0].Key != "Content-Type" {
		t.Fatalf("headers: %+v", got.Response.Headers)
	}
	if string(got.Response.Body) != "hello" {
		t.Fatalf("body: %q", got.Response.Body)
	}

	// 未知字段（field 15 varint）应被跳过而不是报错
	raw := append(resp.Marshal(), 0x78, 0x2a) // field 15, wire 0, value 42
	if _, err := UnmarshalServiceFrame(raw); err != nil {
		t.Fatalf("unknown field should be skipped: %v", err)
	}

	// 损坏输入必须报错而不是静默返回空帧
	if _, err := UnmarshalServiceFrame([]byte{0x0a, 0xff}); err == nil {
		t.Fatal("truncated frame should error")
	}
}

// TestTransportFrameValidation 分片约束（index<total、total 非零）。
func TestTransportFrameValidation(t *testing.T) {
	if _, err := UnmarshalTransportFrame([]byte{0x08, 0x01, 0x22, 0x00}); err == nil {
		t.Fatal("missing total_chunks should error")
	}
	bad := &TransportFrame{ChunkID: 1, Index: 2, Total: 2}
	if _, err := UnmarshalTransportFrame(bad.Marshal()); err == nil {
		t.Fatal("index >= total should error")
	}
}

// TestResponseEnvelope 服务端信封解析。
func TestResponseEnvelope(t *testing.T) {
	payload := []byte{0x08, 0x01}
	env := appendBytes(nil, 1, payload, true)
	got, err := UnmarshalResponseEnvelope(env)
	if err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if !bytes.Equal(got, payload) {
		t.Fatalf("payload = %x", got)
	}
}
