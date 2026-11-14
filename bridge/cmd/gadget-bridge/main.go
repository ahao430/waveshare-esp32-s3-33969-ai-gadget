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
	"errors"
	"flag"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"gadget-bridge/internal/ai"
	"gadget-bridge/internal/api"
	"gadget-bridge/internal/events"
	"gadget-bridge/internal/komari"
	"gadget-bridge/internal/quota"
	"gadget-bridge/internal/quota/kimi"
	"gadget-bridge/internal/quota/mock"
	"gadget-bridge/internal/quota/newapi"
	"gadget-bridge/internal/quota/opencode"
	"gadget-bridge/internal/quota/zhipu"
	"gadget-bridge/internal/security"
	"gadget-bridge/internal/storage"
	"gadget-bridge/internal/voice"
)

func main() {
	addr := flag.String("addr", envOr("BRIDGE_ADDR", ":8081"), "HTTP 监听地址")
	dbPath := flag.String("db", envOr("BRIDGE_DB", "data/gadget.db"), "SQLite 快照缓存路径（空=纯内存）")
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

	prewarmer := zhipu.NewPrewarmer(zhipuProvider,
		func() quota.AccountRef { return resolveAccount("zhipu") },
		os.Getenv("ZHIPU_PREWARM_MODEL")) // 空=默认 glm-4-flash
	prewarmZhipu := prewarmer.Prewarm

	// Komari 监控源：KOMARI_URL 配置真实部署（C 级接口）；否则 Mock。
	var komariSvc *komari.Service
	if u := os.Getenv("KOMARI_URL"); u != "" {
		komariSvc = komari.NewService(komari.NewClient(u, os.Getenv("KOMARI_TOKEN")))
		logger.Info("komari source enabled", "url", u)
	} else {
		komariSvc = komari.NewService(komari.NewMockSource())
		logger.Info("komari source = mock (set KOMARI_URL to enable real deployment)")
	}

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

	// 语音引擎：mock Interactor + 注入聊天路由（语音回复 = 模型生成；
	// 模型失败回退本地 echo）。真实 ASR/TTS 引擎按 Interactor 替换点接入。
	voiceMock := voice.NewMockInteractor()
	voiceMock.SetRouter(chatRouter)
	voiceH := voice.NewHandler(voiceMock)

	// 外观（自定义背景）目录：默认 db 同目录 appearance/；-db 空则 data/
	appearanceDir := envOr("BRIDGE_APPEARANCE_DIR", filepath.Join(filepath.Dir(*dbPath), "appearance"))
	if *dbPath == "" {
		appearanceDir = envOr("BRIDGE_APPEARANCE_DIR", "data/appearance")
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
		CredentialsDB:   db,
		MasterKey:       masterKey,
		SubscriptionsDB: db,
		Accounts: func(id string) []quota.AccountRef {
			acc := resolveAccount(id)
			return []quota.AccountRef{acc}
		},
	})

	logger.Info("gadget-bridge listening", "addr", *addr, "providers", len(reg.List()))
	if err := http.ListenAndServe(*addr, handler); err != nil {
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
