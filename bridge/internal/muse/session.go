// session.go — Noise 会话上的多路复用请求流（Muse 的 "HTTP over Noise"）。
//
// 上游语义（components/muse/muse_chat_session.cpp 头注释）：
//
//	一条连接上并发多条流，每条流用一个 stream_id 标识：
//	  打开：  ServiceFrame{stream_id, request{verb,path,headers,body,end_body}}
//	  上行：  ServiceFrame{stream_id, body_chunk{data, end_body}}
//	  下行：  ServiceFrame{stream_id, response{status,headers}}  ← 服务端受理
//	          ServiceFrame{stream_id, body_chunk{data, end_body}} ← 响应体
//	          或 ServiceFrame{stream_id, reset{code,reason}}
//	取消：  reset
package muse

import (
	"context"
	"fmt"
	"sync"
)

// EventKind 流事件类型。
type EventKind int

const (
	EvStatus EventKind = iota // 服务端受理（含 status/headers）
	EvBody                    // 响应体分片
	EvEnd                     // 响应体结束
	EvReset                   // 服务端重置该流
)

// Event 一条流事件。
type Event struct {
	Kind    EventKind
	Status  int32
	Headers []Header
	Body    []byte
	Reset   int32
	Reason  string
}

// Stream 一条请求流。事件的读取与上行的写入可并发。
type Stream struct {
	ID   int64
	conn *Conn

	mu     sync.Mutex
	events chan Event
	closed bool
	err    error
}

func (c *Conn) allocStream() (*Stream, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	select {
	case <-c.closed:
		// 会话已断：返回终止流而不是让调用方等待
		s := &Stream{ID: 0, conn: c, events: make(chan Event, 1), closed: true, err: fmt.Errorf("muse: 会话已关闭")}
		close(s.events)
		return s, s.err
	default:
	}
	id := c.nextID
	c.nextID++
	s := &Stream{ID: id, conn: c, events: make(chan Event, 64)}
	c.streams[id] = s
	return s, nil
}

// Open 打开一条请求流（body 可为 nil；endBody 表示请求体到此结束）。
func (c *Conn) Open(verb, path string, headers []Header, body []byte, endBody bool) (*Stream, error) {
	s, err := c.allocStream()
	if err != nil {
		return nil, err
	}
	f := &ServiceFrame{
		StreamID: s.ID,
		Kind:     KindRequest,
		Request: &AppRequest{
			Verb:    verb,
			Path:    path,
			Headers: headers,
			Body:    body,
			EndBody: endBody,
		},
	}
	if err := c.sendFrame(f); err != nil {
		s.closeWith(err)
		return nil, err
	}
	return s, nil
}

// Send 追加请求体分片（流式上行）。
func (s *Stream) Send(p []byte) error {
	if len(p) == 0 {
		return nil
	}
	return s.conn.sendFrame(&ServiceFrame{
		StreamID: s.ID,
		Kind:     KindBodyChunk,
		Chunk:    &BodyChunk{Data: p},
	})
}

// Finish 结束请求体（半关闭）。
func (s *Stream) Finish() error {
	return s.conn.sendFrame(&ServiceFrame{
		StreamID: s.ID,
		Kind:     KindBodyChunk,
		Chunk:    &BodyChunk{EndBody: true},
	})
}

// Cancel 中止该流。
func (s *Stream) Cancel(reason string) error {
	err := s.conn.sendFrame(&ServiceFrame{
		StreamID: s.ID,
		Kind:     KindReset,
		Reset:    &Reset{Code: ResetCancelled, Reason: reason},
	})
	s.closeWith(nil)
	return err
}

// Events 事件通道；流结束（end/reset/错误）后关闭。
func (s *Stream) Events() <-chan Event { return s.events }

// Err 流的终止错误（正常结束为 nil）。
func (s *Stream) Err() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.err
}

func (s *Stream) deliver(f *ServiceFrame) {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return
	}
	s.mu.Unlock()

	switch f.Kind {
	case KindResponse:
		s.push(Event{Kind: EvStatus, Status: f.Response.Status, Headers: f.Response.Headers})
		if f.Response.EndBody {
			s.closeWith(nil)
		}
	case KindBodyChunk:
		if len(f.Chunk.Data) > 0 {
			s.push(Event{Kind: EvBody, Body: f.Chunk.Data})
		}
		if f.Chunk.EndBody {
			s.push(Event{Kind: EvEnd})
			s.closeWith(nil)
		}
	case KindReset:
		s.push(Event{Kind: EvReset, Reset: f.Reset.Code, Reason: f.Reset.Reason})
		s.closeWith(fmt.Errorf("muse: 流被重置 code=%d %s", f.Reset.Code, f.Reset.Reason))
	}
}

// push 非阻塞投递；缓冲满说明调用方没读，丢弃最旧事件防止卡住读循环。
func (s *Stream) push(ev Event) {
	select {
	case s.events <- ev:
	default:
		select {
		case <-s.events:
		default:
		}
		select {
		case s.events <- ev:
		default:
		}
	}
}

func (s *Stream) closeWith(err error) {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return
	}
	s.closed = true
	if err != nil {
		s.err = err
	}
	s.mu.Unlock()
	close(s.events)

	s.conn.mu.Lock()
	delete(s.conn.streams, s.ID)
	s.conn.mu.Unlock()
}

// Response 收齐一条流的响应（status/headers/body 全量），带上下文取消。
// 用于非流式调用（chat/stream 的 ack、vms 列表等）。
func (s *Stream) Response(ctx context.Context) (status int32, headers []Header, body []byte, err error) {
	for {
		select {
		case <-ctx.Done():
			_ = s.Cancel("client timeout")
			return 0, nil, nil, ctx.Err()
		case ev, ok := <-s.events:
			if !ok {
				if e := s.Err(); e != nil {
					return status, headers, body, e
				}
				return status, headers, body, nil
			}
			switch ev.Kind {
			case EvStatus:
				status, headers = ev.Status, ev.Headers
			case EvBody:
				body = append(body, ev.Body...)
			case EvEnd:
				return status, headers, body, nil
			case EvReset:
				return status, headers, body, fmt.Errorf("muse: 流重置 code=%d %s", ev.Reset, ev.Reason)
			}
		}
	}
}
