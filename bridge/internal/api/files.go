// files.go — 设备文件互通（2026-11-16，TF 卡文件管理器配套，ADR-010）。
//
// Bridge 侧做一个"设备文件"落地目录（默认 data/device_files/），设备端
// 文件管理器双向同步：
//
//	GET    /api/v1/files          → {files:[{name,size,updated_at}]} 平铺列表
//	POST   /api/v1/files/{name}   → raw body 上传/覆盖（≤8MB，文件名任意
//	                                （除路径分隔符），设备端负责 percent 编码）
//	GET    /api/v1/files/{name}   → 下载（显式 Content-Length，设备端极简
//	                                HTTP 客户端不解 chunked）
//	DELETE /api/v1/files/{name}   → 删除
//
// Web 后台「文件」页复用同一组端点（浏览器原生编码中文名）。
// 安全：文件名清洗（禁 / 与 ..、长度 ≤200、拒控制字符）；目录平铺，
// 无子目录穿越面；响应不含任何凭据。
package api

import (
	"errors"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// maxDeviceFileBytes 单文件上限（设备端 PSRAM 整包缓冲 2MB 水位，
// Bridge 侧留足余量给 Web 上传）。
const maxDeviceFileBytes = 8 << 20

// fileNameOK 校验文件名：平铺、可含中文，禁路径分隔符/控制字符/超长。
func fileNameOK(name string) bool {
	if name == "" || len(name) > 200 {
		return false
	}
	if name == "." || name == ".." || strings.ContainsAny(name, "/\\") {
		return false
	}
	for _, r := range name {
		if r < 0x20 || r == 0x7f {
			return false
		}
	}
	return true
}

func serveFiles(mux *http.ServeMux, d Deps, logger *slog.Logger) {
	dir := d.FilesDir
	if dir == "" {
		return
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		logger.Warn("device files dir", "dir", dir, "err", err)
		return
	}

	mux.HandleFunc("GET /api/v1/files", func(w http.ResponseWriter, r *http.Request) {
		entries, err := os.ReadDir(dir)
		if err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "list_failed", err.Error())
			return
		}
		type fileInfo struct {
			Name      string    `json:"name"`
			Size      int64     `json:"size"`
			UpdatedAt time.Time `json:"updated_at"`
		}
		files := make([]fileInfo, 0, len(entries))
		for _, e := range entries {
			if e.IsDir() || !fileNameOK(e.Name()) {
				continue
			}
			info, err := e.Info()
			if err != nil {
				continue
			}
			files = append(files, fileInfo{Name: e.Name(), Size: info.Size(), UpdatedAt: info.ModTime()})
		}
		sort.Slice(files, func(i, j int) bool { return files[i].Name < files[j].Name })
		writeJSON(w, logger, http.StatusOK, map[string]any{"files": files})
	})

	mux.HandleFunc("POST /api/v1/files/{name}", func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		if !fileNameOK(name) {
			writeErr(w, logger, http.StatusBadRequest, "bad_name", "非法文件名")
			return
		}
		body, err := io.ReadAll(io.LimitReader(r.Body, maxDeviceFileBytes+1))
		if err != nil {
			writeErr(w, logger, http.StatusBadRequest, "read_failed", err.Error())
			return
		}
		if len(body) == 0 {
			writeErr(w, logger, http.StatusBadRequest, "empty", "空文件")
			return
		}
		if len(body) > maxDeviceFileBytes {
			writeErr(w, logger, http.StatusRequestEntityTooLarge, "too_large",
				"单文件上限 8MB")
			return
		}
		if err := os.WriteFile(filepath.Join(dir, name), body, 0o644); err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "save_failed", err.Error())
			return
		}
		logger.Info("device file saved", "name", name, "bytes", len(body))
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true, "size": len(body)})
	})

	mux.HandleFunc("GET /api/v1/files/{name}", func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		if !fileNameOK(name) {
			writeErr(w, logger, http.StatusBadRequest, "bad_name", "非法文件名")
			return
		}
		path := filepath.Join(dir, name)
		info, err := os.Stat(path)
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				writeErr(w, logger, http.StatusNotFound, "not_found", "文件不存在")
				return
			}
			writeErr(w, logger, http.StatusInternalServerError, "stat_failed", err.Error())
			return
		}
		f, err := os.Open(path)
		if err != nil {
			writeErr(w, logger, http.StatusInternalServerError, "open_failed", err.Error())
			return
		}
		defer f.Close()
		w.Header().Set("Content-Type", "application/octet-stream")
		// 显式 Content-Length：设备端极简 HTTP 客户端不解 chunked。
		w.Header().Set("Content-Length", strconv.FormatInt(info.Size(), 10))
		w.WriteHeader(http.StatusOK)
		_, _ = io.Copy(w, f)
	})

	mux.HandleFunc("DELETE /api/v1/files/{name}", func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		if !fileNameOK(name) {
			writeErr(w, logger, http.StatusBadRequest, "bad_name", "非法文件名")
			return
		}
		if err := os.Remove(filepath.Join(dir, name)); err != nil {
			if errors.Is(err, os.ErrNotExist) {
				writeErr(w, logger, http.StatusNotFound, "not_found", "文件不存在")
				return
			}
			writeErr(w, logger, http.StatusInternalServerError, "delete_failed", err.Error())
			return
		}
		logger.Info("device file deleted", "name", name)
		writeJSON(w, logger, http.StatusOK, map[string]any{"ok": true})
	})
}
