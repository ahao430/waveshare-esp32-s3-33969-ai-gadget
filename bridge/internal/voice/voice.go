// Package voice 实现语音交互端点（PLAN §5.3 语音链路 / §11）：
//
//	POST /api/v1/voice/interact
//	  body  = 原始 PCM（audio/l16;rate=16000 单声道，设备端录音）
//	  响应  = WAV 音频（TTS 合成，audio/wav）
//	  X-Voice-Transcript = ASR 文本
//	  X-Voice-Reply      = LLM 回复文本
//
// 一步式协议：设备端录音整段上传，一次往返拿可播放音频 + 展示文本。
// ASR/TTS 当前为 stub（确定性伪结果，协议/数据流真实），真实引擎
// （Whisper/云端 ASR、TTS 服务）按 PLAN Phase 5/6 接入 —— 实现时只替换
// Interactor 内部两函数，端点与设备端零改动。
package voice

import (
	"context"
	"encoding/binary"
	"fmt"
	"log"
	"math"
	"net/http"
	"sync/atomic"
	"time"
	"unicode/utf8"

	"gadget-bridge/internal/ai"
)

// 音频约定（PLAN §11.1：16kHz 16bit 单声道）。
const (
	sampleRate    = 16000
	maxAudioBytes = 1 << 20 // 1MB ≈ 32s@16k16bit 单声道
)

// Interactor 是语音引擎抽象（mock 实现；真实 ASR/LLM/TTS 替换点）。
// StreamingInteractor 可选能力：边生成边回调文本增量（语音流式链路）。
type StreamingInteractor interface {
	RespondStream(ctx context.Context, text string, onDelta func(string) error) error
}

type Interactor interface {
	// Transcribe 语音→文本。
	Transcribe(ctx context.Context, pcmL16 []byte) (string, error)
	// Respond 文本→回复文本（MVP mock echo；TODO 接 internal/ai Router）。
	Respond(ctx context.Context, text string) (string, error)
	// Synthesize 回复文本→WAV 音频。
	Synthesize(ctx context.Context, text string) ([]byte, error)
}

// MockInteractor 确定性伪引擎：转写按请求轮换固定话术；回复经
// ai.Router 生成（SetRouter 注入；未注入时本地 echo 兜底）；TTS 生成
// 按字符时长的哔声 WAV（协议真实、声音是占位）。
type MockInteractor struct {
	counter atomic.Int64
	router  *ai.Router // 可选：非 nil 时 Respond 走模型
}

func NewMockInteractor() *MockInteractor { return &MockInteractor{} }

// SetRouter 注入模型路由（main 装配；语音回复=模型生成）。
func (m *MockInteractor) SetRouter(r *ai.Router) { m.router = r }

var stubUtterances = []string{
	"现在几点了",
	"今天天气怎么样",
	"给我讲个笑话",
	"明天提醒我开会",
	"播放一首歌",
}

func (m *MockInteractor) Transcribe(ctx context.Context, pcmL16 []byte) (string, error) {
	if len(pcmL16) == 0 {
		return "", fmt.Errorf("empty_audio")
	}
	if err := ctx.Err(); err != nil {
		return "", err
	}
	i := m.counter.Add(1)
	return stubUtterances[(int(i)-1)%len(stubUtterances)], nil
}

func (m *MockInteractor) Respond(ctx context.Context, text string) (string, error) {
	if err := ctx.Err(); err != nil {
		return "", err
	}
	if m.router != nil {
		// 语音回复要 TTS 朗读：约束口语化短句（真实 provider 同样受益）
		messages := []ai.ChatMessage{
			{Role: "system", Content: "你是语音助手。回答保持 1~3 句话，口语化，不用 markdown。"},
			{Role: "user", Content: text},
		}
		reply, err := m.router.Complete(ctx, "", messages) // 空 model = 默认 provider
		if err == nil && reply != "" {
			return reply, nil
		}
		// 模型失败回退 echo（语音交互不中断）
	}
	return fmt.Sprintf("[voice-mock] 收到「%s」。语音链路（ASR→LLM→TTS）已打通，这是 Bridge 生成的回复。", text), nil
}

