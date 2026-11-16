// webui.go — Web 后台静态托管（ADR-009 部署形态：一个 Bridge 进程 = API +
// 管理后台，设备扫码即用，无需另跑 Vite/静态服务器）。
//
// 路由规则：
//   - /api/* 仍走原 API mux
//   - 其余路径：文件存在 → 直接返回；不存在 → SPA fallback 到 index.html
//     （vue-router history 模式刷新不 404）
//   - 全部 no-store：后台是配置界面，过期缓存只会制造困惑
package main

import (
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

// detectWebDir 自动探测 Web 构建产物目录（零配置托管）：
//   部署目录：./webdist（包内同级）；开发目录：../web/dist（cwd=bridge）
// 都没命中返回空串 = 不托管（-webdir / BRIDGE_WEBDIR 可显式指定）。
func detectWebDir() string {
	for _, cand := range []string{"webdist", "../web/dist", "web/dist"} {
		if st, err := os.Stat(cand); err == nil && st.IsDir() {
			return cand
		}
	}
	return ""
}

func withWebUI(next http.Handler, dir string, logger *slog.Logger) http.Handler {
	if dir == "" {
		return next
	}
	st, err := os.Stat(dir)
	if err != nil || !st.IsDir() {
		logger.Warn("webdir not a directory, web UI disabled", "dir", dir)
		return next
	}
	fs := http.FileServer(http.Dir(dir))
	logger.Info("web UI enabled", "dir", dir)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p := r.URL.Path
		if p == "/api" || strings.HasPrefix(p, "/api/") {
			next.ServeHTTP(w, r)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		clean := filepath.Clean("/" + p)
		if _, err := os.Stat(filepath.Join(dir, clean)); err == nil {
			fs.ServeHTTP(w, r)
			return
		}
		// SPA fallback：前端路由路径（/xiaozhi、/voice…）回 index.html
		if p == "/" || !strings.Contains(filepath.Base(clean), ".") {
			http.ServeFile(w, r, filepath.Join(dir, "index.html"))
			return
		}
		http.NotFound(w, r) // 带扩展名但不存在的资源（如旧版 hash 文件）
	})
}
