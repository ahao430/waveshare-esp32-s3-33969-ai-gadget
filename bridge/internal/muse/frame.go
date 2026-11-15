// frame.go — Muse Noise 会话的应用层帧协议（protobuf wire 格式）。
//
// 上游是手写 protobuf（esp32/components/noise_core/src/ServiceCodec.cpp
// 与 TransportFrameCodec.cpp，无 .proto 文件）。字段号照抄源码头：
//
//	ServiceFrame:      1=stream_id(varint) 2=request 3=response 4=body_chunk 5=reset
//	AppRequest:        1=verb 2=path 3=headers[] 4=body 5=end_body
//	AppResponse:       1=status(varint) 2=headers[] 3=body 4=end_body
//	BodyChunk:         1=data 2=end_body
//	Reset:             1=code(varint) 2=reason
//	Header:            1=key 2=value
//	ServiceRequest:    1=service(varint) 2=payload      （客户端→服务端信封）
//	ServiceResponse:   1=payload                       （服务端→客户端信封）
//	TransportFrame:    1=chunk_id 2=chunk_index 3=total_chunks 4=payload
//
// 层序：Noise 明文 = TransportFrame（>64KB 时按 chunk 分片，接收端重组），
// 其 payload = ServiceFrame；请求方向外面再套 ServiceRequest 信封。
//
// 字节级正确性由 frame_test.go 的 golden 向量锁定——那些十六进制来自
// 用上游 C++ ServiceCodec 编译的 oracle 程序（见提交信息）。
package muse

import (
	"fmt"
)

// ServiceType 信封里的服务号（上游 ServiceCodec.h ServiceType）。
const (
	ServiceDaemon   = 0
	ServiceSentinel = 1
	ServiceVault    = 2
	ServiceAuthd    = 3
)

// ResetCode 流重置原因（上游 ServiceCodec.h ResetCode）。
const (
	ResetUnspecified = 0
	ResetCancelled   = 1
	ResetTimeout     = 2
	ResetProtocol    = 3
	ResetRefused     = 4
	ResetInternal    = 5
	ResetUnavailable = 6
)

// FrameKind ServiceFrame 的 oneof 分支。
type FrameKind int

const (
	KindNone FrameKind = iota
	KindRequest
	KindResponse
	KindBodyChunk
	KindReset
)

func (k FrameKind) String() string {
	switch k {
	case KindRequest:
		return "request"
	case KindResponse:
		return "response"
	case KindBodyChunk:
		return "body_chunk"
	case KindReset:
		return "reset"
	}
	return "none"
}

// Header 一对 HTTP 风格的键值。
type Header struct{ Key, Value string }

// AppRequest 打开一个流（verb+path+headers+body）。
type AppRequest struct {
	Verb    string
	Path    string
	Headers []Header
	Body    []byte
	EndBody bool
}

// AppResponse 流被服务端接受（status+headers）或带 body 的响应帧。
type AppResponse struct {
	Status  int32
	Headers []Header
	Body    []byte
	EndBody bool
}

// BodyChunk 流上的后续数据分片。
type BodyChunk struct {
	Data    []byte
	EndBody bool
}

// Reset 中止一个流。
type Reset struct {
	Code   int32
	Reason string
}

// ServiceFrame 一条应用层消息（对应一个流的某一步）。
type ServiceFrame struct {
	StreamID int64
	Kind     FrameKind
	Request  *AppRequest
	Response *AppResponse
	Chunk    *BodyChunk
	Reset    *Reset
}

// ---- protobuf wire 原语 ----

func appendVarint(b []byte, v uint64) []byte {
	for v >= 0x80 {
		b = append(b, byte(v)|0x80)
		v >>= 7
	}
	return append(b, byte(v))
}

func appendTag(b []byte, field int, wire int) []byte {
	return appendVarint(b, uint64(field)<<3|uint64(wire))
}