func (m *MockInteractor) Synthesize(ctx context.Context, text string) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return beepWav(text)
}

// beepWav 占位 TTS：每字符 120ms 440Hz 短音（字符间 40ms 间隙），
// 首尾 100ms 静音。Mock 直接用；RealInteractor 在 Edge-TTS 失败/
// 冷却时降级使用（文本回复不受影响）。
func beepWav(text string) ([]byte, error) {
	const (
		charDurMs  = 120
		gapMs      = 40
		marginMs   = 100
		toneFreqHz = 440.0
	)
	chars := len([]rune(text))
	if chars == 0 {
		chars = 1
	}
	totalMs := marginMs*2 + chars*(charDurMs+gapMs)
	samples := totalMs * sampleRate / 1000

	pcm := make([]int16, samples)
	pos := marginMs * sampleRate / 1000
	for range []rune(text) {
		dur := charDurMs * sampleRate / 1000
		for i := 0; i < dur && pos < len(pcm); i++ {
			t := float64(i) / sampleRate
			env := 0.6 * math.Sin(math.Pi*float64(i)/float64(dur)) // 简单包络
			v := env * math.Sin(2*math.Pi*toneFreqHz*t) * 30000
			pcm[pos] = int16(v)
			pos++
		}
		pos += gapMs * sampleRate / 1000
	}
	return wrapWav(pcm), nil
}

// wrapWav 打包 16kHz/16bit/mono PCM 为 WAV（44 字节标准头）。
func wrapWav(pcm []int16) []byte {
	dataLen := len(pcm) * 2
	buf := make([]byte, 44+dataLen)
	copy(buf[0:], "RIFF")
	binary.LittleEndian.PutUint32(buf[4:], uint32(36+dataLen))
	copy(buf[8:], "WAVE")
	copy(buf[12:], "fmt ")
	binary.LittleEndian.PutUint32(buf[16:], 16)           // fmt chunk 大小
	binary.LittleEndian.PutUint16(buf[20:], 1)            // PCM
	binary.LittleEndian.PutUint16(buf[22:], 1)            // mono
	binary.LittleEndian.PutUint32(buf[24:], sampleRate)   // 16k
	binary.LittleEndian.PutUint32(buf[28:], sampleRate*2) // byte rate
	binary.LittleEndian.PutUint16(buf[32:], 2)            // block align
	binary.LittleEndian.PutUint16(buf[34:], 16)           // bits
	copy(buf[36:], "data")
	binary.LittleEndian.PutUint32(buf[40:], uint32(dataLen))
	for i, v := range pcm {
		binary.LittleEndian.PutUint16(buf[44+i*2:], uint16(v))
	}
	return buf
}

// Handler 提供 HTTP 端点处理（api 包装配）。
type Handler struct {
	eng Interactor
}

func NewHandler(eng Interactor) *Handler { return &Handler{eng: eng} }

// ServeInteract 处理 POST /api/v1/voice/interact。
// ctxKeyVoiceApp 承载调用方 App 标识（chat/xiaozhi/muse/zcode），供 TTS
// 选择音色时使用。
type ctxKeyVoiceApp struct{}

func AppFromContext(ctx context.Context) string {
	if v, ok := ctx.Value(ctxKeyVoiceApp{}).(string); ok {
		return v
	}
	return ""
}

