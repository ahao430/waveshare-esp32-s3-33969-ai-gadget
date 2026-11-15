// xiaozhi-probe — 官方 xiaozhi.me 接入真网探针（PLAN §5.5 Phase A）。
//
// 流程：加载/生成设备身份 → OTA（打印激活码）→ 等待控制台绑定（轮询
// /activate）→ 绑定后 re-OTA → 用 -wav 发起一轮语音 → 落盘官方 TTS WAV。
//
// 用法：
//
//	go run -tags nolibopusfile ./cmd/xiaozhi-probe \
//	  -db /path/gadget.db -key $BRIDGE_MASTER_KEY \
//	  -wav sample_24k.mp3.wav -out /tmp/xiaozhi_out.wav
//
// -wav 接受 44 字节头的 16k/16bit/mono WAV（voice/testdata 有样例来源）。
package main

import (
	"context"
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"os"
	"time"

	"gadget-bridge/internal/security"
	"gadget-bridge/internal/storage"
	"gadget-bridge/internal/xiaozhi"
)

func main() {
	dbPath := flag.String("db", "", "gadget.db 路径（必填）")
	keyHex := flag.String("key", os.Getenv("BRIDGE_MASTER_KEY"), "主钥 64 hex（或 BRIDGE_MASTER_KEY）")
	wavPath := flag.String("wav", "", "上行音频：16k/16bit/mono WAV（44B 头）")
	outPath := flag.String("out", "/tmp/xiaozhi_out.wav", "官方 TTS 音频落盘路径")
	otaURL := flag.String("ota", "", "OTA 端点覆盖（空=官方）")
	waitBind := flag.Bool("wait-bind", true, "未绑定时轮询等待控制台绑定")
	flag.Parse()

	if *dbPath == "" || *keyHex == "" {
		log.Fatal("需要 -db 与 -key（BRIDGE_MASTER_KEY）")
	}
	masterKey, err := security.ParseMasterKey(*keyHex)
	if err != nil {
		log.Fatalf("key: %v", err)
	}
	db, err := storage.Open(*dbPath)
	if err != nil {
		log.Fatalf("db: %v", err)
	}
	defer db.Close()

	mgr := &xiaozhi.Manager{
		Creds: func() xiaozhi.Credentials {
			c := db.GetXiaozhiCreds(masterKey)
			out := xiaozhi.Credentials{
				DeviceID: c.DeviceID, ClientID: c.ClientID,
				OTAURL: c.OTAURL, WsURL: c.WsURL, WsToken: c.WsToken, Bound: c.Bound,
			}
			if *otaURL != "" {
				out.OTAURL = *otaURL
			}
			return out
		},
		SaveCreds: func(c xiaozhi.Credentials) error {
			if *otaURL != "" && c.OTAURL == *otaURL {
				c.OTAURL = "" // 端点覆盖不落库
			}
			return db.SaveXiaozhiCreds(storage.XiaozhiCreds{
				DeviceID: c.DeviceID, ClientID: c.ClientID, OTAURL: c.OTAURL,
				WsURL: c.WsURL, WsToken: c.WsToken, Bound: c.Bound,
			}, masterKey)
		},
		Log: log.New(os.Stderr, "probe: ", log.LstdFlags|log.Lshortfile),
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	defer cancel()

	// 1) 身份 + OTA
	creds, err := mgr.EnsureIdentity()
	must(err)
	fmt.Printf("设备身份 Device-Id=%s Client-Id=%s\n", creds.DeviceID, creds.ClientID)
	st := mgr.Connect(ctx)
	fmt.Printf("OTA: bound=%v code=%q msg=%s\n", st.Bound, st.ActivationCode, st.Message)

	// 2) 等绑定
	if !st.Bound && *waitBind {
		fmt.Println("→ 请到 https://xiaozhi.me/console 「设备管理」输入上面的激活码；每 3s 查询一次…")
		for i := 0; i < 60; i++ {
			time.Sleep(3 * time.Second)
			if mgr.Activate(ctx) {
				fmt.Println("绑定成功")
				break
			}
			if i%5 == 4 {
				fmt.Println("…仍在等待绑定")
			}
		}
	}
	if !mgr.Available() {
		log.Fatalf("未绑定或不可用（last_error=%s）", mgr.LastError())
	}

	// 3) 语音回合
	if *wavPath == "" {
		fmt.Println("未提供 -wav：连接验证到此完成（身份已保存，Web「小智」页会显示已连接）")
		return
	}
	pcm, err := readWavPCM(*wavPath)
	must(err)
	fmt.Printf("上行 %d 样本（%.1fs）\n", len(pcm)/2, float64(len(pcm)/2)/16000)

	t0 := time.Now()
	var out []int16
	res, err := mgr.AskVoice(ctx, pcm, xiaozhi.TurnCallbacks{
		OnHeard: func(t string) { fmt.Printf("stt: %s\n", t) },
		OnText:  func(s string) { fmt.Printf("tts 字幕: %s\n", s) },
		OnAudio: func(p []int16) { out = append(out, p...) },
	})
	if err != nil {
		log.Fatalf("回合失败: %v", err)
	}
	fmt.Printf("完成：heard=%q reply=%q audio=%dB emotion=%s 耗时=%v\n",
		res.Heard, res.Reply, res.AudioBytes, res.Emotion, time.Since(t0).Round(time.Millisecond))

	if len(out) > 0 {
		w := wrapWav16k(out)
		must(os.WriteFile(*outPath, w, 0o644))
		fmt.Printf("官方 TTS 已落盘 %s（%.1fs）\n", *outPath, float64(len(out))/16000)
	}
}

func must(err error) {
	if err != nil {
		log.Fatal(err)
	}
}

// readWavPCM 读 16k/16bit/mono WAV → 小端 PCM 字节。
func readWavPCM(path string) ([]byte, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(b) < 44 || string(b[0:4]) != "RIFF" || string(b[8:12]) != "WAVE" {
		return nil, fmt.Errorf("want RIFF/WAVE with 44-byte header")
	}
	return b[44:], nil
}

// wrapWav16k 16k mono int16 → WAV。
func wrapWav16k(pcm []int16) []byte {
	dataLen := len(pcm) * 2
	buf := make([]byte, 44+dataLen)
	copy(buf[0:], "RIFF")
	binary.LittleEndian.PutUint32(buf[4:], uint32(36+dataLen))
	copy(buf[8:], "WAVE")
	copy(buf[12:], "fmt ")
	binary.LittleEndian.PutUint32(buf[16:], 16)
	binary.LittleEndian.PutUint16(buf[20:], 1)
	binary.LittleEndian.PutUint16(buf[22:], 1)
	binary.LittleEndian.PutUint32(buf[24:], 16000)
	binary.LittleEndian.PutUint32(buf[28:], 32000)
	binary.LittleEndian.PutUint16(buf[32:], 2)
	binary.LittleEndian.PutUint16(buf[34:], 16)
	copy(buf[36:], "data")
	binary.LittleEndian.PutUint32(buf[40:], uint32(dataLen))
	for i, v := range pcm {
		binary.LittleEndian.PutUint16(buf[44+i*2:], uint16(v))
	}
	return buf
}