// appendUint 写 varint 字段；proto3 惯例：零值省略。
func appendUint(b []byte, field int, v uint64, includeZero bool) []byte {
	if v == 0 && !includeZero {
		return b
	}
	return appendVarint(appendTag(b, field, 0), v)
}

func appendBool(b []byte, field int, v bool) []byte {
	if !v {
		return b
	}
	return appendVarint(appendTag(b, field, 0), 1)
}

func appendBytes(b []byte, field int, v []byte, includeEmpty bool) []byte {
	if len(v) == 0 && !includeEmpty {
		return b
	}
	b = appendTag(b, field, 2)
	b = appendVarint(b, uint64(len(v)))
	return append(b, v...)
}

func appendString(b []byte, field int, v string, includeEmpty bool) []byte {
	if v == "" && !includeEmpty {
		return b
	}
	return appendBytes(b, field, []byte(v), true)
}

func appendMessage(b []byte, field int, msg []byte) []byte {
	return appendBytes(b, field, msg, true)
}

// parser 最小 protobuf 读取器。
type parser struct {
	b   []byte
	err error
}

func (p *parser) varint() uint64 {
	if p.err != nil {
		return 0
	}
	var v uint64
	var shift uint
	// 注意：不要边读边把 p.b 前移再用下标访问（会跳字节）；用局部 i，
	// 读完整个 varint 后一次性前进。
	for i := 0; i < len(p.b); i++ {
		c := p.b[i]
		v |= uint64(c&0x7f) << shift
		if c < 0x80 {
			p.b = p.b[i+1:]
			return v
		}
		shift += 7
		if shift > 63 {
			break
		}
	}
	p.err = fmt.Errorf("muse: truncated varint")
	p.b = nil
	return 0
}

func (p *parser) bytes() []byte {
	n := int(p.varint())
	if p.err != nil {
		return nil
	}
	if n < 0 || n > len(p.b) {
		p.err = fmt.Errorf("muse: length %d out of range (have %d)", n, len(p.b))
		return nil
	}
	v := p.b[:n]
	p.b = p.b[n:]
	return v
}

// skip 跳过未知字段（保持前向兼容）。
func (p *parser) skip(wire int) {
	switch wire {
	case 0:
		p.varint()
	case 1:
		if len(p.b) < 8 {
			p.err = fmt.Errorf("muse: short fixed64")
			return
		}
		p.b = p.b[8:]
	case 2:
		p.bytes()
	case 5:
		if len(p.b) < 4 {
			p.err = fmt.Errorf("muse: short fixed32")
			return
		}
		p.b = p.b[4:]
	default:
		p.err = fmt.Errorf("muse: unsupported wire type %d", wire)
	}
}

// next 返回 (字段号, wire 类型)。缓冲区读完返回 (0,0) 且不置错误——
// 正常结束与"截断的 varint"要分清，否则每个合法帧都会被判成损坏。
func (p *parser) next() (int, int) {
	if p.err != nil || len(p.b) == 0 {
		return 0, 0
	}
	key := p.varint()
	if p.err != nil {
		return 0, 0
	}
	return int(key >> 3), int(key & 7)
}

// ---- ServiceFrame 编解码 ----

// Marshal 编码一条 ServiceFrame。
func (f *ServiceFrame) Marshal() []byte {
	var out []byte
	out = appendUint(out, 1, uint64(f.StreamID), false)
	switch f.Kind {
	case KindRequest:
		out = appendMessage(out, 2, f.Request.marshal())
	case KindResponse:
		out = appendMessage(out, 3, f.Response.marshal())
	case KindBodyChunk:
		out = appendMessage(out, 4, f.Chunk.marshal())
	case KindReset:
		out = appendMessage(out, 5, f.Reset.marshal())
	}
	return out
}

func (r *AppRequest) marshal() []byte {
	var out []byte
	out = appendString(out, 1, r.Verb, true)
	out = appendString(out, 2, r.Path, true)
	for _, h := range r.Headers {
		out = appendMessage(out, 3, h.marshal())
	}
	out = appendBytes(out, 4, r.Body, false)
	out = appendBool(out, 5, r.EndBody)
	return out
}

