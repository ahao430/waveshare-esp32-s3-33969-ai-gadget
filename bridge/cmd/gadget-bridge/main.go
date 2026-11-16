// gadget-bridge：ESP32 AI Gadget 的 Go Bridge。
// Phase 0：HTTP API + 额度域（Mock + 智谱 C 级 Adapter）。
// 用法：gadget-bridge [-addr :8081]
//
// Provider 装配策略（PLAN §6.12）：
//   - mock 始终注册（开发/演示）。
//   - zhipu 仅在 ZHIPU_API_KEY 存在时注册（C 级接口默认关闭）。
//   - ZHIPU_BASE_URL 可覆盖站点（国内/国际）。
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"log"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"gadget-bridge/internal/ai"
	"gadget-bridge/internal/api"
	museapi "gadget-bridge/internal/api"
	"gadget-bridge/internal/events"
	"gadget-bridge/internal/komari"
	"gadget-bridge/internal/muse"
	"gadget-bridge/internal/quota"
	"gadget-bridge/internal/quota/codingplan"
	"gadget-bridge/internal/quota/deepseek"
	"gadget-bridge/internal/quota/kimi"
	"gadget-bridge/internal/quota/mock"
	"gadget-bridge/internal/quota/newapi"
	"gadget-bridge/internal/quota/opencode"
	"gadget-bridge/internal/quota/zhipu"
	"gadget-bridge/internal/security"
	"gadget-bridge/internal/storage"
	"gadget-bridge/internal/voice"
	"gadget-bridge/internal/weather"
	"gadget-bridge/internal/xiaozhi"
)

