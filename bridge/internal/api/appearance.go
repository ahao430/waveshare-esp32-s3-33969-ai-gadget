// appearance — 自定义背景（用户 2026-11-14 需求：Web 上传自定义背景）。
// 存储：AppearanceDir/appearance_bg.bin（单文件，魔数判 PNG/JPEG）。
// 设备端 GET 后缓存到本地（模拟器 ds_appearance_fetch / 固件 esp_http_client）。
package api

import (
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
)

// maxBackgroundBytes 背景上限（Web 端压到 480×480，实际远小于此）。
const maxBackgroundBytes = 256 * 1024

func bgPath(dir string) string { return filepath.Join(dir, "appearance_bg.bin") }

func bgContentType(data []byte) string {
	if len(data) >= 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' {
		return "image/png"
	}
	if len(data) >= 2 && data[0] == 0xFF && data[1] == 0xD8 {
		return "image/jpeg"
	}
	return ""
}

func appearanceInfo(d Deps, logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		_, err := os.Stat(bgPath(d.AppearanceDir))
		writeJSON(w, logger, http.StatusOK, map[string]bool{
			"background": err == nil,
		})
	}
}

func appearanceGetBG(d Deps, logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		data, err := os.ReadFile(bgPath(d.AppearanceDir))
		if err != nil {
			writeErr(w, logger, http.StatusNotFound, "not_found", "no background uploaded")
			return
		}
		ct := bgContentType(data)
		if ct == "" {
			ct = "application/octet-stream"
		}
		// 显式 Content-Length：避免 Go 对 >2KB 响应启用 chunked（模拟器
		// 极简 HTTP 客户端不解 chunked，字节会混入块长度行）。
		w.Header().Set("Content-Type", ct)
		w.Header().Set("Content-Length", strconv.Itoa(len(data)))
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write(data)
	}
}

func appearancePutBG(d Deps, logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		data, err := io.ReadAll(io.LimitReader(r.Body, maxBackgroundBytes+1))
		if err != nil {
			writeErr(w, logger, http.StatusBadRequest, "bad_request", "read body failed")
			return
		}
		if len(data) > maxBackgroundBytes {
			writeErr(w, logger, http.StatusRequestEntityTooLarge, "too_large",
				"background exceeds 256KB")
			return
		}
		if bgContentType(data) == "" {
			writeErr(w, logger, http.StatusUnsupportedMediaType, "bad_type",
				"want PNG or JPEG")
			return
		}
		if err := os.MkdirAll(d.AppearanceDir, 0o750); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "io_failed", err.Error())
			return
		}
		if err := os.WriteFile(bgPath(d.AppearanceDir), data, 0o640); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "io_failed", err.Error())
			return
		}
		if d.Events != nil {
			d.Events.Publish("appearance.background", map[string]bool{"updated": true})
		}
		logger.Info("appearance background updated", "bytes", len(data))
		writeJSON(w, logger, http.StatusOK, map[string]int{"bytes": len(data)})
	}
}

func appearanceDelBG(d Deps, logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if err := os.Remove(bgPath(d.AppearanceDir)); err != nil && !os.IsNotExist(err) {
			writeErr(w, logger, http.StatusInternalServerError, "io_failed", err.Error())
			return
		}
		logger.Info("appearance background removed")
		writeJSON(w, logger, http.StatusOK, map[string]bool{"deleted": true})
	}
}