func (h Header) marshal() []byte {
	var out []byte
	out = appendString(out, 1, h.Key, true)
	out = appendString(out, 2, h.Value, true)
	return out
}

func (r *AppResponse) marshal() []byte {
	var out []byte
	out = appendUint(out, 1, uint64(uint32(r.Status)), false)
	for _, h := range r.Headers {
		out = appendMessage(out, 2, h.marshal())
	}
	out = appendBytes(out, 3, r.Body, false)
	out = appendBool(out, 4, r.EndBody)
	return out
}

func (c *BodyChunk) marshal() []byte {
	var out []byte
	out = appendBytes(out, 1, c.Data, false)
	out = appendBool(out, 2, c.EndBody)
	return out
}

func (r *Reset) marshal() []byte {
	var out []byte
	out = appendUint(out, 1, uint64(uint32(r.Code)), false)
	out = appendString(out, 2, r.Reason, false)
	return out
}

// UnmarshalServiceFrame 解析一条 ServiceFrame（未知字段跳过）。
func UnmarshalServiceFrame(b []byte) (*ServiceFrame, error) {
	p := &parser{b: b}
	f := &ServiceFrame{}
	for {
		field, wire := p.next()
		if p.err != nil || field == 0 {
			break
		}
		switch {
		case field == 1 && wire == 0:
			f.StreamID = int64(p.varint())
		case field == 2 && wire == 2:
			req, err := unmarshalAppRequest(p.bytes())
			if err != nil {
				return nil, err
			}
			f.Kind, f.Request = KindRequest, req
		case field == 3 && wire == 2:
			resp, err := unmarshalAppResponse(p.bytes())
			if err != nil {
				return nil, err
			}
			f.Kind, f.Response = KindResponse, resp
		case field == 4 && wire == 2:
			ch, err := unmarshalBodyChunk(p.bytes())
			if err != nil {
				return nil, err
			}
			f.Kind, f.Chunk = KindBodyChunk, ch
		case field == 5 && wire == 2:
			rs, err := unmarshalReset(p.bytes())
			if err != nil {
				return nil, err
			}
			f.Kind, f.Reset = KindReset, rs
		default:
			p.skip(wire)
		}
	}
	if p.err != nil {
		return nil, p.err
	}
	return f, nil
}

func unmarshalHeaders(p *parser, field int, into *[]Header) error {
	if field != 3 && field != 2 {
		p.skip(2)
		return nil
	}
	h, err := unmarshalHeader(p.bytes())
	if err != nil {
		return err
	}
	*into = append(*into, h)
	return nil
}

func unmarshalAppRequest(b []byte) (*AppRequest, error) {
	p := &parser{b: b}
	r := &AppRequest{}
	for {
		field, wire := p.next()
		if p.err != nil || field == 0 {
			break
		}
		switch {
		case field == 1 && wire == 2:
			r.Verb = string(p.bytes())
		case field == 2 && wire == 2:
			r.Path = string(p.bytes())
		case field == 3 && wire == 2:
			if err := unmarshalHeaders(p, 3, &r.Headers); err != nil {
				return nil, err
			}
		case field == 4 && wire == 2:
			r.Body = append([]byte(nil), p.bytes()...)
		case field == 5 && wire == 0:
			r.EndBody = p.varint() != 0
		default:
			p.skip(wire)
		}
	}
	return r, p.err
}

func unmarshalHeader(b []byte) (Header, error) {
	p := &parser{b: b}
	var h Header
	for {
		field, wire := p.next()
		if p.err != nil || field == 0 {
			break
		}
		switch {
		case field == 1 && wire == 2:
			h.Key = string(p.bytes())
		case field == 2 && wire == 2:
			h.Value = string(p.bytes())
		default:
			p.skip(wire)
		}
	}
	return h, p.err
}

