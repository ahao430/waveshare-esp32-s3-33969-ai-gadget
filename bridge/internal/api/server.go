// Package api 提供 Bridge 的 HTTP API（PLAN §15.2 子集）。
// 使用标准库 net/http（ADR-002），路由模式匹配要求 Go 1.22+。
package api

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"time"

	"github.com/gorilla/websocket"

	"gadget-bridge/internal/ai"
	"gadget-bridge/internal/events"
	"gadget-bridge/internal/komari"
	"gadget-bridge/internal/quota"
	"gadget-bridge/internal/quota/zhipu"
	"gadget-bridge/internal/storage"
	"gadget-bridge/internal/voice"
	"gadget-bridge/internal/weather"
)

// Deps 是装配 Server 所需依赖。
type Deps struct {
	Registry *quota.Registry
	Store    *quota.Store
	Logger   *slog.Logger
	// Token 非空时启用 Bearer 认证（PLAN §19 Web 管理 API 认证）。
	Token string
	// Komari 为 nil 时不挂监控路由。
	Komari *komari.Service
	// PrewarmZhipu 为 nil（未配 ZHIPU_API_KEY）时预热接口返回 503。
	PrewarmZhipu func(ctx context.Context, idemKey string) (*zhipu.PrewarmResult, error)
	// Events 为 nil 时不挂 /api/v1/events（PLAN §15.2 WebSocket）。
	Events *events.Hub
	// ChatRouter 为 nil 时不挂聊天路由（PLAN §7.4 Model Router）。
	ChatRouter *ai.Router
	// VoiceHandler 为 nil 时不挂语音端点（PLAN §5.3 语音链路）。
	VoiceHandler *voice.Handler
	// Credentials：nil 或 MasterKey nil 时不挂凭据路由（fail-safe）。
	CredentialsDB *storage.DB
	MasterKey     []byte
	// AppearanceDir 非空时挂外观路由（自定义背景，用户 2026-11-14 需求：
	// Web 上传自定义背景图片）。背景存 AppearanceDir/appearance_bg.bin。
	AppearanceDir string
	// SubscriptionsDB 非 nil（且 MasterKey 非 nil）时挂订阅配置路由：
	// Web 后台动态配置订阅实例（zhipu/kimi/opencode/newapi/mock）。
	SubscriptionsDB *storage.DB
	// Weather 非 nil 时挂天气路由（Open-Meteo，免 Key）。
	Weather *weather.Service
	// Accounts 返回某 Provider 的抓取账号；Phase 0 从环境变量来，
	// 后续换成 SQLite 加密配置（PLAN §15.3）。
	Accounts func(providerID string) []quota.AccountRef
}

// fetchTimeout 单次抓取上限。
const fetchTimeout = 15 * time.Second

// maxChatRequestBytes 聊天请求体上限（对话历史可能较长）。
const maxChatRequestBytes = 1 << 20

