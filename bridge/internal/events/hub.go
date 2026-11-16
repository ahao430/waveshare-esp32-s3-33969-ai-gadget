// Package events 提供 Bridge 的进程内事件总线（PLAN §15.2 /api/v1/events 的后端）。
// 语义：尽力推送——慢消费者丢消息是可接受的（客户端可随时 HTTP 拉全量），
// 事件只作为"该刷新了"的信号与增量载荷。
package events

import (
	"encoding/json"
	"sync"
)

// Message 是推送给客户端的事件信封。
type Message struct {
	Topic string `json:"topic"`          // 如 "quota.zhipu" / "appearance.background"
	Data  any    `json:"data,omitempty"` // 主题相关载荷
}

// Hub 扇出消息到所有订阅者。
type Hub struct {
	mu   sync.RWMutex
	subs map[chan []byte]struct{}
}

func NewHub() *Hub {
	return &Hub{subs: make(map[chan []byte]struct{})}
}

// Subscribe 返回新订阅通道（缓冲 16）。返回的退订函数幂等。
func (h *Hub) Subscribe() (ch chan []byte, cancel func()) {
	ch = make(chan []byte, 16)
	h.mu.Lock()
	h.subs[ch] = struct{}{}
	h.mu.Unlock()

	cancel = func() {
		h.mu.Lock()
		if _, ok := h.subs[ch]; ok {
			delete(h.subs, ch)
			close(ch)
		}
		h.mu.Unlock()
	}
	return ch, cancel
}

// Publish JSON 序列化后扇出。订阅者缓冲满则丢弃该条（不阻塞发布者）。
func (h *Hub) Publish(topic string, data any) {
	payload, err := json.Marshal(Message{Topic: topic, Data: data})
	if err != nil {
		return
	}
	h.mu.RLock()
	defer h.mu.RUnlock()
	for ch := range h.subs {
		select {
		case ch <- payload:
		default: // 慢消费者：丢事件，客户端可 HTTP 拉全量
		}
	}
}
