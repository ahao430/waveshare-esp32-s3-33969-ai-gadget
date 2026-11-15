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
	"log"
	"os"
	"sync"
	"time"

	"gadget-bridge/internal/ai"
)

// ttsCooldown：Edge-TTS 连续失败后的冷却窗口（非官方通道，失效期
// 常持续数小时；10 分钟平衡恢复灵敏度与无效请求量）
const ttsCooldown = 10 * time.Minute

type RealInteractor struct {
	asr      ASRKeys
	baseURL  string // ZHIPU_BASE_URL（空=国内站）
	router   *ai.Router
	ttsVoice string // Edge-TTS 音色（env EDGE_TTS_VOICE，空=晓晓）

	ttsMu       sync.Mutex
	ttsBrokenAt time.Time // Edge-TTS 冷却（零值=正常）

	dsMu       sync.Mutex
	dsBrokenAt time.Time // DeepSeek TTS 冷却

	// ttsPref 按 App 取 TTS 偏好（引擎 + 音色）；nil=未接入。
	// app="" 或 "chat" → 默认机器人实例；"muse"/"xiaozhi" 等 → App 级配置。
	ttsPref func(app string) (provider, voice string)
}

// SetTTSPreferenceForApp 注入按 App 查询 TTS 偏好的解析器（main 装配）。
// provider="" 表示自动（配了 DeepSeek Key 用 DeepSeek，否则 Edge）。
func (r *RealInteractor) SetTTSPreferenceForApp(fn func(app string) (string, string)) {
	r.ttsPref = fn
}

func (r *RealInteractor) dsCooling() bool {
	r.dsMu.Lock()
	defer r.dsMu.Unlock()
	return time.Since(r.dsBrokenAt) < ttsCooldown
}

func (r *RealInteractor) markDeepSeek(ok bool) {
	r.dsMu.Lock()
	defer r.dsMu.Unlock()
	if ok {
		r.dsBrokenAt = time.Time{}
	} else {
		r.dsBrokenAt = time.Now()
	}
}

// ASRKeys 各路 ASR 的 Key 解析器（动态读凭据库/env，保存即生效）。
// 选择顺序：ASR_PROVIDER 显式指定 > siliconflow > minimax > mimo > zhipu
// （前三个新账号有免费额度/按量便宜；智谱额度常被 coding plan 占满）。
type ASRKeys struct {
	Zhipu       func() string
	Siliconflow func() string
	Minimax     func() string
	Mimo        func() string
	Deepseek    func() string
}

func NewRealInteractor(asr ASRKeys, baseURL string, router *ai.Router) *RealInteractor {
	return &RealInteractor{
		asr:      asr,
		baseURL:  baseURL,
		router:   router,
		ttsVoice: os.Getenv("EDGE_TTS_VOICE"),
	}
}

// pickASR 返回 (provider, key)；都未配置返回空。
func (r *RealInteractor) pickASR() (string, string) {
	type cand struct {
		name string
		fn   func() string
	}
	all := []cand{
		{"siliconflow", r.asr.Siliconflow},
		{"deepseek", r.asr.Deepseek}, // 全家桶：ASR/TTS 同一 Key
		{"minimax", r.asr.Minimax},
		{"mimo", r.asr.Mimo},
		{"zhipu", r.asr.Zhipu},
	}
	if want := os.Getenv("ASR_PROVIDER"); want != "" {
		for _, c := range all {
			if c.name == want && c.fn != nil {
				return c.name, c.fn()
			}
		}
	}
	for _, c := range all {
		if c.fn == nil {
			continue
		}
		if k := c.fn(); k != "" {
			return c.name, k
		}
	}
	return "", ""
}

func (r *RealInteractor) Transcribe(ctx context.Context, pcmL16 []byte) (string, error) {
	if len(pcmL16) == 0 {
		return "", fmt.Errorf("empty_audio")
	}
	provider, key := r.pickASR()
	if provider == "" || key == "" {
		return "", fmt.Errorf("asr: 未配置语音识别 Key（Web 订阅管理 → 服务凭据：硅基流动 / MiniMax / MiMo）")
	}
	var text string
	var err error
	switch provider {
	case "siliconflow":
		text, err = siliconflowTranscribe(ctx, key, "", pcmL16)
	case "minimax":
		text, err = minimaxTranscribe(ctx, key, pcmL16)
	case "mimo":
		text, err = mimoTranscribe(ctx, key, pcmL16)
	case "deepseek":
		text, err = deepseekTranscribe(ctx, key, pcmL16)
	default:
		text, err = zhipuTranscribe(ctx, key, r.baseURL, pcmL16)
	}
	if err != nil {
		return "", fmt.Errorf("asr(%s): %w", provider, err)
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

// RespondStream 流式回复（语音链路）：与 Respond 同一路由，增量回调。
func (r *RealInteractor) RespondStream(ctx context.Context, text string,
	onDelta func(string) error) error {
	if ctx.Err() != nil {
		return ctx.Err()
	}
	if r.router == nil {
		return fmt.Errorf("no_router")
	}
	msgs := []ai.ChatMessage{
		{Role: "system", Content: "你是语音助手。回答保持 1~3 句话，口语化，不用 markdown。"},
		{Role: "user", Content: text},
	}
	return r.router.CompleteStream(ctx, "", msgs, onDelta)
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
	// 引擎顺序：实例偏好（Web 机器人配置）> TTS_PROVIDER env > 自动
	// （配了 DeepSeek Key 用 DeepSeek TTS，否则 Edge）> 哔声占位
	prefProvider, prefVoice := "", ""
	if r.ttsPref != nil {
		prefProvider, prefVoice = r.ttsPref(AppFromContext(ctx))
	}
	if prefProvider == "" {
		prefProvider = os.Getenv("TTS_PROVIDER")
	}
	var order []string
	switch prefProvider {
	case "deepseek":
		order = []string{"deepseek", "edge"}
	case "edge":
		order = []string{"edge"}
	default:
		if r.asr.Deepseek != nil && r.asr.Deepseek() != "" {
			order = []string{"deepseek", "edge"}
		} else {
			order = []string{"edge"}
		}
	}

	for _, eng := range order {
		switch eng {
		case "deepseek":
			key := ""
			if r.asr.Deepseek != nil {
				key = r.asr.Deepseek()
			}
			if key == "" || r.dsCooling() {
				continue
			}
			voiceID := prefVoice
			if voiceID == "" {
				voiceID = deepseekVoiceID()
			}
			pcm, err := deepseekTTS(ctx, key, text, voiceID)
			r.markDeepSeek(err == nil)
			if err == nil {
				return wrapWav(pcm), nil
			}
			log.Printf("voice: tts deepseek failed: %v", err)
		case "edge":
			if r.ttsCooling() {
				continue
			}
			edgeVoice := prefVoice
			if edgeVoice == "" {
				edgeVoice = r.ttsVoice
			}
			pcm, err := edgeSynthesize(ctx, text, edgeVoice)
			r.markTTS(err == nil)
			if err == nil {
				return wrapWav(pcm), nil
			}
			log.Printf("voice: tts edge failed: %v", err)
		}
	}
	return beepWav(text) // 音质降级，会话不断
}