func unmarshalAppResponse(b []byte) (*AppResponse, error) {
	p := &parser{b: b}
	r := &AppResponse{}
	for {
		field, wire := p.next()
		if p.err != nil || field == 0 {
			break
		}
		switch {
		case field == 1 && wire == 0:
			r.Status = int32(p.varint())
		case field == 2 && wire == 2:
			if err := unmarshalHeaders(p, 2, &r.Headers); err != nil {
				return nil, err
			}
		case field == 3 && wire == 2:
			r.Body = append([]byte(nil), p.bytes()...)
		case field == 4 && wire == 0:
			r.EndBody = p.varint() != 0
		default:
			p.skip(wire)
		}
	}
	return r, p.err
}

func unmarshalBodyChunk(b []byte) (*BodyChunk, error) {
	p := &parser{b: b}
	c := &BodyChunk{}
	for {
		field, wire := p.next()
		if p.err != nil || field == 0 {
			break
		}
		switch {
		case field == 1 && wire == 2:
			c.Data = append([]byte(nil), p.bytes()...)
		case field == 2 && wire == 0:
			c.EndBody = p.varint() != 0
		default:
			p.skip(wire)
		}
	}
	return c, p.err
}

func unmarshalReset(b []byte) (*Reset, error) {
	p := &parser{b: b}
	r := &Reset{}
	for {
		field, wire := p.next()
		if p.err != nil || field == 0 {
			break
		}
		switch {
		case field == 1 && wire == 0:
			r.Code = int32(p.varint())
		case field == 2 && wire == 2:
			r.Reason = string(p.bytes())
		default:
			p.skip(wire)
		}
	}
	return r, p.err
}

// ---- 信封 ----

// MarshalRequestEnvelope 包一层 ServiceRequest（客户端→服务端）。
func MarshalRequestEnvelope(service int, payload []byte) []byte {
	var out []byte
	out = appendUint(out, 1, uint64(service), false) // Daemon=0 省略，与上游一致
	out = appendBytes(out, 2, payload, true)
	return out
}

// UnmarshalResponseEnvelope 解 ServiceResponse 信封，返回 payload。
func UnmarshalResponseEnvelope(b []byte) ([]byte, error) {
	p := &parser{b: b}
	var payload []byte
	for {
		field, wire := p.next()
		if p.err != nil || field == 0 {
			break
		}
		if field == 1 && wire == 2 {
			payload = append([]byte(nil), p.bytes()...)
		} else {
			p.skip(wire)
		}
	}
	return payload, p.err
}

// ---- TransportFrame ----

// TransportFrame Noise 明文的一帧；消息 >64KB 时按 chunk 分片。
type TransportFrame struct {
	ChunkID uint64
	Index   uint32
	Total   uint32
	Payload []byte
}

// Marshal 编码 TransportFrame。
func (t *TransportFrame) Marshal() []byte {
	var out []byte
	out = appendUint(out, 1, t.ChunkID, false)
	out = appendUint(out, 2, uint64(t.Index), false)
	out = appendUint(out, 3, uint64(t.Total), false)
	out = appendBytes(out, 4, t.Payload, false)
	return out
}

// UnmarshalTransportFrame 解析 TransportFrame。
func UnmarshalTransportFrame(b []byte) (*TransportFrame, error) {
	p := &parser{b: b}
	t := &TransportFrame{}
	for {
		field, wire := p.next()
		if p.err != nil || field == 0 {
			break
		}
		switch {
		case field == 1 && wire == 0:
			t.ChunkID = p.varint()
		case field == 2 && wire == 0:
			t.Index = uint32(p.varint())
		case field == 3 && wire == 0:
			t.Total = uint32(p.varint())
		case field == 4 && wire == 2:
			t.Payload = append([]byte(nil), p.bytes()...)
		default:
			p.skip(wire)
		}
	}
	if p.err != nil {
		return nil, p.err
	}
	if t.Total == 0 {
		return nil, fmt.Errorf("muse: transport frame missing total_chunks")
	}
	if t.Index >= t.Total {
		return nil, fmt.Errorf("muse: chunk index %d >= total %d", t.Index, t.Total)
	}
	return t, nil
}
