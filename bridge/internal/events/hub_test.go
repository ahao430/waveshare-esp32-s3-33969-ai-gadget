package events

import (
	"encoding/json"
	"testing"
	"time"
)

func TestPubSub(t *testing.T) {
	h := NewHub()
	ch, cancel := h.Subscribe()

	h.Publish("quota.zhipu", map[string]int{"used": 42})

	select {
	case raw := <-ch:
		var m Message
		if err := json.Unmarshal(raw, &m); err != nil {
			t.Fatalf("unmarshal: %v", err)
		}
		if m.Topic != "quota.zhipu" {
			t.Fatalf("topic = %s", m.Topic)
		}
		var payload struct{ Used int }
		b, _ := json.Marshal(m.Data)
		json.Unmarshal(b, &payload)
		if payload.Used != 42 {
			t.Fatalf("data lost: %v", m.Data)
		}
	case <-time.After(time.Second):
		t.Fatal("事件未收到")
	}

	cancel()
	cancel() // 幂等

	// 退订后不再收到（channel 已关闭：读到零值 + ok=false）
	h.Publish("t", nil)
	select {
	case raw, ok := <-ch:
		if ok {
			t.Fatalf("退订后仍收到事件: %s", raw)
		} // ok=false = 已关闭，符合预期
	case <-time.After(100 * time.Millisecond):
		t.Fatal("退订应关闭 channel")
	}
}

func TestSlowConsumerDoesNotBlock(t *testing.T) {
	h := NewHub()
	ch, _ := h.Subscribe() // 不消费

	done := make(chan struct{})
	go func() {
		defer close(done)
		for i := 0; i < 100; i++ {
			h.Publish("t", i) // 缓冲 16，超出部分应被丢弃而非阻塞
		}
	}()

	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("慢消费者阻塞了发布者")
	}
	_ = ch
}
