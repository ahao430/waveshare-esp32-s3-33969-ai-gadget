// edgetts.go — 自研最小 Edge-TTS WebSocket 客户端（PLAN §5.3 TTS 引擎）。
//
// 为什么不用社区库：输出格式 raw-16khz-16bit-mono-pcm 在现成 Go 库中要么
// 硬编码 MP3（difyz9）、要么走旧端点被限流（andresayac）——设备端约定
// 16k/16bit/mono PCM，raw 直出可零转码。协议要素（参考 edge-tts 社区
// 实现与 difyz9/edge-tts-go v0.0.3）：
//  1. wss://api.msedgeservices.com/tts/cognitiveservices/websocket/v1
//     + Ocp-Apim-Subscription-Key + Sec-MS-GEC + Sec-MS-GEC-Version + ConnectionId
//  2. Sec-MS-GEC = UPPER( SHA256( ticks + TrustedClientToken ) )，
//     ticks = (unix + 1601 纪元偏移) 向下取整 300s × 1e7（int64 精确运算，
//     勿用 float——1.3e17 量级超 float64 精度）
//  3. 发 speech.config（outputFormat 指定 raw-16khz-16bit-mono-pcm）
//  4. 发 SSML 请求；收文本帧（turn.end 收尾）与二进制帧
//     （前 2 字节 header 长度 + Path:audio 头 + 音频体）
//
// 非官方免费通道：可能整体失效（429/403），调用方需有降级路径
// （voice.RealInteractor 降级哔声）。
package voice

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/gorilla/websocket"
	mp3 "github.com/hajimehoshi/go-mp3"
)

const (
	edgeBaseURL       = "api.msedgeservices.com/tts/cognitiveservices"
	edgeTrustedToken  = "6A5AA1D4EAFF4E9FB37E23D68491D6F4"
	edgeChromiumVer   = "140.0.3485.14"
	edgeChromiumMajor = "140"
	edgeWinEpoch      = 11644473600 // 1601-01-01 UTC 相对 1970 的秒数
	edgeDefaultVoice  = "zh-CN-XiaoxiaoNeural"
	// 免费通道格式白名单极窄（实测拒 raw/riff PCM、48kHz MP3；
	// 仅 24k MP3 ×2 比率 + webm-opus，2026-11-15 真网验证）。
	// 用 24kHz MP3：go-mp3 对 MPEG-2 LSF 每帧双倍输出 → edgeMp3ToPCM16k
	// 按 2×srcRate 补偿重采样（时长/过零率双验证）。
	edgeMP3Format      = "audio-24khz-48kbitrate-mono-mp3"
	edgeReceiveTimeout = 60 * time.Second
)

// edgeSecMSGEC 生成 DRM token（整数运算，与 Python edge-tts 一致）。
func edgeSecMSGEC(now int64) string {
	ticks := now + edgeWinEpoch
	ticks -= ticks % 300 // 向下取整 5 分钟窗口
	ticks *= 10000000    // 秒 → 100ns
	sum := sha256.Sum256([]byte(strconv.FormatInt(ticks, 10) + edgeTrustedToken))
	return strings.ToUpper(hex.EncodeToString(sum[:]))
}

// edgeConnectionID 生成 32 hex 连接 ID（无横线 UUID 形态即可）。
func edgeConnectionID() string {
	var b [16]byte
	_, _ = rand.Read(b[:])
	return hex.EncodeToString(b[:])
}

func edgeHeaders() http.Header {
	ua := "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" +
		edgeChromiumMajor + ".0.0.0 Safari/537.36 Edg/" + edgeChromiumMajor + ".0.0.0"
	h := http.Header{}
	h.Set("User-Agent", ua)
	h.Set("Origin", "chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold")
	h.Set("Accept-Encoding", "gzip, deflate, br")
	h.Set("Accept-Language", "en-US,en;q=0.9")
	return h
}

func edgeWSSURL(now int64) string {
	return "wss://" + edgeBaseURL + "/websocket/v1" +
		"?Ocp-Apim-Subscription-Key=" + edgeTrustedToken +
		"&Sec-MS-GEC=" + edgeSecMSGEC(now) +
		"&Sec-MS-GEC-Version=1-" + edgeChromiumVer +
		"&ConnectionId=" + edgeConnectionID()
}

func edgeJSDate(t time.Time) string {
	// JS 风格 UTC 时间串（服务端仅作记录，格式宽松）
	return t.UTC().Format("Mon Jan 02 2006 15:04:05 GMT+0000 (Coordinated Universal Time)")
}

