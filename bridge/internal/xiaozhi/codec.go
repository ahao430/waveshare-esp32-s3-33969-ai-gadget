// codec.go — Opus 编解码薄封装（libopus via github.com/hraban/opus）。
//
// 上行：16k/16bit/mono PCM → 60ms/960 样本 Opus 包（AppVoIP）。
// 下行：官方 TTS 通常 24k 编码——libopus 解码器可按请求采样率输出
// （内部重采样），统一解到 16k 与设备约定一致，无需手动 resample。
package xiaozhi

import (
	"fmt"

	"github.com/hraban/opus"
)

const (
	opusSampleRate = 16000
	opusChannels   = 1
	opusFrame      = 60 * opusSampleRate / 1000 // 960 样本
	opusMaxBuf     = 1275                        // RFC 6716 单包上限
)

// Encoder 流式上行编码器（非并发安全；会话内单发送协程使用）。
type Encoder struct {
	enc *opus.Encoder
	buf []byte
}

func NewEncoder() (*Encoder, error) {
	e, err := opus.NewEncoder(opusSampleRate, opusChannels, opus.AppVoIP)
	if err != nil {
		return nil, fmt.Errorf("opus encoder: %w", err)
	}
	return &Encoder{enc: e}, nil
}

// Encode 一帧 16k mono int16 → Opus 包（复用内部缓冲，返回切片只在下次
// 调用前有效）。
func (e *Encoder) Encode(pcm []int16) ([]byte, error) {
	if len(pcm) != opusFrame {
		return nil, fmt.Errorf("opus: want %d samples, got %d", opusFrame, len(pcm))
	}
	if e.buf == nil {
		e.buf = make([]byte, opusMaxBuf)
	}
	n, err := e.enc.Encode(pcm, e.buf)
	if err != nil {
		return nil, fmt.Errorf("opus encode: %w", err)
	}
	return e.buf[:n], nil
}

// Decoder 流式下行解码器（非并发安全；会话内单读取协程使用）。
type Decoder struct {
	dec *opus.Decoder
	buf []int16
}

func NewDecoder() (*Decoder, error) {
	d, err := opus.NewDecoder(opusSampleRate, opusChannels)
	if err != nil {
		return nil, fmt.Errorf("opus decoder: %w", err)
	}
	return &Decoder{dec: d}, nil
}

// Decode 一个 Opus 包 → 16k mono int16（帧长按包头自描述，上限 120ms）。
func (d *Decoder) Decode(packet []byte) ([]int16, error) {
	if d.buf == nil {
		d.buf = make([]int16, 120*opusSampleRate/1000)
	}
	n, err := d.dec.Decode(packet, d.buf)
	if err != nil {
		return nil, fmt.Errorf("opus decode: %w", err)
	}
	return d.buf[:n], nil
}