func (h *Handler) ServeInteract(w http.ResponseWriter, r *http.Request) {
	audio := make([]byte, 0, 4096)
	buf := make([]byte, 8192)
	for {
		n, err := r.Body.Read(buf)
		if n > 0 {
			if len(audio)+n > maxAudioBytes {
				http.Error(w, `{"error":"audio_too_large"}`, http.StatusBadRequest)
				return
			}
			audio = append(audio, buf[:n]...)
		}
		if err != nil {
			break // EOF
		}
	}
	if len(audio) == 0 {
		http.Error(w, `{"error":"empty_audio"}`, http.StatusBadRequest)
		return
	}

	// App 标识：查询参数 ?app= 或请求头 X-Voice-App（设备端传的是前者）
	app := r.URL.Query().Get("app")
	if app == "" {
		app = r.Header.Get("X-Voice-App")
	}
	// 流式：设备 ?stream=1 → 帧协议 + 边生成边合成（首声更早）
	if r.URL.Query().Get("stream") == "1" {
		h.ServeInteractStream(w, r, audio, app)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 60*time.Second)
	defer cancel()
	ctx = context.WithValue(ctx, ctxKeyVoiceApp{}, app)

	// 分段耗时：语音链路"慢"必须能看出卡在 ASR / LLM / TTS 哪一段
	t0 := time.Now()
	text, err := h.eng.Transcribe(ctx, audio)
	if err != nil {
		log.Printf("voice: asr failed after %v: %v", time.Since(t0).Round(time.Millisecond), err)
		w.Header().Set("X-Voice-Error", "asr_failed")
		http.Error(w, `{"error":"asr_failed"}`, http.StatusBadGateway)
		return
	}
	tASR := time.Since(t0)

	// 没听清拦截：ASR 无有效内容就不再打 LLM/TTS（省时省钱），
	// 直接回提示文本 + 空 WAV（设备端播放空音频即秒结束，字幕照常）
	if !speechValid(text) {
		log.Printf("voice: empty transcript after %v, skip llm/tts",
			tASR.Round(time.Millisecond))
		w.Header().Set("Content-Type", "audio/wav")
		w.Header().Set("X-Voice-Transcript", text)
		w.Header().Set("X-Voice-Reply", "没听清，再按住说一次试试。")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write(wrapWavBytes(nil))
		return
	}

	t1 := time.Now()
	reply, err := h.eng.Respond(ctx, text)
	if err != nil {
		log.Printf("voice: llm failed after %v: %v", time.Since(t1).Round(time.Millisecond), err)
		w.Header().Set("X-Voice-Error", "llm_failed")
		http.Error(w, `{"error":"llm_failed"}`, http.StatusBadGateway)
		return
	}
	tLLM := time.Since(t1)
	// 语音场景清洗：丢掉英文推理前言、按句截断（TTS 只念中文正文）
	rawRunes := utf8.RuneCountInString(reply)
	reply = sanitizeVoiceReply(reply)

	// 朗读版再去掉括号描写/推理句（头里的 reply 是展示版，含括号）
	ttsText := ttsSpeakText(reply)
	if ttsText == "" {
		ttsText = truncateSentences(reply, maxVoiceReplyRunes) // 全被过滤：退回原文
	}
	t2 := time.Now()
	wav, err := h.eng.Synthesize(ctx, ttsText)
	if err != nil {
		log.Printf("voice: tts failed after %v: %v", time.Since(t2).Round(time.Millisecond), err)
		w.Header().Set("X-Voice-Error", "tts_failed")
		http.Error(w, `{"error":"tts_failed"}`, http.StatusBadGateway)
		return
	}
	tTTS := time.Since(t2)
	// 不记文本内容（隐私）；只记长度，便于判断是否"回复太长导致 TTS 慢"
	log.Printf("voice: app=%s audio=%dKB asr=%v(%d字) llm=%v(%d字→清洗后%d字) tts=%v(%dKB) total=%v",
		app, len(audio)/1024,
		tASR.Round(time.Millisecond), utf8.RuneCountInString(text),
		tLLM.Round(time.Millisecond), rawRunes, utf8.RuneCountInString(reply),
		tTTS.Round(time.Millisecond), len(wav)/1024,
		time.Since(t0).Round(time.Millisecond))

	w.Header().Set("Content-Type", "audio/wav")
	w.Header().Set("X-Voice-Transcript", text)
	w.Header().Set("X-Voice-Reply", reply)
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(wav)
}