// edgeSynthesize 合成 text（voice 空=default）为 16k/16bit/mono PCM。
// 每次合成新建连接（语音低频场景，简单优先）。内部走 MP3 + 转码
// （免费通道不支持 raw PCM 直出，见 edgeMP3Format 注释）。
func edgeSynthesize(ctx context.Context, text, voiceName string) ([]int16, error) {
	return edgeSynthesizeFmt(ctx, text, voiceName, edgeMP3Format)
}

// edgeSynthesizeFmt 任意输出格式（探针/降级 MP3 用）；raw 直出失败时
// 调用方回退 MP3 格式 + Bridge 端转码（见 real.go）。
func edgeSynthesizeFmt(ctx context.Context, text, voiceName, format string) ([]int16, error) {
	if voiceName == "" {
		voiceName = edgeDefaultVoice
	}
	text = edgeSanitize(text)
	if strings.TrimSpace(text) == "" {
		text = "。" // 空文本至少合成一声停顿，保持有音频返回
	}

	dialer := websocket.Dialer{
		Proxy:             http.ProxyFromEnvironment,
		HandshakeTimeout:  10 * time.Second,
		EnableCompression: true,
	}

	// 403 重试一次（时钟偏移校正：按服务器 Date 头平移后再生成 token）
	now := time.Now().Unix()
	conn, resp, err := dialer.DialContext(ctx, edgeWSSURL(now), edgeHeaders())
	if err != nil && resp != nil && resp.StatusCode == http.StatusForbidden {
		if d := resp.Header.Get("Date"); d != "" {
			if st, perr := http.ParseTime(d); perr == nil {
				now = st.Unix()
			}
		}
		conn, resp, err = dialer.DialContext(ctx, edgeWSSURL(now), edgeHeaders())
	}
	if err != nil {
		if resp != nil {
			return nil, fmt.Errorf("edge-tts dial: %w (http %d)", err, resp.StatusCode)
		}
		return nil, fmt.Errorf("edge-tts dial: %w", err)
	}
	defer conn.Close()
	_ = conn.SetReadDeadline(time.Now().Add(edgeReceiveTimeout))

	// 1) speech.config：输出格式 raw 16k PCM（不要边界元数据）
	cfg := "X-Timestamp:" + edgeJSDate(time.Now()) + "\r\n" +
		"Content-Type:application/json; charset=utf-8\r\n" +
		"Path:speech.config\r\n\r\n" +
		`{"context":{"synthesis":{"audio":{"metadataoptions":` +
		`{"sentenceBoundaryEnabled":"false","wordBoundaryEnabled":"false"},` +
		`"outputFormat":"` + format + `"}}}}`
	if err := conn.WriteMessage(websocket.TextMessage, []byte(cfg)); err != nil {
		return nil, fmt.Errorf("edge-tts speech.config: %w", err)
	}

	// 2) SSML 请求
	ssml := "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='zh-CN'>" +
		"<voice name='" + voiceName + "'>" + text + "</voice></speak>"
	req := "X-RequestId:" + edgeConnectionID() + "\r\n" +
		"Content-Type:application/ssml+xml\r\n" +
		"X-Timestamp:" + edgeJSDate(time.Now()) + "Z\r\n" + // 尾缀 Z 为服务端历史 bug，保留
		"Path:ssml\r\n\r\n" + ssml
	if err := conn.WriteMessage(websocket.TextMessage, []byte(req)); err != nil {
		return nil, fmt.Errorf("edge-tts ssml: %w", err)
	}

	// 3) 收帧：二进制=音频，文本 turn.end=结束
	var pcm []byte
	for {
		mt, data, rerr := conn.ReadMessage()
		if rerr != nil {
			return nil, fmt.Errorf("edge-tts read: %w", rerr)
		}
		switch mt {
		case websocket.TextMessage:
			path := edgeTextPath(data)
			switch path {
			case "turn.end":
				if len(pcm) == 0 {
					return nil, fmt.Errorf("edge-tts: no audio in turn")
				}
				if format == edgeMP3Format {
					return edgeMp3ToPCM16k(pcm)
				}
				// 其他格式按小端 PCM 切片（len 奇数尾部丢弃，防御性）
				n := len(pcm) / 2
				out := make([]int16, n)
				for i := 0; i < n; i++ {
					out[i] = int16(binary.LittleEndian.Uint16(pcm[i*2:]))
				}
				return out, nil
			case "audio.metadata", "turn.start", "response":
				// 预期内的控制帧
			default:
				return nil, fmt.Errorf("edge-tts: unknown path %q", path)
			}
		case websocket.BinaryMessage:
			// 头 2 字节 header 长度；校验 Path:audio 后收音频体
			if len(data) < 2 {
				continue
			}
			hl := int(binary.BigEndian.Uint16(data[:2]))
			if hl+2 > len(data) {
				continue
			}
			if !strings.Contains(string(data[2:2+hl]), "Path:audio") {
				continue
			}
			pcm = append(pcm, data[2+hl:]...)
		}
		if ctx.Err() != nil {
			return nil, ctx.Err()
		}
	}
}

