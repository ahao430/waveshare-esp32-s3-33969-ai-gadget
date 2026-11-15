// asr_volcengine.go — 火山引擎（豆包）大模型语音识别，一句话模式。
//
// 官方对"本地音频字节"的一句话识别走 WebSocket 二进制协议（HTTP 版
// auc/flash 只收公网 URL）。用流式输入模式端点——音频全发完出最终结果，
// 准确率比逐字回显的 bigmodel 高，正对设备"按住说一段"的形态：
//
//	wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream
//	Header: X-Api-App-Key: <AppID>  X-Api-Access-Key: <AccessToken>
//	        X-Api-Resource-Id: volc.bigasr.sauc.duration（1.0 时长版，
//	        可用 ASR_VOLC_RESOURCE 换 seedasr 2.0 / 并发版）
//
// 帧格式（与 xiaozhi-esp32 生态一致的 v3 sauc 协议）：
//
//	[u8 0x11][u8 消息类型|标志][u8 序列化|压缩][u8 0]
//	[u32 BE payload 大小][payload]
//	full client request（gzip JSON，含音频元数据）→ audio only × N
//	（gzip PCM，建议 100~200ms/包）→ 空包 + 大小 0xFFFFFFFF 结束。
//	服务端响应 gzip JSON：result[0].text；错误 payload 含 error_msg。
package voice

