// stream.go — 语音流式响应：边生成边合成边下发。
//
// 背景（2026-11-15 实测）：非流式一次请求 = ASR + 等 LLM 全文 + 全文 TTS，
// 用户要等 26s 才有第一声。流式后：ASR 完就回 200 + transcript 头，
// 之后每凑满一句就 TTS 一句、立刻把 PCM 帧刷给设备播放。
//
// 帧协议（设备 http 层按帧解析，见 simulator/.../data_source_bridge.c）：
//
//	[u8 type][u32 LE len][payload]
//	  type 1 = PCM 16kHz/16bit/mono
//	  type 2 = 本次朗读的最终文本（UTF-8；设备用于字幕/历史）
//	  type 3 = 错误文本（UTF-8；设备显示"语音请求失败"）
//
// 为什么不用裸 PCM + 尾部 header：设备侧没法区分音频与文本边界，
// 也没有可靠的分帧点；显式帧让两端都能确定性解析、且方便以后加事件。
package voice

import (
	"context"
	"encoding/binary"
	"fmt"
	"log"
	"net/http"
	"strings"
	"time"
	"unicode/utf8"

	"gadget-bridge/internal/ai"
)

const (
	framePCM   byte = 1
	frameText  byte = 2
	frameError byte = 3
)

// writeFrame 写一个帧并 flush（device 侧边收边播）。
func writeFrame(w http.ResponseWriter, typ byte, payload []byte) error {
	hdr := make([]byte, 5)
	hdr[0] = typ
	binary.LittleEndian.PutUint32(hdr[1:], uint32(len(payload)))
	if _, err := w.Write(hdr); err != nil {
		return err
	}
	if len(payload) > 0 {
		if _, err := w.Write(payload); err != nil {
			return err
		}
	}
	if f, ok := w.(http.Flusher); ok {
		f.Flush()
	}
	return nil
}