// edgeTextPath 从文本帧 header 区取 Path: 值（找不到返回空串）。
func edgeTextPath(data []byte) string {
	head := data
	if i := bytesIndex(data, "\r\n\r\n"); i >= 0 {
		head = data[:i]
	}
	for _, line := range strings.Split(string(head), "\r\n") {
		if v, ok := strings.CutPrefix(line, "Path:"); ok {
			return strings.TrimSpace(v)
		}
	}
	return ""
}

func bytesIndex(b []byte, sep string) int {
	return strings.Index(string(b), sep)
}

// edgeSanitize 清洗控制字符 + XML 转义（LLM 回复可能带 markdown 符号，
// 原样朗读即可，仅处理 XML 结构字符）。
func edgeSanitize(s string) string {
	var sb strings.Builder
	for _, r := range s {
		if r < 0x20 && r != '\n' && r != '\t' {
			r = ' '
		}
		sb.WriteRune(r)
	}
	out := sb.String()
	out = strings.ReplaceAll(out, "&", "&amp;")
	out = strings.ReplaceAll(out, "<", "&lt;")
	out = strings.ReplaceAll(out, ">", "&gt;")
	// 换行转句点停顿
	out = strings.ReplaceAll(out, "\n", "。")
	return out
}

// edgeMp3ToPCM16k MP3（Edge 24kHz mono）→ 16k/16bit/mono PCM。
// 解码 go-mp3（纯 Go）；重采样线性插值——语音频带 8kHz 以下，
// 24k→16k 线性插值的失真人耳不可辨，不引重采样库。
func edgeMp3ToPCM16k(mp3Bytes []byte) ([]int16, error) {
	dec, err := mp3.NewDecoder(bytes.NewReader(mp3Bytes))
	if err != nil {
		return nil, fmt.Errorf("edge-tts mp3 decode: %w", err)
	}
	srcRate := dec.SampleRate()

	var raw []byte
	buf := make([]byte, 16384)
	for {
		n, rerr := dec.Read(buf)
		raw = append(raw, buf[:n]...)
		if rerr != nil {
			break // io.EOF 或尾部帧异常：已解码部分可用
		}
	}
	n := len(raw) / 2
	src := make([]int16, n)
	for i := 0; i < n; i++ {
		src[i] = int16(binary.LittleEndian.Uint16(raw[i*2:]))
	}
	if srcRate == 16000 {
		return src, nil
	}
	// go-mp3 对 MPEG-2 LSF（≤24kHz，Edge 24k MP3 即此族）每帧双倍
	// 输出：3.26s 源解出 6.53s 样本，输出实为 2×采样率语义的正确波形
	//（2026-11-15 fixture 实测 + 过零率/时长双验证）。按 2×srcRate
	// 重采样恢复原速；>24k（MPEG-1，如 44.1k/48k）输出正确无需补偿。
	realRate := srcRate
	if srcRate <= 24000 {
		realRate = srcRate * 2
	}
	return resampleTo16k(src, realRate), nil
}

// resampleTo16k 任意采样率 → 16kHz（线性插值）。
func resampleTo16k(src []int16, from int) []int16 {
	if from <= 0 || len(src) == 0 {
		return src
	}
	outLen := len(src) * 16000 / from
	out := make([]int16, outLen)
	for i := 0; i < outLen; i++ {
		pos := float64(i) * float64(from) / 16000.0
		i0 := int(pos)
		if i0 >= len(src)-1 {
			out[i] = src[len(src)-1]
			continue
		}
		frac := pos - float64(i0)
		out[i] = int16(float64(src[i0])*(1-frac) + float64(src[i0+1])*frac)
	}
	return out
}