import (
	"bytes"
	"compress/gzip"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

const volcASRURL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream"

// volcASRWSURL 测试注入用（VOLC_ASR_WS_URL 覆盖端点）。
func volcASRWSURL() string {
	if u := os.Getenv("VOLC_ASR_WS_URL"); u != "" {
		return u
	}
	return volcASRURL
}

// 协议位（官方 v3 二进制协议；volcHeader 里再左移到高 nibble）
const (
	volcMsgFullClient   byte = 0x01 // full client request
	volcMsgAudioOnly    byte = 0x02 // audio only
	volcFlagSeqNeg      byte = 0x02 // 响应：负序列号 = 最后一包
	volcSerGzipJSON     byte = 0x01 // 序列化 = JSON（gzip 压缩）
	volcCompressionGzip byte = 0x01 // 压缩 = gzip
	volcASRTimeout           = 45 * time.Second
)

// volcHeader 组 4 字节协议头（无压缩可用 0，这里固定 gzip）。
func volcHeader(msgType, msgFlags, serialization, compression byte) [4]byte {
	return [4]byte{0x11, msgType<<4 | msgFlags, serialization<<4 | compression, 0x00}
}

// volcGzip 压缩（协议要求 gzip；音频与 JSON 均压）。
func volcGzip(b []byte) ([]byte, error) {
	var buf bytes.Buffer
	zw := gzip.NewWriter(&buf)
	if _, err := zw.Write(b); err != nil {
		return nil, err
	}
	if err := zw.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// volcGunzip 解压。
func volcGunzip(b []byte) ([]byte, error) {
	zr, err := gzip.NewReader(bytes.NewReader(b))
	if err != nil {
		return nil, err
	}
	defer zr.Close()
	return io.ReadAll(io.LimitReader(zr, 8<<20))
}

// volcengineTranscribe PCM L16 16k/mono → 文本。apiKey 格式 "AppID:AccessToken"。
func volcengineTranscribe(ctx context.Context, apiKey string, pcmL16 []byte) (string, error) {
	appID, token, err := volcCreds(apiKey)
	if err != nil {
		return "", err
	}
	if len(pcmL16) == 0 {
		return "", fmt.Errorf("volc asr: empty audio")
	}

	resourceID := os.Getenv("ASR_VOLC_RESOURCE")
	if resourceID == "" {
		resourceID = "volc.bigasr.sauc.duration" // 1.0 时长版（默认开通项）
	}

	cctx, cancel := context.WithTimeout(ctx, volcASRTimeout)
	defer cancel()
	h := http.Header{}
	h.Set("X-Api-App-Key", appID)
	h.Set("X-Api-Access-Key", token)
	h.Set("X-Api-Resource-Id", resourceID)
	h.Set("X-Api-Connect-Id", volcReqID())

	dialer := websocket.Dialer{HandshakeTimeout: 10 * time.Second}
	conn, _, err := dialer.DialContext(cctx, volcASRWSURL(), h)
	if err != nil {
		return "", fmt.Errorf("volc asr dial: %w", err)
	}
	defer conn.Close()

	// 1) full client request：音频元数据 + 识别参数
	meta, err := json.Marshal(map[string]any{
		"user":  map[string]any{"uid": "gadget-bridge"},
		"audio": map[string]any{"format": "pcm", "codec": "raw", "rate": 16000, "bits": 16, "channel": 1},
		"request": map[string]any{
			"model_name":  "bigmodel",
			"enable_punc": true, // 加标点
			"enable_itn":  true, // 口语数字转书面（"一九七零年"→"1970 年"）
			"enable_ddc":  true, // 语义顺滑：删"嗯、那个"等停顿语气词
			"result_type": "single",
		},
	})
	if err != nil {
		return "", err
	}
	if err := volcSend(conn, volcMsgFullClient, 0, volcSerGzipJSON, meta); err != nil {
		return "", fmt.Errorf("volc asr meta: %w", err)
	}

	// 2) audio only：100ms/包（3200B）分片发，末包空载荷 + 大小 -1
	const chunk = 3200
	for off := 0; off < len(pcmL16); off += chunk {
		end := off + chunk
		if end > len(pcmL16) {
			end = len(pcmL16)
		}
		if err := volcSend(conn, volcMsgAudioOnly, 0, 0, pcmL16[off:end]); err != nil {
			return "", fmt.Errorf("volc asr feed: %w", err)
		}
	}
	// 末包：类型 audio only、无序列化无压缩，payload 大小写 0xFFFFFFFF
	lastHdr := volcHeader(volcMsgAudioOnly, 0, 0, 0)
	last := append(lastHdr[:], 0xFF, 0xFF, 0xFF, 0xFF)
	if err := conn.WriteMessage(websocket.BinaryMessage, last); err != nil {
		return "", fmt.Errorf("volc asr last: %w", err)
	}

	// 3) 收响应：负序列号包 = 最终结果
	text := ""
	for {
		mt, data, rerr := conn.ReadMessage()
		if rerr != nil {
			if strings.TrimSpace(text) != "" {
				return strings.TrimSpace(text), nil // 已拿到文本，连接收尾失败不再纠缠
			}
			return "", fmt.Errorf("volc asr read: %w", rerr)
		}
		if mt != websocket.BinaryMessage || len(data) < 8 {
			continue
		}
		msgFlags := data[1] & 0x0F
		serialization := data[2] >> 4
		compression := data[2] & 0x0F
		off := 4
		if msgFlags != 0 { // 0b0001/0b0010 均带 4B 序列号
			off += 4
		}
		if off+4 > len(data) {
			continue
		}
		payload := data[off+4:]
		if serialization != 1 { // 1 = JSON
			continue
		}
		var body []byte
		if len(payload) > 0 {
			if compression == volcCompressionGzip {
				if b, gerr := volcGunzip(payload); gerr == nil {
					body = b
				}
			} else {
				body = payload
			}
		}
		if t, perr := volcExtractText(body); perr == nil && t != "" {
			text = t
		}
		if msgFlags == volcFlagSeqNeg { // 最后一包
			break
		}
	}
	text = strings.TrimSpace(text)
	if text == "" {
		return "", fmt.Errorf("volc asr: no text in result")
	}
	return text, nil
}

// volcSend 组帧并发送（payload 一律 gzip；序列化位随消息类型）。
func volcSend(conn *websocket.Conn, msgType, msgFlags, serialization byte, payload []byte) error {
	zb, err := volcGzip(payload)
	if err != nil {
		return err
	}
	hdr := volcHeader(msgType, msgFlags, serialization, volcCompressionGzip)
	size := make([]byte, 4)
	binary.BigEndian.PutUint32(size, uint32(len(zb)))
	return conn.WriteMessage(websocket.BinaryMessage,
		append(hdr[:], append(size, zb...)...))
}

// volcExtractText 从响应 payload 取 result[0].text；错误形态返回 err。
func volcExtractText(body []byte) (string, error) {
	if len(body) == 0 {
		return "", fmt.Errorf("empty payload")
	}
	var out struct {
		Result []struct {
			Text string `json:"text"`
		} `json:"result"`
		ErrorCode any    `json:"error_code"`
		ErrorMsg  string `json:"error_msg"`
	}
	if err := json.Unmarshal(body, &out); err != nil {
		return "", err
	}
	if out.ErrorMsg != "" || out.ErrorCode != nil {
		return "", fmt.Errorf("volc asr error: %v %s", out.ErrorCode, out.ErrorMsg)
	}
	if len(out.Result) == 0 {
		return "", fmt.Errorf("no result field")
	}
	return out.Result[0].Text, nil
}
