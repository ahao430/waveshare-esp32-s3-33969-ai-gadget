// muse-probe — Muse 连接器真网探针（开发/排障用，不参与服务）。
//
// 用法：
//
//	MUSE_CREDS=/tmp/muse_creds.json go run ./cmd/muse-probe -say "你好"
//	MUSE_CREDS=... go run ./cmd/muse-probe -vms        # 只验证 token/VM 列表
//
// creds 文件是 Phase 0 从配对后设备的 NVS 导出的 JSON：
// {"access_token": "...", "refresh_token": "...", "node_id": "...",
//
//	"noise_host": "...", "api_url_v2": "..."}
//
// 绝不打印 token 明文（只打前缀与长度）。
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"strings"
	"time"

	"gadget-bridge/internal/muse"
)

type creds struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	NodeID       string `json:"node_id"`
	NoiseHost    string `json:"noise_host"`
	APIURL       string `json:"api_url_v2"`
}

type vmInfo struct {
	VMID      string `json:"vm_id"`
	VMWSURL   string `json:"vm_ws_url"`
	VMName    string `json:"vm_name"`
	VMAuthTok string `json:"vm_auth_token"`
	IsDefault bool   `json:"is_default"`
}

func main() {
	credsPath := flag.String("creds", os.Getenv("MUSE_CREDS"), "creds JSON 路径")
	say := flag.String("say", "", "发一条文本消息（空=只测连接）")
	vmsOnly := flag.Bool("vms", false, "只拉 VM 列表")
	proxy := flag.String("proxy", "http://127.0.0.1:7897", "HTTP 代理（空=环境变量）")
	wait := flag.Duration("wait", 60*time.Second, "等回复的最长时间")
	flag.Parse()

	if *credsPath == "" {
		log.Fatal("需要 -creds 或 MUSE_CREDS")
	}
	raw, err := os.ReadFile(*credsPath)
	if err != nil {
		log.Fatalf("读 creds: %v", err)
	}
	var c creds
	if err := json.Unmarshal(raw, &c); err != nil {
		log.Fatalf("解析 creds: %v", err)
	}
	api := strings.TrimRight(c.APIURL, "/")
	if api == "" {
		api = "https://api.muse.ai"
	}
	log.Printf("node_id=%s noise_host=%s api=%s access=%s…(%d)",
		c.NodeID, c.NoiseHost, api, prefix(c.AccessToken, 10), len(c.AccessToken))

	// 1) fetch_vms
	vms, err := fetchVMs(*proxy, api, c.AccessToken)
	if err != nil {
		log.Fatalf("fetch_vms: %v", err)
	}
	if len(vms) == 0 {
		log.Fatal("该账号没有 VM")
	}
	vm := vms[0]
	for _, v := range vms {
		if v.IsDefault {
			vm = v
		}
	}
	log.Printf("VM: id=%s ws=%s token=%s…(%d)", vm.VMID, vm.VMWSURL,
		prefix(vm.VMAuthTok, 12), len(vm.VMAuthTok))
	if *vmsOnly {
		return
	}

	// 2) Noise 会话
	ctx, cancel := context.WithTimeout(context.Background(), *wait+30*time.Second)
	defer cancel()
	conn, err := muse.Dial(ctx, muse.Config{
		NoiseHost: c.NoiseHost, VMID: vm.VMID, Token: vm.VMAuthTok,
		ProxyURL: *proxy,
	})
	if err != nil {
		log.Fatalf("Noise: %v", err)
	}
	defer conn.Close()
	log.Printf("Noise 会话已建立")

	// 3) 订阅
	sub, err := conn.Open("POST", "/chat/subscribe",
		[]muse.Header{{Key: "Content-Type", Value: "application/json"},
			{Key: "Accept", Value: "application/x-ndjson"}},
		[]byte("{}"), true)
	if err != nil {
		log.Fatalf("subscribe: %v", err)
	}
	log.Printf("已打开 /chat/subscribe")

	// 后台打印订阅事件
	go func() {
		for ev := range sub.Events() {
			switch ev.Kind {
			case muse.EvStatus:
				log.Printf("sub status=%d", ev.Status)
			case muse.EvBody:
				for _, line := range strings.Split(strings.TrimSpace(string(ev.Body)), "\n") {
					if line != "" {
						log.Printf("sub: %s", truncate(line, 300))
					}
				}
			case muse.EvEnd:
				log.Printf("sub ended")
			case muse.EvReset:
				log.Printf("sub reset code=%d %s", ev.Reset, ev.Reason)
			}
		}
	}()

	if *say == "" {
		select {
		case <-time.After(5 * time.Second):
		case <-conn.Done():
			log.Printf("会话结束: %v", conn.Err())
		}
		return
	}

	// 4) 发文本回合
	body, _ := json.Marshal(map[string]string{
		"message":         *say,
		"output_modality": "text",
		"device":          "mcu-wearable",
	})
	chat, err := conn.Open("POST", "/chat/stream",
		[]muse.Header{{Key: "Content-Type", Value: "application/json"}}, body, true)
	if err != nil {
		log.Fatalf("chat/stream: %v", err)
	}
	st, hdrs, resp, err := chat.Response(ctx)
	log.Printf("chat/stream status=%d headers=%v err=%v body=%s", st, hdrs, err, truncate(string(resp), 300))

	// 等订阅侧回复
	deadline := time.After(*wait)
	select {
	case <-deadline:
		log.Printf("等待超时")
	case <-conn.Done():
		log.Printf("会话结束: %v", conn.Err())
	}
}

func prefix(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "…"
}

// fetchVMs GET {api}/fetch_vms（Bearer access token）。
func fetchVMs(proxyURL, api, token string) ([]vmInfo, error) {
	var out struct {
		VMList []vmInfo `json:"vm_list"`
	}
	body, err := httpGetJSON(proxyURL, api+"/fetch_vms", token)
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(body, &out); err != nil {
		return nil, fmt.Errorf("解析: %w (%.200s)", err, string(body))
	}
	return out.VMList, nil
}
