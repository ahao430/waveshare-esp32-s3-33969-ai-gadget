package main

import (
	"context"
	"fmt"
	"os"
	"strings"

	"gadget-bridge/internal/ai"
	"gadget-bridge/internal/security"
	"gadget-bridge/internal/storage"
)

func main() {
	mk, err := security.ParseMasterKey(os.Getenv("BRIDGE_MASTER_KEY"))
	if err != nil { panic(err) }
	db, err := storage.Open("data/gadget.db")
	if err != nil { panic(err) }
	id, base, tok, cfg, prov, model, err := db.GetBot("bot-9cdde300", mk)
	if err != nil { panic(err) }
	fmt.Printf("id=%s base=%s token_len=%d token_prefix=%s cfg=%s prov=%s model=%s\n",
		id, base, len(tok), tok[:min(8,len(tok))], cfg, prov, model)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	reply, err := ai.ProbeAstrBot(ctx, id, base, tok, cfg, prov, model)
	fmt.Printf("probe reply=%q err=%v\n", reply, err)
}

func min(a, b int) int { if a < b { return a }; return b }
var _ = strings.TrimSpace