// ServeInteractStream 流式语音交互（?stream=1）。
func (h *Handler) ServeInteractStream(w http.ResponseWriter, r *http.Request, audio []byte, app string) {
	ctx, cancel := context.WithTimeout(r.Context(), 120*time.Second)
	defer cancel()
	ctx = context.WithValue(ctx, ctxKeyVoiceApp{}, app)

	t0 := time.Now()
	text, err := h.eng.Transcribe(ctx, audio)
	if err != nil {
		log.Printf("voice(stream): asr failed after %v: %v",
			time.Since(t0).Round(time.Millisecond), err)
		w.Header().Set("X-Voice-Error", "asr_failed")
		http.Error(w, `{"error":"asr_failed"}`, http.StatusBadGateway)
		return
	}
	tASR := time.Since(t0)

	// 头必须先发：设备拿到 transcript 就能亮字幕；body 全是帧
	w.Header().Set("Content-Type", "application/x-voice-frames")
	w.Header().Set("X-Voice-Transcript", text)
	w.WriteHeader(http.StatusOK)
	if f, ok := w.(http.Flusher); ok {
		f.Flush()
	}

	start := time.Now()
	var spoken strings.Builder
	var pending strings.Builder
	reasoningHold := 0 // 中文正文出现前已扣住的纯 ASCII 字数

	spokenRunes := func() int { return utf8.RuneCountInString(spoken.String()) }

	// speak 把一句送去 TTS 并把 PCM 帧发出去
	speak := func(sentence string) error {
		sentence = strings.TrimSpace(sentence)
		if utf8.RuneCountInString(sentence) < 2 {
			return nil
		}
		pcmWav, perr := h.eng.Synthesize(ctx, sentence)
		if perr != nil {
			return fmt.Errorf("tts: %w", perr)
		}
		pcm := stripWavHeader(pcmWav)
		if len(pcm) == 0 {
			return nil
		}
		if err := writeFrame(w, framePCM, pcm); err != nil {
			return err
		}
		spoken.WriteString(sentence)
		return nil
	}

	// flushSentences 把 pending 里已完整的句子都念掉；返回是否已够长
	flushSentences := func(final bool) (bool, error) {
		for {
			s, rest := splitFirstSentence(pending.String(), final)
			if s == "" {
				break
			}
			pending.Reset()
			pending.WriteString(rest)
			if err := speak(s); err != nil {
				return true, err
			}
			if spokenRunes() >= maxVoiceReplyRunes {
				return true, nil
			}
		}
		return spokenRunes() >= maxVoiceReplyRunes, nil
	}

	emit := func(delta string) error {
		pending.WriteString(delta)
		// 中文正文之前可能有一段英文推理/元叙述：先扣住，等中文出现再丢前缀
		if spokenRunes() == 0 && !hasCJK(pending.String()) {
			reasoningHold = utf8.RuneCountInString(pending.String())
			if reasoningHold < 120 {
				return nil // 继续等中文
			}
			// 确实没有中文（纯英文人设）：照常念，不再扣
		}
		if i := chineseBodyStart(pending.String()); i > 0 {
			trimmed := pending.String()[i:]
			pending.Reset()
			pending.WriteString(trimmed)
		}
		enough, err := flushSentences(false)
		if err != nil {
			return err
		}
		if enough {
			return ai.ErrStreamDone
		}
		return nil
	}

	si, ok := h.eng.(StreamingInteractor)
	if !ok {
		_ = writeFrame(w, frameError, []byte("当前引擎不支持流式"))
		return
	}
	streamErr := si.RespondStream(ctx, text, emit)
	if streamErr != nil && !ai.IsStreamDone(streamErr) {
		// 已经有音频播出去了就不算彻底失败：把错误作为文本帧告诉设备
		log.Printf("voice(stream): llm failed after %v: %v",
			time.Since(start).Round(time.Millisecond), streamErr)
		if spokenRunes() == 0 {
			_ = writeFrame(w, frameError, []byte("模型暂不可用"))
			return
		}
	}
	// 收尾：剩下的半句也念掉
	if _, err := flushSentences(true); err != nil {
		log.Printf("voice(stream): tail tts failed: %v", err)
	}

	out := strings.TrimSpace(spoken.String())
	if out == "" {
		out = "（没有可播报的内容）"
	}
	_ = writeFrame(w, frameText, []byte(out))

	log.Printf("voice(stream): app=%s audio=%dKB asr=%v llm+tts=%v 播报=%d字 total=%v",
		app, len(audio)/1024, tASR.Round(time.Millisecond),
		time.Since(start).Round(time.Millisecond),
		utf8.RuneCountInString(out), time.Since(t0).Round(time.Millisecond))
}

// stripWavHeader 去掉 RIFF/WAVE 头，只留 PCM（各 TTS 引擎输出 44B 头）。
func stripWavHeader(b []byte) []byte {
	if len(b) < 12 || string(b[0:4]) != "RIFF" || string(b[8:12]) != "WAVE" {
		return b // 不是 WAV：原样返回（已是裸 PCM）
	}
	// 找 data chunk
	for i := 12; i+8 <= len(b); {
		id := string(b[i : i+4])
		sz := int(binary.LittleEndian.Uint32(b[i+4 : i+8]))
		off := i + 8
		if id == "data" {
			end := off + sz
			if sz <= 0 || end > len(b) {
				end = len(b)
			}
			return b[off:end]
		}
		i = off + sz + (sz & 1)
	}
	return b
}

// splitFirstSentence 切出第一句（含句末标点）。final=true 时把剩余全部当一句。
func splitFirstSentence(s string, final bool) (sentence, rest string) {
	runes := []rune(s)
	for i, r := range runes {
		switch r {
		case '。', '！', '？', '!', '?', '\n':
			return strings.TrimSpace(string(runes[:i+1])), strings.TrimSpace(string(runes[i+1:]))
		case '，', ',', '、', '；', ';':
			// 逗号也允许切（长句先出声，体验优先），但太短就继续攒
			if i+1 >= 12 {
				return strings.TrimSpace(string(runes[:i+1])), strings.TrimSpace(string(runes[i+1:]))
			}
		}
	}
	if final {
		return strings.TrimSpace(s), ""
	}
	return "", s
}

// hasCJK：是否有汉字（用于判断"英文前言是否结束"）。
func hasCJK(s string) bool {
	for _, r := range s {
		if r >= 0x4E00 && r <= 0x9FFF {
			return true
		}
	}
	return false
}
