// real.go — RealInteractor：真实语音引擎组装（PLAN §5.3 Phase 5 收尾）。
//
//	Transcribe = 智谱 GLM-ASR（Key 动态解析：env / Web 加密凭据）
//	Respond    = ai.Router（与文字聊天同一路由，模型可配）
//	Synthesize = Edge-TTS raw-16k PCM（免费通道，可能整体失效）
//
// 降级语义（语音会话尽量不中断）：
//
//	ASR 失败  → 返回错误（设备显示"语音请求失败"——没有文本无法继续）
//	LLM 失败  → echo 兜底（与 Mock 同）
//	TTS 失败  → 哔声占位（文本回复照常显示）并进入 10 分钟冷却，
//	            冷却期直接哔声不撞墙，到期自动重试真实引擎
package voice

import (
	"context"
	"fmt"
	"os"
	"sync"
	"time"

	"gadget-bridge/internal/ai"
)

// ttsCooldown：Edge-TTS 连续失败后的冷却窗口（非官方通道，失效期
// 常持续数小时；10 分钟平衡恢复灵敏度与无效请求量）
const ttsCooldown = 10 * time.Minute

type RealInteractor struct {
	zhipuKey   func() string // 动态解析（保存 Key 后无需重启，与预热同语义）
	siliconKey func() string // 硅基流动 ASR（配置后优先于智谱）
	baseURL    string        // ZHIPU_BASE_URL（空=国内站）
	router     *ai.Router
	ttsVoice   string // Edge-TTS 音色（env EDGE_TTS_VOICE，空=晓晓）

	ttsMu       sync.Mutex
	ttsBrokenAt time.Time // 零值=从未失败；冷却判断 time.Since(brokenAt) < ttsCooldown
}

func NewRealInteractor(zhipuKey, siliconKey func() string, baseURL string,
	router *ai.Router) *RealInteractor {
	return &RealInteractor{
		zhipuKey:   zhipuKey,
		siliconKey: siliconKey,
		baseURL:    baseURL,
		router:     router,
		ttsVoice:   os.Getenv("EDGE_TTS_VOICE"),
	}
}

func (r *RealInteractor) Transcribe(ctx context.Context, pcmL16 []byte) (string, error) {
	if len(pcmL16) == 0 {
		return "", fmt.Errorf("empty_audio")
	}
	// ASR 引擎选择：配置了硅基流动 Key 就用它（新账号免费额度，
	// 智谱余额不足 429 时的替代）；否则走智谱 GLM-ASR
	if r.siliconKey != nil && r.siliconKey() != "" {
		text, err := siliconflowTranscribe(ctx, r.siliconKey(), "", pcmL16)
		if err != nil {
			return "", fmt.Errorf("asr(siliconflow): %w", err)
		}
		return text, nil
	}
	text, err := zhipuTranscribe(ctx, r.zhipuKey(), r.baseURL, pcmL16)
	if err != nil {
		return "", fmt.Errorf("asr: %w", err)
	}
	return text, nil
}

func (r *RealInteractor) Respond(ctx context.Context, text string) (string, error) {
	if ctx.Err() != nil {
		return "", ctx.Err()
	}
	if r.router != nil {
		messages := []ai.ChatMessage{
			{Role: "system", Content: "你是语音助手。回答保持 1~3 句话，口语化，不用 markdown。"},
			{Role: "user", Content: text},
		}
		reply, err := r.router.Complete(ctx, "", messages) // 空 model = 默认 provider
		if err == nil && reply != "" {
			return reply, nil
		}
	}
	// 模型失败回退 echo（语音交互不中断）
	return fmt.Sprintf("[voice] 收到「%s」。模型暂不可用，这是本地回声回复。", text), nil
}

func (r *RealInteractor) ttsCooling() bool {
	r.ttsMu.Lock()
	defer r.ttsMu.Unlock()
	return time.Since(r.ttsBrokenAt) < ttsCooldown
}

func (r *RealInteractor) markTTS(ok bool) {
	r.ttsMu.Lock()
	defer r.ttsMu.Unlock()
	if ok {
		r.ttsBrokenAt = time.Time{}
	} else {
		r.ttsBrokenAt = time.Now()
	}
}

func (r *RealInteractor) Synthesize(ctx context.Context, text string) ([]byte, error) {
	if r.ttsCooling() {
		return beepWav(text)
	}
	pcm, err := edgeSynthesize(ctx, text, r.ttsVoice)
	r.markTTS(err == nil)
	if err != nil {
		return beepWav(text) // 音质降级，会话不断
	}
	return wrapWav(pcm), nil
}
