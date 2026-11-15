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
	"strings"
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

	volMu       sync.Mutex
	volBrokenAt time.Time // 火山 TTS 冷却

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

func (r *RealInteractor) volCooling() bool {
	r.volMu.Lock()
	defer r.volMu.Unlock()
	return time.Since(r.volBrokenAt) < ttsCooldown
}

func (r *RealInteractor) markVolc(ok bool) {
	r.volMu.Lock()
	defer r.volMu.Unlock()
	if ok {
		r.volBrokenAt = time.Time{}
	} else {
		r.volBrokenAt = time.Now()
	}
}

// volcCredsOK Key 是否形如 "AppID:AccessToken"（自动档判定用）。
func volcCredsOK(key string) bool {
	_, _, err := volcCreds(key)
	return err == nil
}

// ASRKeys 各路 ASR 的 Key 解析器（动态读凭据库/env，保存即生效）。
// 选择顺序：ASR_PROVIDER 显式指定 > siliconflow > volcengine > deepseek
// > minimax > mimo > zhipu（SenseVoice 最便宜；豆包大模型 ASR 准确率
// 更高；智谱额度常被 coding plan 占满）。volcengine Key 格式
// "AppID:AccessToken"，同一应用同时供 TTS 使用。
type ASRKeys struct {
	Zhipu       func() string
	Siliconflow func() string
	Minimax     func() string
	Mimo        func() string
	Deepseek    func() string
	Volcengine  func() string
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
		{"volcengine", r.asr.Volcengine},
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
	case "volcengine":
		text, err = volcengineTranscribe(ctx, key, pcmL16)
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
	case "volcengine":
		order = []string{"volcengine", "edge"}
	case "edge":
		order = []string{"edge"}
	default:
		// 自动模式按音色形态路由：Edge 形态（xxxNeural）直接走 Edge、
		// 火山形态（BV*）直接走火山——不把音色名发错厂家白白失败触发
		// 冷却（Web 自动档下拉里三家音色混排）。无音色偏好时
		// DeepSeek Key > 火山 Key > Edge。
		if strings.Contains(prefVoice, "Neural") {
			order = []string{"edge"}
		} else if strings.HasPrefix(prefVoice, "BV") {
			order = []string{"volcengine"}
		} else if r.asr.Deepseek != nil && r.asr.Deepseek() != "" {
			order = []string{"deepseek", "edge"}
		} else if r.asr.Volcengine != nil && volcCredsOK(r.asr.Volcengine()) {
			order = []string{"volcengine", "edge"}
		} else {
			order = []string{"edge"}
		}
	}

	for _, eng := range order {
		switch eng {
		case "volcengine":
			if r.volCooling() {
				continue
			}
			vkey := ""
			if r.asr.Volcengine != nil {
				vkey = r.asr.Volcengine()
			}
			if !volcCredsOK(vkey) {
				continue
			}
			pcm, err := volcengineTTS(ctx, vkey, text, prefVoice)
			r.markVolc(err == nil)
			if err == nil {
				return wrapWav(pcm), nil
			}
			log.Printf("voice: tts volcengine failed: %v", err)
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