// NewHandler 构建 API 路由。
func NewHandler(d Deps) http.Handler {
	mux := http.NewServeMux()
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}

	mux.HandleFunc("GET /api/v1/device/status", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, logger, http.StatusOK, map[string]any{
			"service":         "gadget-bridge",
			"status":          "ok",
			"providers_count": len(d.Registry.List()),
			"time":            time.Now().UTC(),
		})
	})

	mux.HandleFunc("GET /api/v1/quota/providers", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, logger, http.StatusOK, map[string]any{"providers": d.Registry.List()})
	})

	mux.HandleFunc("GET /api/v1/quota/{id}/snapshot", func(w http.ResponseWriter, r *http.Request) {
		id := r.PathValue("id")
		p, ok := d.Registry.Get(id)
		if !ok {
			writeErr(w, logger, http.StatusNotFound, "unknown_provider", "provider not found: "+id)
			return
		}
		view := d.Store.Get(id)
		// 冷启动（从未抓取）时立即抓一次；否则返回缓存，由客户端 /refresh 主动更新。
		if view.Snapshot == nil {
			view = fetchAndStore(r.Context(), d, p)
		}
		writeJSON(w, logger, http.StatusOK, view)
	})

	mux.HandleFunc("POST /api/v1/quota/{id}/refresh", func(w http.ResponseWriter, r *http.Request) {
		id := r.PathValue("id")
		p, ok := d.Registry.Get(id)
		if !ok {
			writeErr(w, logger, http.StatusNotFound, "unknown_provider", "provider not found: "+id)
			return
		}
		writeJSON(w, logger, http.StatusOK, fetchAndStore(r.Context(), d, p))
	})

	if d.Komari != nil {
		mux.HandleFunc("GET /api/v1/komari/nodes", func(w http.ResponseWriter, r *http.Request) {
			force := r.URL.Query().Get("refresh") == "1"
			view := d.Komari.Nodes(r.Context(), force)
			writeJSON(w, logger, http.StatusOK, view)
			if d.Events != nil {
				d.Events.Publish("komari.updated", view)
			}
		})
	}

	if d.Events != nil {
		mux.HandleFunc("GET /api/v1/events", serveEventsWS(logger, d.Events))
	}

	// 订阅配置与聚合快照（Web 后台动态配置；空列表 → 设备空态文案）。
	if d.SubscriptionsDB != nil && d.MasterKey != nil {
		serveSubscriptions(mux, d, logger)
		// 大肥鱼机器人（AstrBot 实例，独立于订阅）+ 应用配置 KV
		// （ZCode 远程 / Komari 监控 / 按键映射）——同一 storage.DB。
		serveBots(mux, d, logger)
		serveZcode(mux, d, logger)
		serveMonitorConfig(mux, d, logger)
		serveKeys(mux, d, logger)
		serveWeather(mux, d, logger)
	}

	// 外观：自定义背景上传/下载（Web → Bridge → 设备拉取）。
	if d.AppearanceDir != "" {
		mux.Handle("GET /api/v1/appearance", appearanceInfo(d, logger))
		mux.Handle("GET /api/v1/appearance/theme", appearanceGetTheme(d, logger))
		mux.Handle("POST /api/v1/appearance/theme", appearancePutTheme(d, logger))
		mux.Handle("GET /api/v1/appearance/background", appearanceGetBG(d, logger))
		mux.Handle("POST /api/v1/appearance/background", appearancePutBG(d, logger))
		mux.Handle("DELETE /api/v1/appearance/background", appearanceDelBG(d, logger))
	}

	// Bigfish Chat：OpenAI 兼容透传（model = "<provider>/<model>"）。
	if d.ChatRouter != nil {
		mux.HandleFunc("GET /api/v1/chat/providers", func(w http.ResponseWriter, r *http.Request) {
			ids := d.ChatRouter.Providers()
			if d.SubscriptionsDB != nil && d.MasterKey != nil {
				if bots, err := d.SubscriptionsDB.ListBots(); err == nil {
					for _, b := range bots {
						ids = append(ids, b.ID) // bot 实例 id 作路由前缀
					}
				}
			}
			writeJSON(w, logger, http.StatusOK, map[string]any{"providers": ids})
		})
		mux.HandleFunc("POST /api/v1/chat/completions", func(w http.ResponseWriter, r *http.Request) {
			body, err := io.ReadAll(io.LimitReader(r.Body, maxChatRequestBytes))
			if err != nil {
				writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
				return
			}
			if err := d.ChatRouter.ChatCompletions(w, body, r.Context().Done()); err != nil {
				if errors.Is(err, ai.ErrNoUpstream) {
					writeErr(w, logger, http.StatusBadRequest, "unknown_provider", err.Error())
					return
				}
				writeErr(w, logger, http.StatusBadGateway, "upstream_error", err.Error())
				return
			}
		})

		// 语音一步式端点：PCM → WAV + 文本响应头（PLAN §5.3）。
		if d.VoiceHandler != nil {
			mux.HandleFunc("POST /api/v1/voice/interact", d.VoiceHandler.ServeInteract)
		}
	}

	// 凭据 CRUD（PLAN §15.3）：明文 Key 只进不回出；保存后下一次
	// provider 抓取即热生效（resolver 每次动态解析）。
	if d.CredentialsDB != nil && d.MasterKey != nil {
		db := d.CredentialsDB
		mk := d.MasterKey
		mux.HandleFunc("GET /api/v1/credentials", func(w http.ResponseWriter, r *http.Request) {
			list, err := db.ListCredentials()
			if err != nil {
				writeErr(w, logger, http.StatusInternalServerError, "list_failed", err.Error())
				return
			}
			if list == nil {
				list = []storage.CredentialInfo{}
			}
			writeJSON(w, logger, http.StatusOK, map[string]any{"credentials": list})
		})

		mux.HandleFunc("PUT /api/v1/credentials/{id}", func(w http.ResponseWriter, r *http.Request) {
			id := r.PathValue("id")
			var body struct {
				Label  string `json:"account_label"`
				APIKey string `json:"api_key"`
			}
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.APIKey == "" {
				writeErr(w, logger, http.StatusBadRequest, "bad_request", "want {account_label, api_key}")
				return
			}
			if err := db.SaveCredential(id, body.Label, body.APIKey, mk); err != nil {
				writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
				return
			}
			// 安全：响应不含 Key；日志只记 provider id。
			logger.Info("credential saved", "provider", id)
			writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true})
		})

		mux.HandleFunc("DELETE /api/v1/credentials/{id}", func(w http.ResponseWriter, r *http.Request) {
			id := r.PathValue("id")
			if err := db.DeleteCredential(id); err != nil {
				writeErr(w, logger, http.StatusInternalServerError, "delete_failed", err.Error())
				return
			}
			logger.Info("credential deleted", "provider", id)
			writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true})
		})
	}

	// 智谱预热：必须由用户交互触发（PLAN §5.6/§15.2），客户端生成幂等 Key。
	mux.HandleFunc("POST /api/v1/quota/zhipu/prewarm", func(w http.ResponseWriter, r *http.Request) {
		if d.PrewarmZhipu == nil {
			writeErr(w, logger, http.StatusServiceUnavailable, "provider_disabled",
				"zhipu provider not configured (set ZHIPU_API_KEY)")
			return
		}
		var body struct {
			IdempotencyKey string `json:"idempotency_key"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "invalid json body")
			return
		}
		result, err := d.PrewarmZhipu(r.Context(), body.IdempotencyKey)
		if err != nil {
			writeErr(w, logger, http.StatusBadRequest, "prewarm_failed", err.Error())
			return
		}
		writeJSON(w, logger, http.StatusOK, result)
	})

	return authMiddleware(d.Token, logMiddleware(logger, mux))
}

// authMiddleware：Token 配置后要求所有 /api/v1/* 携带 Bearer Token。
// WebSocket（浏览器无法自定义 header）对 /api/v1/events 额外接受
// ?token= 查询参数——注意：查询串可能被中间代理记录，仅限受控局域网。
func authMiddleware(token string, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if token != "" {
			ok := r.Header.Get("Authorization") == "Bearer "+token
			if !ok && r.URL.Path == "/api/v1/events" {
				ok = r.URL.Query().Get("token") == token
			}
			if !ok {
				writeErr(w, slog.Default(), http.StatusUnauthorized, "unauthorized", "missing or invalid bearer token")
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}

// ---- WebSocket /api/v1/events ----

var wsUpgrader = websocket.Upgrader{
	ReadBufferSize:  512,
	WriteBufferSize: 2048,
	// 认证已在 authMiddleware 完成；Origin 校验对局域网设备/浏览器放宽
	CheckOrigin: func(r *http.Request) bool { return true },
}

const (
	wsPingInterval = 20 * time.Second
	wsWriteWait    = 5 * time.Second
)

// serveEventsWS：升级连接后把 hub 事件推给客户端，直至断开。
// 客户端→服务端消息一律忽略（纯服务端推送语义）。
// 注意：连接被 hijack 后 r.Context() 会在 handler 返回时取消，
// 断开感知必须依赖读循环（读出错即客户端离开）。
func serveEventsWS(logger *slog.Logger, hub *events.Hub) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		conn, err := wsUpgrader.Upgrade(w, r, nil)
		if err != nil {
			logger.Warn("ws upgrade", "err", err)
			return
		}

		ch, cancel := hub.Subscribe()
		go func() {
			defer cancel()
			defer conn.Close()

			// 读循环：感知客户端断开（消息一律丢弃）
			conn.SetReadLimit(512)
			readDone := make(chan struct{})
			go func() {
				defer close(readDone)
				for {
					if _, _, err := conn.ReadMessage(); err != nil {
						return
					}
				}
			}()

			ticker := time.NewTicker(wsPingInterval)
			defer ticker.Stop()
			for {
				select {
				case payload, ok := <-ch:
					if !ok {
						return
					}
					conn.SetWriteDeadline(time.Now().Add(wsWriteWait))
					if err := conn.WriteMessage(websocket.TextMessage, payload); err != nil {
						return
					}
				case <-ticker.C:
					conn.SetWriteDeadline(time.Now().Add(wsWriteWait))
					if err := conn.WriteMessage(websocket.PingMessage, nil); err != nil {
						return
					}
				case <-readDone:
					return
				}
			}
		}()
	}
}

// fetchAndStore 抓取并写入 Store；失败时保留旧数据并标记 stale。
// 成功或失败都推送 quota.<id> 事件（含 stale 视图）。
func fetchAndStore(ctx context.Context, d Deps, p quota.QuotaProvider) quota.StoredSnapshot {
	ctx, cancel := context.WithTimeout(ctx, fetchTimeout)
	defer cancel()

	var account quota.AccountRef
	if d.Accounts != nil {
		accounts := d.Accounts(p.ID())
		if len(accounts) > 0 {
			account = accounts[0]
		}
	}
	account.ID = firstNonEmpty(account.ID, "default")

	snap, err := p.FetchQuota(ctx, account)
	if err != nil {
		// 不把错误细节里的 URL/参数带出去；Provider 内部错误已脱敏为短码。
		d.Store.SaveError(p.ID(), err.Error())
		view := d.Store.Get(p.ID())
		if d.Events != nil {
			d.Events.Publish("quota."+p.ID(), view)
		}
		return view
	}
	d.Store.SaveSuccess(p.ID(), snap)
	view := d.Store.Get(p.ID())
	if d.Events != nil {
		d.Events.Publish("quota."+p.ID(), view)
	}
	return view
}

func writeJSON(w http.ResponseWriter, logger *slog.Logger, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	if err := enc.Encode(v); err != nil {
		logger.Error("write response", "err", err)
	}
}

func writeErr(w http.ResponseWriter, logger *slog.Logger, code int, kind, msg string) {
	writeJSON(w, logger, code, map[string]any{"error": kind, "message": msg})
}

func logMiddleware(logger *slog.Logger, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		// 只记录方法/路径/耗时，不记录 header（避免 Authorization 泄漏，PLAN §6.12）。
		logger.Info("http", "method", r.Method, "path", r.URL.Path, "dur", time.Since(start).String())
	})
}

func firstNonEmpty(vs ...string) string {
	for _, v := range vs {
		if v != "" {
			return v
		}
	}
	return ""
}