func main() {
	addr := flag.String("addr", envOr("BRIDGE_ADDR", ":8081"), "HTTP 监听地址")
	dbPath := flag.String("db", envOr("BRIDGE_DB", "data/gadget.db"), "SQLite 快照缓存路径（空=纯内存）")
	webDir := flag.String("webdir", envOr("BRIDGE_WEBDIR", detectWebDir()),
		"Web 后台静态目录（默认自动探测 webdist/ 或 ../web/dist；空=不托管）")
	flag.Parse()

	logger := slog.New(slog.NewTextHandler(os.Stdout, nil))
	reg := quota.NewRegistry()
	store := quota.NewStore()
	eventHub := events.NewHub()

	var db *storage.DB // 凭据与快照持久层；无 -db 时为 nil（纯内存模式）
	if *dbPath != "" {
		var err error
		db, err = storage.Open(*dbPath)
		if err != nil {
			logger.Error("open db", "err", err)
			os.Exit(1)
		}
		defer db.Close()
		if err := store.AttachPersister(db); err != nil {
			logger.Warn("restore snapshots partial fail", "err", err) // 恢复失败降级为空缓存
		} else {
			logger.Info("snapshot cache restored", "path", *dbPath)
		}
	}

	if err := reg.Register(mock.New()); err != nil {
		logger.Error("register mock", "err", err)
		os.Exit(1)
	}

	// 凭据解析：env 显式配置优先，其次 SQLite 加密凭据（Web 配置）。
	// master key 缺失时 DB 凭据不可用（fail-safe，PLAN §15.3）。
	var masterKey []byte
	if k, err := security.ParseMasterKey(os.Getenv("BRIDGE_MASTER_KEY")); err == nil {
		masterKey = k
		logger.Info("credential store enabled (BRIDGE_MASTER_KEY ok)")
	} else {
		logger.Warn("credential store disabled (set BRIDGE_MASTER_KEY=<64 hex chars>; env keys still work)")
	}

	accounts := map[string][]quota.AccountRef{}
	if key := os.Getenv("ZHIPU_API_KEY"); key != "" {
		accounts["zhipu"] = []quota.AccountRef{{
			ID:          "default",
			DisplayName: os.Getenv("ZHIPU_ACCOUNT_NAME"),
			APIKey:      key, // 只进内存，不进日志
		}}
	}
	if key := os.Getenv("KIMI_API_KEY"); key != "" {
		accounts["kimi"] = []quota.AccountRef{{
			ID:          "default",
			DisplayName: os.Getenv("KIMI_ACCOUNT_NAME"),
			APIKey:      key,
		}}
	}

	resolveAccount := func(providerID string) quota.AccountRef {
		if accs, ok := accounts[providerID]; ok && len(accs) > 0 {
			return accs[0]
		}
		if db != nil && masterKey != nil {
			label, key, err := db.GetCredential(providerID, masterKey)
			if err == nil {
				return quota.AccountRef{ID: "default", DisplayName: label, APIKey: key}
			}
			if !errors.Is(err, storage.ErrNotFound) {
				logger.Warn("read credential", "provider", providerID, "err", err.Error())
			}
		}
		return quota.AccountRef{ID: "default"} // 无凭据：provider 抓取时报 missing_api_key
	}

	// zhipu/kimi 恒注册：账号经 resolver 动态解析（env 或 Web 配置的
	// 加密凭据），保存 Key 后下一次刷新即生效，无需重启。
	zhipuProvider := zhipu.New(os.Getenv("ZHIPU_BASE_URL"))
	if err := reg.Register(zhipuProvider); err != nil {
		logger.Error("register zhipu", "err", err)
		os.Exit(1)
	}
	if err := reg.Register(kimi.New(os.Getenv("KIMI_BASE_URL"))); err != nil {
		logger.Error("register kimi", "err", err)
		os.Exit(1)
	}
	if err := reg.Register(opencode.New(os.Getenv("OPENCODE_BASE_URL"))); err != nil {
		logger.Error("register opencode", "err", err)
		os.Exit(1)
	}
	if err := reg.Register(newapi.New()); err != nil {
		logger.Error("register newapi", "err", err)
		os.Exit(1)
	}
	// Coding Plan 中转订阅（kimi 形状 usages / newapi 形状余额自动探测）
	for kind, name := range map[string]string{
		"gpt": "GPT Coding Plan", "claude": "Claude Coding Plan",
		"minimax": "MiniMax Coding Plan", "mimo": "MiMo Coding Plan",
	} {
		if err := reg.Register(codingplan.New(kind, name)); err != nil {
			logger.Error("register codingplan", "kind", kind, "err", err)
			os.Exit(1)
		}
	}
	// DeepSeek 官方余额
	if err := reg.Register(deepseek.New()); err != nil {
		logger.Error("register deepseek", "err", err)
		os.Exit(1)
	}

	prewarmer := zhipu.NewPrewarmer(zhipuProvider,
		func() quota.AccountRef { return resolveAccount("zhipu") },
		os.Getenv("ZHIPU_PREWARM_MODEL")) // 空=默认 glm-4-flash
	prewarmZhipu := prewarmer.Prewarm

	// Komari 监控源（动态解析，每次抓取现读配置）：Web 后台配置
	// （app_settings.monitor）优先 → 环境变量 KOMARI_URL → 空（不回退
	// mock：未配置就返回空列表，设备/Web 显示空态引导去后台配置）。
	resolveKomari := func() komari.Source {
		if db != nil && masterKey != nil {
			if raw, err := db.GetSetting(storage.SettingKeyMonitor); err == nil {
				var cfg struct {
					BaseURL  string `json:"base_url"`
					EncToken string `json:"enc_token"`
				}
				if json.Unmarshal([]byte(raw), &cfg) == nil && cfg.BaseURL != "" {
					token := ""
					if cfg.EncToken != "" {
						if t, derr := security.Decrypt(cfg.EncToken, masterKey); derr == nil {
							token = t
						}
					}
					return komari.NewClient(cfg.BaseURL, token)
				}
			}
		}
		if u := os.Getenv("KOMARI_URL"); u != "" {
			return komari.NewClient(u, os.Getenv("KOMARI_TOKEN"))
		}
		return nil
	}
	komariSvc := komari.NewService(komari.NewDynamicSource(resolveKomari))
	logger.Info("komari source = dynamic (web config > KOMARI_URL > none)")

	// Bigfish Chat Model Router（PLAN §7.4）：OpenAI 兼容 upstream。
	// 环境变量示例：CHAT_UPSTREAM_ZHIPU="https://open.bigmodel.cn/api/paas/v4,ZHIPU_KEY"
	//               CHAT_UPSTREAM_DEEPSEEK="https://api.deepseek.com,DS_KEY"
	// 无任何配置时自动 mock（默认 provider 也是 mock）。
	var chatUpstreams []ai.Upstream
	for _, spec := range os.Environ() {
		if !strings.HasPrefix(spec, "CHAT_UPSTREAM_") {
			continue
		}
		parts := strings.SplitN(spec, "=", 2)
		vals := strings.SplitN(parts[1], ",", 2)
		if len(vals) != 2 {
			logger.Warn("chat upstream env malformed (want URL,KEY)", "env", parts[0])
			continue
		}
		id := strings.TrimPrefix(parts[0], "CHAT_UPSTREAM_")
		chatUpstreams = append(chatUpstreams, ai.Upstream{
			ID:      strings.ToLower(id),
			BaseURL: strings.TrimRight(vals[0], "/"),
			APIKey:  vals[1], // 只进内存
		})
		logger.Info("chat upstream enabled", "provider", strings.ToLower(id))
	}
	chatDefault := "mock"
	if len(chatUpstreams) > 0 {
		if d := os.Getenv("CHAT_DEFAULT_PROVIDER"); d != "" {
			chatDefault = strings.ToLower(d)
		} else {
			chatDefault = strings.ToLower(chatUpstreams[0].ID)
		}
	}
	if len(chatUpstreams) == 0 {
		logger.Info("chat router = mock only (configure CHAT_UPSTREAM_<ID>=\"URL,KEY\" to enable real providers)")
	}
	chatRouter := ai.NewRouter(chatUpstreams, chatDefault)
	// 会话轮换代持久化（"新对话"后 Bridge 重启不得退回旧代——否则
	// 服务端旧记忆复活）
	if db != nil {
		ai.SetSessionStore(&sessionStoreDB{db: db})
	}

	// 语音引擎：真实 Interactor——智谱 GLM-ASR + ai.Router + Edge-TTS
	//（raw-16k PCM 直出）。ASR Key 动态解析（env/加密凭据，与预热同语义）；
	// Edge-TTS 失效降级哔声（10 分钟冷却自动重试）。MockInteractor 保留
	// 在 voice 包内作为单测与降级实现。
	voiceEng := voice.NewRealInteractor(voice.ASRKeys{
		Zhipu:       func() string { return resolveAccount("zhipu").APIKey },
		Siliconflow: func() string { return resolveAccount("siliconflow").APIKey },
		Minimax:     func() string { return resolveAccount("minimax").APIKey },
		Mimo:        func() string { return resolveAccount("mimo").APIKey },
		Deepseek:    func() string { return resolveAccount("deepseek").APIKey },
		// 火山豆包：凭据库 "AppID:AccessToken"；env 双变量兜底
		Volcengine: func() string {
			if k := resolveAccount("volcengine").APIKey; k != "" {
				return k
			}
			if id, tok := os.Getenv("VOLCENGINE_APP_ID"),
				os.Getenv("VOLCENGINE_ACCESS_TOKEN"); id != "" && tok != "" {
				return id + ":" + tok
			}
			return ""
		},
	}, os.Getenv("ZHIPU_BASE_URL"), chatRouter)
	// TTS 偏好：App 级设置（Muse/小智等）优先，空则回退默认机器人实例
	if db != nil {
		voiceEng.SetTTSPreferenceForApp(func(app string) (string, string) {
			if app != "" && app != "chat" {
				if p, ok := db.GetVoiceApps()[app]; ok {
					if p.Provider != "" || p.Voice != "" {
						return p.Provider, p.Voice
					}
				}
			}
			return db.DefaultBotTTS()
		})
	}
	voiceH := voice.NewHandler(voiceEng)

	// 官方小智管理器（db/masterKey 缺失时保持 nil：不挂端点、不接回合）
	var xiaozhiGlobal *xiaozhi.Manager

	// Muse 云端回合（app=muse）：凭据来自一次性配对后导出的 NVS 值，
	// 加密存 Bridge（Web「模型 API」页可改）。未配置时 Available()=false，
	// Muse 页自动回落常规链路，不会变哑巴。
	if db != nil && masterKey != nil {
		museMgr := &muse.Manager{
			Creds: func() muse.Credentials {
				return museapi.LoadMuseCredentials(db, masterKey)
			},
			Log: log.New(os.Stderr, "muse: ", log.LstdFlags),
		}
		// 环境变量兜底（首次装配/容器场景）：MUSE_ACCESS_TOKEN + MUSE_NODE_ID
		if os.Getenv("MUSE_ACCESS_TOKEN") != "" && os.Getenv("MUSE_NODE_ID") != "" {
			envCreds := muse.Credentials{
				AccessToken:  os.Getenv("MUSE_ACCESS_TOKEN"),
				RefreshToken: os.Getenv("MUSE_REFRESH_TOKEN"),
				NodeID:       os.Getenv("MUSE_NODE_ID"),
				NoiseHost:    os.Getenv("MUSE_NOISE_HOST"),
				APIURL:       os.Getenv("MUSE_API_URL"),
				ProxyURL:     os.Getenv("MUSE_PROXY_URL"),
			}
			base := museMgr.Creds
			museMgr.Creds = func() muse.Credentials {
				if c := base(); c.Ready() {
					return c
				}
				return envCreds
			}
		}
		voiceH.SetMuse(museMgr)
		if museMgr.Available() {
			logger.Info("muse voice enabled")
		}

		// 官方小智回合（app=xiaozhi，ADR-005）：Bridge 模拟设备，凭据/
		// 身份加密存库（Web「小智」页连接/激活）。未绑定或失败自动回落
		// 常规链路。
		xiaozhiGlobal = &xiaozhi.Manager{
			Creds: func() xiaozhi.Credentials {
				return museapi.LoadXiaozhiCreds(db, masterKey)
			},
			SaveCreds: func(c xiaozhi.Credentials) error {
				return museapi.SaveXiaozhiCredsToDB(db, masterKey, c)
			},
			Log: log.New(os.Stderr, "xiaozhi: ", log.LstdFlags),
		}
		voiceH.SetXiaozhi(&xiaozhiTurner{xz: xiaozhiGlobal})
		if xiaozhiGlobal.Available() {
			logger.Info("xiaozhi official voice enabled")
		}
	}

	// 外观（自定义背景）目录：默认 db 同目录 appearance/；-db 空则 data/
	appearanceDir := envOr("BRIDGE_APPEARANCE_DIR", filepath.Join(filepath.Dir(*dbPath), "appearance"))
	if *dbPath == "" {
		appearanceDir = envOr("BRIDGE_APPEARANCE_DIR", "data/appearance")
	}

	// 一次性迁移：旧版把地址/Token 存在 bots 行里——若公用连接还是空，
	// 启动时从旧行搬到 app_settings.astrbot（之后实例更新不再丢凭据）
	if db != nil && masterKey != nil {
		if base, tok := db.GetAstrBotConn(masterKey); base != "" {
			if raw, err := db.GetSetting(storage.SettingKeyAstrBot); err != nil || raw == "" {
				if err := db.SaveAstrBotConn(base, tok, masterKey); err == nil {
					logger.Info("astrbot conn migrated from legacy bot row")
				}
			}
		}
	}

	// 大肥鱼对接 AstrBot 机器人实例：地址/Token 为公用连接
	// （app_settings.astrbot，兼容旧版 bots 表首条），实例只带
	// 名称/配置文件/provider/model。每次请求现读，Web 改动即时生效。
	chatRouter.BotResolver = func() []ai.Upstream {
		if db == nil || masterKey == nil {
			return nil
		}
		baseURL, token := db.GetAstrBotConn(masterKey)
		if baseURL == "" {
			return nil
		}
		list, err := db.ListBots()
		if err != nil {
			return nil
		}
		// 多实例切换：当前机器人排首位——Router.resolve 的空 model 回落
		// 取 bots[0]，设备端不指定实例时就用"当前"这一只。
		if activeID, _, ok := db.ActiveBot(); ok {
			for i, b := range list {
				if b.ID == activeID && i > 0 {
					list[0], list[i] = list[i], list[0]
					break
				}
			}
		}
		var bots []ai.Upstream
		for _, b := range list {
			bots = append(bots, ai.Upstream{
				ID: b.ID, BaseURL: baseURL, APIKey: token, Kind: "astrbot",
				ConfigID: b.ConfigID, Provider: b.Provider, Model: b.Model,
			})
		}
		return bots
	}

	// 天气（Open-Meteo 免费无 Key）：配置每次抓取现读（Web 改城市
	// TTL 内收敛），启动即预热 + 每 10 分钟保温。
	weatherSvc := weather.NewService(weather.NewClient(), func() weather.ResolveConfig {
		cfg := weather.ResolveConfig{Mode: "auto"}
		if db != nil {
			if raw, err := db.GetSetting(storage.SettingKeyWeather); err == nil {
				var c struct {
					Mode string `json:"mode"`
					City string `json:"city"`
				}
				if json.Unmarshal([]byte(raw), &c) == nil && (c.Mode == "auto" || c.Mode == "manual") {
					cfg = weather.ResolveConfig{Mode: c.Mode, City: c.City}
				}
			}
		}
		return cfg
	}, logger)
	weatherSvc.Warm(context.Background())
	// Komari 预热：设备端 HTTP 超时 1.5s，冷缓存时同步抓上游要 1.3~1.5s
	// → 设备永远拿不到数据。启动时后台预热，首个请求即有缓存。
	if komariSvc != nil {
		go komariSvc.Warm(context.Background())
	}

	handler := api.NewHandler(api.Deps{
		Registry:        reg,
		Store:           store,
		Logger:          logger,
		Token:           os.Getenv("BRIDGE_TOKEN"), // 非空即启用 Bearer 认证（PLAN §19）
		Komari:          komariSvc,
		AppearanceDir:   appearanceDir,
		PrewarmZhipu:    prewarmZhipu,
		Events:          eventHub,
		ChatRouter:      chatRouter,
		VoiceHandler:    voiceH,
		Xiaozhi:         xiaozhiGlobal,
		CredentialsDB:   db,
		MasterKey:       masterKey,
		SubscriptionsDB: db,
		Weather:         weatherSvc,
		Accounts: func(id string) []quota.AccountRef {
			acc := resolveAccount(id)
			return []quota.AccountRef{acc}
		},
	})

	logger.Info("gadget-bridge listening", "addr", *addr, "providers", len(reg.List()),
		"webdir", *webDir)
	if err := http.ListenAndServe(*addr, withWebUI(handler, *webDir, logger)); err != nil {
		logger.Error("serve", "err", err)
		os.Exit(1)
	}
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

// xiaozhiTurner 把 *xiaozhi.Manager 适配成 voice.XiaozhiTurner。
type xiaozhiTurner struct{ xz *xiaozhi.Manager }

func (t *xiaozhiTurner) Available() bool { return t.xz.Available() }

func (t *xiaozhiTurner) AskVoice(ctx context.Context, pcm []byte,
	onHeard, onText func(string), onAudioPCM func([]byte)) (string, string, int, error) {
	res, err := t.xz.AskVoice(ctx, pcm, xiaozhi.TurnCallbacks{
		OnHeard: onHeard,
		OnText:  onText,
		OnAudio: func(pcm []int16) {
			if onAudioPCM != nil && len(pcm) > 0 {
				buf := make([]byte, len(pcm)*2)
				for i, v := range pcm {
					buf[2*i] = byte(v)
					buf[2*i+1] = byte(uint16(v) >> 8)
				}
				onAudioPCM(buf)
			}
		},
	})
	return res.Heard, res.Reply, res.AudioBytes, err
}

// sessionStoreDB 把 storage.DB 适配成 ai.SessionStore。
type sessionStoreDB struct{ db *storage.DB }

func (s *sessionStoreDB) LoadSessionGens() (uint64, map[string]uint64, error) {
	g, per := s.db.GetAstrbotSessionGens()
	return g, per, nil
}

func (s *sessionStoreDB) SaveSessionGens(g uint64, per map[string]uint64) error {
	return s.db.SaveAstrbotSessionGens(g, per)
}
